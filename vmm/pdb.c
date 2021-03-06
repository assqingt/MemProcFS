// pdb.h : implementation related to parsing of program databases (PDB) files
//         used for debug symbols and automatic retrieval from the Microsoft
//         Symbol Server. (Windows exclusive functionality).
//
// (c) Ulf Frisk, 2019
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "pdb.h"
#include "pe.h"
#include "util.h"
#include "vmmwininit.h"
#include <dbghelp.h>
#include <winreg.h>
#include <io.h>

#define VMMWIN_PDB_LOAD_ADDRESS_STEP    0x10000000;
#define VMMWIN_PDB_LOAD_ADDRESS_BASE    0x0000511f'00000000;
#define VMMWIN_PDB_FAKEPROCHANDLE       (HANDLE)0x00005fed'6fed7fed
#define VMMWIN_PDB_WARN_DEFAULT         "WARNING: Functionality may be limited. Extended debug information disabled.\n"

typedef struct tdPDB_ENTRY {
    OB ObHdr;
    QWORD qwHash;
    QWORD vaModuleBase;
    LPSTR szModuleName;
    LPSTR szName;
    BYTE pbGUID[16];
    DWORD dwAge;
    // load data below
    BOOL fLoadFailed;
    LPSTR szPath;
    QWORD qwLoadAddress;
} PDB_ENTRY, *PPDB_ENTRY;

const LPSTR szVMMWIN_PDB_FUNCTIONS[] = {
    "SymGetOptions",
    "SymSetOptions",
    "SymInitialize",
    "SymCleanup",
    "SymFindFileInPath",
    "SymLoadModuleEx",
    "SymUnloadModule64",
    "SymEnumSymbols",
    "SymEnumTypesByName",
    "SymGetTypeInfo",
};

typedef struct tdVMMWIN_PDB_FUNCTIONS {
    DWORD(*SymGetOptions)(VOID);
    DWORD(*SymSetOptions)(_In_ DWORD SymOptions);
    BOOL(*SymInitialize)(_In_ HANDLE hProcess, _In_opt_ PCSTR UserSearchPath, _In_ BOOL fInvadeProcess);
    BOOL(*SymCleanup)(_In_ HANDLE hProcess);
    BOOL(*SymFindFileInPath)(_In_ HANDLE hprocess, _In_opt_ PCSTR SearchPath, _In_ PCSTR FileName, _In_opt_ PVOID id, _In_ DWORD two, _In_ DWORD three, _In_ DWORD flags, _Out_writes_(MAX_PATH + 1) PSTR FoundFile, _In_opt_ PFINDFILEINPATHCALLBACK callback, _In_opt_ PVOID context);
    DWORD64(*SymLoadModuleEx)(_In_ HANDLE hProcess, _In_opt_ HANDLE hFile, _In_opt_ PCSTR ImageName, _In_opt_ PCSTR ModuleName, _In_ DWORD64 BaseOfDll, _In_ DWORD DllSize, _In_opt_ PMODLOAD_DATA Data, _In_opt_ DWORD Flags);
    BOOL(*SymUnloadModule64)(_In_ HANDLE hProcess, _In_ DWORD64 BaseOfDll);
    BOOL(*SymEnumSymbols)(_In_ HANDLE hProcess, _In_ ULONG64 BaseOfDll, _In_opt_ PCSTR Mask, _In_ PSYM_ENUMERATESYMBOLS_CALLBACK EnumSymbolsCallback, _In_opt_ PVOID UserContext);
    BOOL(*SymEnumTypesByName)(_In_ HANDLE hProcess, _In_ ULONG64 BaseOfDll, _In_opt_ PCSTR mask, _In_ PSYM_ENUMERATESYMBOLS_CALLBACK EnumSymbolsCallback, _In_opt_ PVOID UserContext);
    BOOL(*SymGetTypeInfo)(_In_ HANDLE hProcess, _In_ DWORD64 ModBase, _In_ ULONG TypeId, _In_ IMAGEHLP_SYMBOL_TYPE_INFO GetType, _Out_ PVOID pInfo);
} VMMWIN_PDB_FUNCTIONS;

typedef struct tdVMMWIN_PDB_CONTEXT {
    BOOL fDisabled;
    HANDLE hSym;
    HMODULE hModuleSymSrv;
    HMODULE hModuleDbgHelp;
    CRITICAL_SECTION Lock;
    POB_MAP pmPdbByHash;
    POB_MAP pmPdbByModule;
    QWORD qwLoadAddressNext;
    union {
        VMMWIN_PDB_FUNCTIONS pfn;
        QWORD vafn[sizeof(VMMWIN_PDB_FUNCTIONS) / sizeof(PVOID)];
    };
} VMMWIN_PDB_CONTEXT, *PVMMWIN_PDB_CONTEXT;

typedef struct tdVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS {
    PHANDLE phEventThreadStarted;
    BOOL fPdbInfo;
    IMAGE_DEBUG_TYPE_CODEVIEW_PDBINFO PdbInfo;
} VMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS, *PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS;

QWORD PDB_HashPdb(_In_ LPSTR szPdbName, _In_reads_(16) PBYTE pbPdbGUID, _In_ DWORD dwPdbAge)
{
    QWORD qwHash = 0;
    qwHash = Util_HashStringA(szPdbName);
    qwHash = dwPdbAge + ((qwHash >> 13) | (qwHash << 51));
    qwHash = *(PQWORD)pbPdbGUID + ((qwHash >> 13) | (qwHash << 51));
    qwHash = *(PQWORD)(pbPdbGUID + 8) + ((qwHash >> 13) | (qwHash << 51));
    return qwHash;
}

DWORD PDB_HashModuleName(_In_ LPSTR szModuleName)
{
    DWORD i, c, dwHash = 0;
    WCHAR wszBuffer[MAX_PATH];
    c = Util_PathFileNameFix_Registry(wszBuffer, szModuleName, NULL, 0, 0, TRUE);
    for(i = 0; i < c; i++) {
        dwHash = ((dwHash >> 13) | (dwHash << 19)) + wszBuffer[i];
    }
    return dwHash;
}

VOID PDB_CallbackCleanup_ObPdbEntry(PPDB_ENTRY pOb)
{
    LocalFree(pOb->szModuleName);
    LocalFree(pOb->szName);
    LocalFree(pOb->szPath);
}

/*
* Add a module to the PDB database and return its handle. The PDB for the added
* module won't be loaded until required. If the module already exists in the
* PDB database the handle will also be returned.
* -- vaModuleBase
* -- szModuleName
* -- szPdbName
* -- pbPdbGUID
* -- dwPdbAge
* -- return
*/
VMMWIN_PDB_HANDLE PDB_AddModuleEntry(_In_ QWORD vaModuleBase, _In_ LPSTR szModuleName, _In_ LPSTR szPdbName, _In_reads_(16) PBYTE pbPdbGUID, _In_ DWORD dwPdbAge)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry;
    QWORD qwPdbHash;
    if(!ctx) { return 0; }
    qwPdbHash = PDB_HashPdb(szPdbName, pbPdbGUID, dwPdbAge);
    if(!ObMap_ExistsKey(ctx->pmPdbByHash, qwPdbHash)) {
        pObPdbEntry = Ob_Alloc(OB_TAG_PDB_ENTRY, LMEM_ZEROINIT, sizeof(PDB_ENTRY), PDB_CallbackCleanup_ObPdbEntry, NULL);
        if(!pObPdbEntry) { return 0; }
        pObPdbEntry->dwAge = dwPdbAge;
        pObPdbEntry->qwHash = qwPdbHash;
        memcpy(pObPdbEntry->pbGUID, pbPdbGUID, 16);
        pObPdbEntry->szName = Util_StrDupA(szPdbName);
        pObPdbEntry->szModuleName = Util_StrDupA(szModuleName);
        pObPdbEntry->vaModuleBase = vaModuleBase;
        ObMap_Push(ctx->pmPdbByHash, qwPdbHash, pObPdbEntry);
        ObMap_Push(ctx->pmPdbByModule, PDB_HashModuleName(szModuleName), pObPdbEntry);
        Ob_DECREF(pObPdbEntry);
    }
    return qwPdbHash;
}

/*
* Retrieve a PDB handle from an already added module.
* NB! If multiple modules exists with the same name the 1st module to be added
*     is returned.
* -- szModuleName
* -- return
*/
VMMWIN_PDB_HANDLE PDB_GetHandleFromModuleName(_In_ LPSTR szModuleName)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry;
    QWORD qwHashPdb;
    DWORD dwHashModule;
    if(!ctx) { return 0; }
    if(!szModuleName || !strcmp("nt", szModuleName)) {
        szModuleName = "ntoskrnl.exe";
    }
    dwHashModule = PDB_HashModuleName(szModuleName);
    pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByModule, dwHashModule);
    if(!pObPdbEntry) { return 0; }
    qwHashPdb = pObPdbEntry->qwHash;
    Ob_DECREF(pObPdbEntry);
    return qwHashPdb;
}

/*
* Ensure that the PDB_ENTRY have its symbols loaded into memory - symbols are
* loaded if required.
* NB! this function must be called in a single-threaded context!
* -- pPdbEntry
* -- return
*/
_Success_(return)
BOOL PDB_PdbLoadEnsure(_In_ PPDB_ENTRY pPdbEntry)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    CHAR szPdbPath[MAX_PATH + 1];
    if(!ctx || pPdbEntry->fLoadFailed) { return FALSE; }
    if(pPdbEntry->qwLoadAddress) { return TRUE; }
    if(!ctx->pfn.SymFindFileInPath(ctx->hSym, NULL, pPdbEntry->szName, &pPdbEntry->pbGUID, pPdbEntry->dwAge, 0, SSRVOPT_GUIDPTR, szPdbPath, NULL, NULL)) { goto fail; }
    pPdbEntry->szPath = Util_StrDupA(szPdbPath);
    pPdbEntry->qwLoadAddress = ctx->pfn.SymLoadModuleEx(ctx->hSym, NULL, szPdbPath, NULL, ctx->qwLoadAddressNext, 0, NULL, 0);
    ctx->qwLoadAddressNext += VMMWIN_PDB_LOAD_ADDRESS_STEP;
    if(!pPdbEntry->szPath || !pPdbEntry->qwLoadAddress) { goto fail; }
    return TRUE;
fail:
    pPdbEntry->fLoadFailed = TRUE;
    return FALSE;
}

/*
* Callback function for PDB_GetSymbolOffset() / SymEnumSymbols()
*/
BOOL PDB_GetSymbolOffset_Callback(_In_ PSYMBOL_INFO pSymInfo, _In_ ULONG SymbolSize, _In_ PDWORD pdwSymbolOffset)
{
    if(pSymInfo->Address - pSymInfo->ModBase < 0x10000000) {
        *pdwSymbolOffset = (DWORD)(pSymInfo->Address - pSymInfo->ModBase);
    }
    return FALSE;
}

/*
* Query the PDB for the offset of a symbol. If szSymbolName contains wildcard
* '?*' characters and matches multiple symbols the offset of the 1st symbol is
* returned.
* -- hPDB
* -- szSymbolName = wildcard symbol name
* -- pdwSymbolOffset
* -- return
*/
_Success_(return)
BOOL PDB_GetSymbolOffset(_In_opt_ VMMWIN_PDB_HANDLE hPDB, _In_ LPSTR szSymbolName, _Out_ PDWORD pdwSymbolOffset)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry = NULL;
    BOOL fResult = FALSE;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == VMMWIN_PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl.exe"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_PdbLoadEnsure(pObPdbEntry)) { goto fail; }
    *pdwSymbolOffset = 0;
    if(!ctx->pfn.SymEnumSymbols(ctx->hSym, pObPdbEntry->qwLoadAddress, szSymbolName, PDB_GetSymbolOffset_Callback, pdwSymbolOffset)) { goto fail; }
    if(!*pdwSymbolOffset) { goto fail; }
    fResult = TRUE;
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

/*
* Query the PDB for the offset of a symbol and return its virtual address. If
* szSymbolName contains wildcard '?*' characters and matches multiple symbols
* the virtual address of the 1st symbol is returned.
* -- hPDB
* -- szSymbolName = wildcard symbol name
* -- pvaSymbolOffset
* -- return
*/
_Success_(return)
BOOL PDB_GetSymbolAddress(_In_opt_ VMMWIN_PDB_HANDLE hPDB, _In_ LPSTR szSymbolName, _Out_ PQWORD pvaSymbolOffset)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry = NULL;
    DWORD cbSymbolOffset;
    BOOL fResult = FALSE;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == VMMWIN_PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl.exe"); }
    if(!PDB_GetSymbolOffset(hPDB, szSymbolName, &cbSymbolOffset)) { return FALSE; }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    *pvaSymbolOffset = pObPdbEntry->vaModuleBase + cbSymbolOffset;
    Ob_DECREF(pObPdbEntry);
    return TRUE;
}

/*
* Read memory at the PDB acquired symbol offset. If szSymbolName contains
* wildcard '?*' characters and matches multiple symbols the offset of the
* 1st symbol is used to read the memory.
* -- hPDB
* -- szSymbolName = wildcard symbol name
* -- pProcess
* -- pb
* -- cb
* -- return
*/
_Success_(return)
BOOL PDB_GetSymbolPBYTE(_In_opt_ VMMWIN_PDB_HANDLE hPDB, _In_ LPSTR szSymbolName, _In_ PVMM_PROCESS pProcess, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry = NULL;
    DWORD dwSymbolOffset;
    BOOL fResult;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == VMMWIN_PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl.exe"); }
    if(!PDB_GetSymbolOffset(hPDB, szSymbolName, &dwSymbolOffset)) { return FALSE; }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    fResult = VmmRead(pProcess, pObPdbEntry->vaModuleBase + dwSymbolOffset, pb, cb);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

/*
* Callback function for PDB_GetTypeSize() / SymEnumTypesByName()
*/
BOOL PDB_GetTypeSize_Callback(_In_ PSYMBOL_INFO pSymInfo, _In_ ULONG SymbolSize, _In_ PDWORD pdwTypeSize)
{
    *pdwTypeSize = pSymInfo->Size;
    return FALSE;
}

/*
* Query the PDB for the size of a type. If szTypeName contains wildcard '?*'
* characters and matches multiple types the size of the 1st type is returned.
* -- hPDB
* -- szTypeName = wildcard type name
* -- pdwTypeSize
* -- return
*/
_Success_(return)
BOOL PDB_GetTypeSize(_In_opt_ VMMWIN_PDB_HANDLE hPDB, _In_ LPSTR szTypeName, _Out_ PDWORD pdwTypeSize)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry = NULL;
    BOOL fResult = FALSE;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == VMMWIN_PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl.exe"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_PdbLoadEnsure(pObPdbEntry)) { goto fail; }
    *pdwTypeSize = 0;
    if(!ctx->pfn.SymEnumTypesByName(ctx->hSym, pObPdbEntry->qwLoadAddress, szTypeName, PDB_GetTypeSize_Callback, pdwTypeSize)) { goto fail; }
    if(!*pdwTypeSize) { goto fail; }
    fResult = TRUE;
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

/*
* Callback function for PDB_GetTypeChildOffset()
*/
BOOL PDB_GetTypeChildOffset_Callback(_In_ PSYMBOL_INFO pSymInfo, _In_ ULONG SymbolSize, _In_ PDWORD pdwTypeId)
{
    *pdwTypeId = pSymInfo->Index;
    pSymInfo->Index;
    return FALSE;
}

/*
* Query the PDB for the offset of a child inside a type - often inside a struct.
* If szTypeName contains wildcard '?*' characters and matches multiple types the
* first type is queried for children. The child name must match exactly.
* -- hPDB
* -- szTypeName = wildcard type name.
* -- wszTypeChildName = exact match of child name.
* -- pdwTypeOffset = offset relative to type base.
* -- return
*/
_Success_(return)
BOOL PDB_GetTypeChildOffset(_In_opt_ VMMWIN_PDB_HANDLE hPDB, _In_ LPSTR szTypeName, _In_ LPWSTR wszTypeChildName, _Out_ PDWORD pdwTypeOffset)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    BOOL fResult = FALSE;
    LPWSTR wszTypeChildSymName;
    PPDB_ENTRY pObPdbEntry = NULL;
    DWORD dwTypeId, cTypeChildren, iTypeChild;
    TI_FINDCHILDREN_PARAMS *pFindChildren = NULL;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == VMMWIN_PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl.exe"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_PdbLoadEnsure(pObPdbEntry)) { goto fail; }
    if(!ctx->pfn.SymEnumTypesByName(ctx->hSym, pObPdbEntry->qwLoadAddress, szTypeName, PDB_GetTypeChildOffset_Callback, &dwTypeId) || !dwTypeId) { goto fail; }
    if(!ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, dwTypeId, TI_GET_CHILDRENCOUNT, &cTypeChildren) || !cTypeChildren) { goto fail; }
    if(!(pFindChildren = LocalAlloc(LMEM_ZEROINIT, sizeof(TI_FINDCHILDREN_PARAMS) + cTypeChildren * sizeof(ULONG)))) { goto fail; }
    pFindChildren->Count = cTypeChildren;
    if(!ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, dwTypeId, TI_FINDCHILDREN, pFindChildren)) { goto fail; }
    for(iTypeChild = 0; iTypeChild < cTypeChildren; iTypeChild++) {
        if(!ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, pFindChildren->ChildId[iTypeChild], TI_GET_SYMNAME, &wszTypeChildSymName)) { continue; }
        if(!wcscmp(wszTypeChildName, wszTypeChildSymName)) {
            if(ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, pFindChildren->ChildId[iTypeChild], TI_GET_OFFSET, pdwTypeOffset)) {
                LocalFree(wszTypeChildSymName);
                fResult = TRUE;
                break;
            }
        }
        LocalFree(wszTypeChildSymName);
    }
fail:
    LocalFree(pFindChildren);
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

_Success_(return)
BOOL PDB_GetTypeChildOffsetShort(_In_opt_ VMMWIN_PDB_HANDLE hPDB, _In_ LPSTR szTypeName, _In_ LPWSTR wszTypeChildName, _Out_ PWORD pwTypeOffset)
{
    DWORD dwTypeOffset;
    if(!PDB_GetTypeChildOffset(hPDB, szTypeName, wszTypeChildName, &dwTypeOffset) || (dwTypeOffset > 0xffff)) { return FALSE; }
    if(pwTypeOffset) { *pwTypeOffset = (WORD)dwTypeOffset; }
    return TRUE;
}



//-----------------------------------------------------------------------------
// INITIALIZATION/REFRESH/CLOSE FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

/*
* Cleanup the PDB sub-system. This should ideally be done on Vmm Close().
*/
VOID PDB_Close()
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    if(!ctx) { return; }
    ctxVmm->pPdbContext = NULL;
    EnterCriticalSection(&ctx->Lock);
    LeaveCriticalSection(&ctx->Lock);
    DeleteCriticalSection(&ctx->Lock);
    if(ctx->hSym) {
        ctx->pfn.SymCleanup(ctx->hSym);
    }
    Ob_DECREF(ctx->pmPdbByHash);
    Ob_DECREF(ctx->pmPdbByModule);
    if(ctx->hModuleDbgHelp) { FreeLibrary(ctx->hModuleDbgHelp); }
    if(ctx->hModuleSymSrv) { FreeLibrary(ctx->hModuleSymSrv); }
    ZeroMemory(ctx, sizeof(VMMWIN_PDB_CONTEXT));
    ctxMain->pdb.fInitialized = FALSE;
}

/*
* 
*/
_Success_(return)
BOOL PDB_Initialize_Async_Kernel_ScanForPdbInfo(_In_ PVMM_PROCESS pSystemProcess, _Out_writes_(MAX_PATH) LPSTR szPdbName, _Out_writes_(16) PBYTE pbGUID, _Out_ PDWORD pdwAge)
{
    PBYTE pb = NULL;
    DWORD i, cbRead;
    PIMAGE_DEBUG_TYPE_CODEVIEW_PDBINFO pPdb;
    if(!ctxVmm->kernel.vaBase) { return FALSE; }
    if(!(pb = LocalAlloc(0, 0x00800000))) { return FALSE; }
    VmmReadEx(pSystemProcess, ctxVmm->kernel.vaBase, pb, 0x00800000, &cbRead, VMM_FLAG_ZEROPAD_ON_FAIL);
    // 1: search for pdb debug information adn extract offset of PsInitialSystemProcess
    for(i = 0; i < 0x00800000 - sizeof(IMAGE_DEBUG_TYPE_CODEVIEW_PDBINFO); i += 4) {
        pPdb = (PIMAGE_DEBUG_TYPE_CODEVIEW_PDBINFO)(pb + i);
        if(pPdb->Signature == 0x53445352) {
            if(pPdb->Age > 0x20) { continue; }
            if(memcmp("nt", pPdb->PdbFileName, 2)) { continue; }
            if(memcmp(".pdb", pPdb->PdbFileName + 8, 5)) { continue; }
            *pdwAge = pPdb->Age;
            memcpy(pbGUID, pPdb->Guid, 16);
            strncpy_s(szPdbName, MAX_PATH, pPdb->PdbFileName, sizeof(pPdb->PdbFileName));
            LocalFree(pb);
            return TRUE;
        }
    }
    LocalFree(pb);
    return FALSE;
}

VOID PDB_Initialize_WaitComplete()
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    if(ctx && ctxMain->pdb.fEnable) {
        EnterCriticalSection(&ctx->Lock);
        LeaveCriticalSection(&ctx->Lock);
    }
}

/*
* Asynchronous initialization of the PDB for the kernel. This is done async
* since it may take some time to load the PDB from the Microsoft Symbol server.
* Once this initializtion is successfully completed the fDisabled flag will be
* removed - allowing other threads to use the PDB subsystem.
* If the initialization fails it's assume the PDB system should be disabled.
* -- hEventThreadStart
* -- return
*/
DWORD PDB_Initialize_Async_Kernel(LPVOID lpParameter)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS pKernelParameters = (PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS)lpParameter;
    DWORD dwReturnStatus = 0, dwPdbAge;
    BYTE pbPdbGUID[16];
    CHAR szPdbName[MAX_PATH] = { 0 };
    PVMM_PROCESS pObSystemProcess = NULL;
    PPDB_ENTRY pObKernelEntry = NULL;
    QWORD qwPdbHash;
    if(!ctx) { return 0; }
    EnterCriticalSection(&ctx->Lock);
    InterlockedIncrement(&ctxVmm->ThreadWorkers.c);
    SetEvent(*pKernelParameters->phEventThreadStarted);
    if(!(pObSystemProcess = VmmProcessGet(4))) { goto fail; }
    if(pKernelParameters->fPdbInfo) {
        dwPdbAge = pKernelParameters->PdbInfo.Age;
        memcpy(pbPdbGUID, pKernelParameters->PdbInfo.Guid, 16);
        memcpy(szPdbName, pKernelParameters->PdbInfo.PdbFileName, sizeof(pKernelParameters->PdbInfo.PdbFileName));
    } else if(!PE_GetPdbInfo(pObSystemProcess, ctxVmm->kernel.vaBase, NULL, szPdbName, pbPdbGUID, &dwPdbAge) && !PDB_Initialize_Async_Kernel_ScanForPdbInfo(pObSystemProcess, szPdbName, pbPdbGUID, &dwPdbAge)) {
        vmmprintf("%s         Reason: Unable to locate debugging information in kernel image.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    qwPdbHash = PDB_AddModuleEntry(ctxVmm->kernel.vaBase, "ntoskrnl.exe", szPdbName, pbPdbGUID, dwPdbAge);
    pObKernelEntry = ObMap_GetByKey(ctx->pmPdbByHash, qwPdbHash);
    if(!pObKernelEntry) {
        vmmprintf("%s         Reason: Failed creating initial PDB entry.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    if(!PDB_PdbLoadEnsure(pObKernelEntry)) {
        vmmprintf("%s         Reason: Unable to download kernel symbols to cache from Symbol Server.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    vmmprintfvv_fn("Initialization of debug symbol .pdb functionality completed.\n    [ %s ]\n", ctxMain->pdb.szSymbolPath);
    ctx->fDisabled = FALSE;
    dwReturnStatus = 1;
    // fall-through to fail for cleanup
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObKernelEntry);
    Ob_DECREF(pObSystemProcess);
    InterlockedDecrement(&ctxVmm->ThreadWorkers.c);
    LocalFree(pKernelParameters);
    return dwReturnStatus;
}

VOID PDB_Initialize_InitialValues()
{
    HKEY hKey;
    DWORD cbData, dwEnableSymbols, dwEnableSymbolServer;
    // 1: try load values from registry
    if(!ctxMain->pdb.fInitialized) {
        ctxMain->pdb.fEnable = 1;
        ctxMain->pdb.fServerEnable = !ctxMain->cfg.fDisableSymbolServerOnStartup;
    }
    ctxMain->pdb.szLocal[0] = 0;
    ctxMain->pdb.szServer[0] = 0;
    dwEnableSymbols = ctxMain->pdb.fEnable ? 1 : 0;
    dwEnableSymbolServer = ctxMain->pdb.fServerEnable ? 1 : 0;
    if(ERROR_SUCCESS == RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\UlfFrisk\\MemProcFS", 0, KEY_READ, &hKey)) {
        cbData = _countof(ctxMain->pdb.szLocal) - 1;
        RegQueryValueExA(hKey, "SymbolCache", NULL, NULL, (PBYTE)ctxMain->pdb.szLocal, &cbData);
        if(cbData < 3) { ctxMain->pdb.szLocal[0] = 0; }
        cbData = _countof(ctxMain->pdb.szServer) - 1;
        RegQueryValueExA(hKey, "SymbolServer", NULL, NULL, (PBYTE)ctxMain->pdb.szServer, &cbData);
        if(cbData < 3) { ctxMain->pdb.szServer[0] = 0; }
        if(ctxMain->pdb.fEnable) {
            cbData = sizeof(DWORD);
            RegQueryValueExA(hKey, "SymbolEnable", NULL, NULL, (PBYTE)&dwEnableSymbols, &cbData);
        }
        if(ctxMain->pdb.fServerEnable) {
            cbData = sizeof(DWORD);
            RegQueryValueExA(hKey, "SymbolServerEnable", NULL, NULL, (PBYTE)&dwEnableSymbolServer, &cbData);
        }
        RegCloseKey(hKey);
    }
    // 2: set default values (if not already loaded from registry)
    if(!ctxMain->pdb.szLocal[0]) {
        Util_GetPathDll(ctxMain->pdb.szLocal, ctxVmm->hModuleVmm);
        strncat_s(ctxMain->pdb.szLocal, _countof(ctxMain->pdb.szLocal), "Symbols", _TRUNCATE);
    }
    if(!ctxMain->pdb.szServer[0]) {
        strncpy_s(ctxMain->pdb.szServer, _countof(ctxMain->pdb.szServer), "https://msdl.microsoft.com/download/symbols", _TRUNCATE);
    }
    // 3: set final values
    ctxMain->pdb.fEnable = (dwEnableSymbols == 1);
    ctxMain->pdb.fServerEnable = (dwEnableSymbolServer == 1);
    strncpy_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), "srv*", _TRUNCATE);
    strncat_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), ctxMain->pdb.szLocal, _TRUNCATE);
    if(ctxMain->pdb.fServerEnable) {
        strncat_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), "*", _TRUNCATE);
        strncat_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), ctxMain->pdb.szServer, _TRUNCATE);
    }
    ctxMain->pdb.fInitialized = TRUE;
}

/*
* Update the PDB configuration. The PDB syb-system will be reloaded on
* configuration changes - which may cause a short interruption for any
* caller.
*/
VOID PDB_ConfigChange()
{
    HKEY hKey;
    CHAR szLocalPath[MAX_PATH] = { 0 };
    // update new values in registry
    if(ERROR_SUCCESS == RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\UlfFrisk\\MemProcFS", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL)) {
        Util_GetPathDll(szLocalPath, ctxVmm->hModuleVmm);
        if(strncmp(szLocalPath, ctxMain->pdb.szLocal, strlen(szLocalPath) - 1) && !_access_s(ctxMain->pdb.szLocal, 06)) {
            RegSetValueExA(hKey, "SymbolCache", 0, REG_SZ, (PBYTE)ctxMain->pdb.szLocal, (DWORD)strlen(ctxMain->pdb.szLocal));
        } else {
            RegSetValueExA(hKey, "SymbolCache", 0, REG_SZ, (PBYTE)"", 0);
        }
        if((!strncmp("http://", ctxMain->pdb.szServer, 7) || !strncmp("https://", ctxMain->pdb.szServer, 8)) && !strstr(ctxMain->pdb.szServer, "msdl.microsoft.com")) {
            RegSetValueExA(hKey, "SymbolServer", 0, REG_SZ, (PBYTE)ctxMain->pdb.szServer, (DWORD)strlen(ctxMain->pdb.szServer));
        } else {
            RegSetValueExA(hKey, "SymbolServer", 0, REG_SZ, (PBYTE)"", 0);
        }
        RegCloseKey(hKey);
    }
    // refresh values and reload!
    EnterCriticalSection(&ctxVmm->MasterLock);
    PDB_Close();
    PDB_Initialize(NULL, FALSE);
    LeaveCriticalSection(&ctxVmm->MasterLock);
}

/*
* Initialize the PDB sub-system. This should ideally be done on Vmm Init()
*/
VOID PDB_Initialize(_In_opt_ PIMAGE_DEBUG_TYPE_CODEVIEW_PDBINFO pPdbInfoOpt, _In_ BOOL fInitializeKernelAsync)
{
    HANDLE hThreadAsyncKernel, hEventThreadStarted = 0;
    PVMMWIN_PDB_CONTEXT ctx = NULL;
    DWORD i, dwSymOptions;
    CHAR szPathSymSrv[MAX_PATH], szPathDbgHelp[MAX_PATH];
    PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS pKernelParameters = NULL;
    if(ctxMain->pdb.fInitialized) { return; }
    PDB_Initialize_InitialValues();
    if(!ctxMain->pdb.fEnable) { goto fail; }
    if(!(ctx = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMWIN_PDB_CONTEXT)))) { goto fail; }
    if(!(ctx->pmPdbByHash = ObMap_New(OB_MAP_FLAGS_OBJECT_OB))) { goto fail; }
    if(!(ctx->pmPdbByModule = ObMap_New(OB_MAP_FLAGS_OBJECT_OB))) { goto fail; }
    // 1: dynamic load of dbghelp.dll and symsrv.dll from directory of vmm.dll - i.e. not from system32
    Util_GetPathDll(szPathSymSrv, ctxVmm->hModuleVmm);
    Util_GetPathDll(szPathDbgHelp, ctxVmm->hModuleVmm);
    strncat_s(szPathSymSrv, MAX_PATH, "symsrv.dll", _TRUNCATE);
    strncat_s(szPathDbgHelp, MAX_PATH, "dbghelp.dll", _TRUNCATE);
    ctx->hModuleSymSrv = LoadLibraryA(szPathSymSrv);
    ctx->hModuleDbgHelp = LoadLibraryA(szPathDbgHelp);
    if(!ctx->hModuleSymSrv || !ctx->hModuleDbgHelp) {
        vmmprintf("%s         Reason: Could not load PDB required files - symsrv.dll/dbghelp.dll.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    for(i = 0; i < sizeof(VMMWIN_PDB_FUNCTIONS) / sizeof(PVOID); i++) {
        ctx->vafn[i] = (QWORD)GetProcAddress(ctx->hModuleDbgHelp, szVMMWIN_PDB_FUNCTIONS[i]);
        if(!ctx->vafn[i]) {
            vmmprintf("%s         Reason: Could not load function(s) from symsrv.dll/dbghelp.dll.\n", VMMWIN_PDB_WARN_DEFAULT);
            goto fail;
        }
    }
    // 2: initialize dbghelp.dll
    ctx->hSym = VMMWIN_PDB_FAKEPROCHANDLE;
    dwSymOptions = ctx->pfn.SymGetOptions();
    dwSymOptions &= ~SYMOPT_DEFERRED_LOADS;
    dwSymOptions &= ~SYMOPT_LOAD_LINES;
    dwSymOptions |= SYMOPT_CASE_INSENSITIVE;
    dwSymOptions |= SYMOPT_IGNORE_NT_SYMPATH;
    dwSymOptions |= SYMOPT_UNDNAME;
    ctx->pfn.SymSetOptions(dwSymOptions);
    if(!ctx->pfn.SymInitialize(ctx->hSym, ctxMain->pdb.szSymbolPath, FALSE)) {
        vmmprintf("%s         Reason: Failed to initialize Symbol Handler / dbghelp.dll.\n", VMMWIN_PDB_WARN_DEFAULT);
        ctx->hSym = NULL;
        goto fail;
    }
    // success - finish up and load kernel .pdb async (to optimize startup time).
    // pdb subsystem won't be fully initialized until before the kernel is loaded.
    if(!(hEventThreadStarted = CreateEvent(NULL, TRUE, FALSE, NULL))) { goto fail; }
    if(!(pKernelParameters = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS)))) { goto fail; }
    pKernelParameters->phEventThreadStarted = &hEventThreadStarted;
    if(pPdbInfoOpt) {
        pKernelParameters->fPdbInfo = TRUE;
        memcpy(&pKernelParameters->PdbInfo, pPdbInfoOpt, sizeof(IMAGE_DEBUG_TYPE_CODEVIEW_PDBINFO));
    }
    InitializeCriticalSection(&ctx->Lock);
    ctx->qwLoadAddressNext = VMMWIN_PDB_LOAD_ADDRESS_BASE;
    ctx->fDisabled = TRUE;
    ctxVmm->pPdbContext = ctx;
    if(fInitializeKernelAsync) {
        hThreadAsyncKernel = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PDB_Initialize_Async_Kernel, (LPVOID)pKernelParameters, 0, NULL);
        if(!hThreadAsyncKernel) {
            DeleteCriticalSection(&ctx->Lock);
            goto fail;
        }
        WaitForSingleObject(hEventThreadStarted, 500);  // wait for async thread initialize thread to start (and acquire PDB lock).
        CloseHandle(hThreadAsyncKernel);
    } else {
        PDB_Initialize_Async_Kernel(pKernelParameters); // synchronous call
    }
    CloseHandle(hEventThreadStarted);
    return;
fail:
    if(hEventThreadStarted) { CloseHandle(hEventThreadStarted); }
    if(ctx) {
        if(ctx->hSym) {
            ctx->pfn.SymCleanup(ctx->hSym);
        }
        Ob_DECREF(ctx->pmPdbByHash);
        Ob_DECREF(ctx->pmPdbByModule);
        if(ctx->hModuleDbgHelp) { FreeLibrary(ctx->hModuleDbgHelp); }
        if(ctx->hModuleSymSrv) { FreeLibrary(ctx->hModuleSymSrv); }
    }
    LocalFree(ctx);
    LocalFree(pKernelParameters);
    ctxMain->pdb.fEnable = FALSE;
}
