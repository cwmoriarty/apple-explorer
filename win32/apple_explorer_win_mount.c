#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/apple_explorer.h"

/* ---------------- Dynamic Windows Projected File System (ProjectedFSLib.dll) ABI ---------------- */

typedef void *PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT;
typedef void *PRJ_DIR_ENTRY_BUFFER_HANDLE;

typedef struct {
    UINT32                               Size;
    UINT32                               Flags;
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT NamespaceVirtualizationContext;
    INT32                                CommandId;
    GUID                                 FileId;
    GUID                                 DataStreamId;
    PCWSTR                               FilePathName;
    void                                *VersionInfo;
    UINT32                               TriggeringProcessId;
    PCWSTR                               TriggeringProcessImageFileName;
    void                                *InstanceContext;
} PRJ_CALLBACK_DATA;

typedef struct {
    BOOLEAN IsDirectory;
    INT64 FileSize;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    UINT32 FileAttributes;
} PRJ_FILE_BASIC_INFO;

typedef struct {
    PRJ_FILE_BASIC_INFO FileBasicInfo;
    struct { UINT32 EaBufferSize; UINT32 OffsetToFirstEa; } EaInformation;
    struct { UINT32 SecurityBufferSize; UINT32 OffsetToSecurityDescriptor; } SecurityInformation;
    struct { UINT32 StreamsInfoBufferSize; UINT32 OffsetToFirstStreamInfo; } StreamsInformation;
    UINT8 VersionInfo[256];
} PRJ_PLACEHOLDER_INFO;

typedef HRESULT (CALLBACK *PRJ_START_DIR_ENUM_CB)(const PRJ_CALLBACK_DATA *, const GUID *);
typedef HRESULT (CALLBACK *PRJ_END_DIR_ENUM_CB)(const PRJ_CALLBACK_DATA *, const GUID *);
typedef HRESULT (CALLBACK *PRJ_GET_DIR_ENUM_CB)(const PRJ_CALLBACK_DATA *, const GUID *, PCWSTR, PRJ_DIR_ENTRY_BUFFER_HANDLE);
typedef HRESULT (CALLBACK *PRJ_GET_PLACEHOLDER_CB)(const PRJ_CALLBACK_DATA *);
typedef HRESULT (CALLBACK *PRJ_GET_FILE_DATA_CB)(const PRJ_CALLBACK_DATA *, UINT64, UINT32);

typedef struct {
    PRJ_START_DIR_ENUM_CB   StartDirectoryEnumerationCallback;
    PRJ_END_DIR_ENUM_CB     EndDirectoryEnumerationCallback;
    PRJ_GET_DIR_ENUM_CB     GetDirectoryEnumerationCallback;
    PRJ_GET_PLACEHOLDER_CB  GetPlaceholderInfoCallback;
    PRJ_GET_FILE_DATA_CB    GetFileDataCallback;
    void                   *QueryFileNameCallback;
    void                   *NotificationCallback;
    void                   *CancelCommandCallback;
} PRJ_CALLBACKS;

typedef HRESULT (WINAPI *PFN_PrjMarkDirectoryAsPlaceholder)(PCWSTR, PCWSTR, const void *, const GUID *);
typedef HRESULT (WINAPI *PFN_PrjStartVirtualizing)(PCWSTR, const PRJ_CALLBACKS *, const void *, const void *, PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT *);
typedef void    (WINAPI *PFN_PrjStopVirtualizing)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT);
typedef HRESULT (WINAPI *PFN_PrjFillDirEntryBuffer)(PCWSTR, PRJ_FILE_BASIC_INFO *, PRJ_DIR_ENTRY_BUFFER_HANDLE);
typedef BOOLEAN (WINAPI *PFN_PrjFileNameMatch)(PCWSTR, PCWSTR);
typedef int     (WINAPI *PFN_PrjFileNameCompare)(PCWSTR, PCWSTR);
typedef HRESULT (WINAPI *PFN_PrjWritePlaceholderInfo)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT, PCWSTR, const PRJ_PLACEHOLDER_INFO *, UINT32);
typedef void *  (WINAPI *PFN_PrjAllocateAlignedBuffer)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT, SIZE_T);
typedef void    (WINAPI *PFN_PrjFreeAlignedBuffer)(void *);
typedef HRESULT (WINAPI *PFN_PrjWriteFileData)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT, const GUID *, void *, UINT64, UINT32);

typedef struct {
    WCHAR       wname[256];
    uint64_t    ino;
    aex_ftype_t ftype;
    aex_attr_t  attr;
} proj_entry_t;

typedef struct proj_enum_session {
    GUID                       id;
    int                        populated;
    size_t                     cursor;
    size_t                     count;
    size_t                     cap;
    proj_entry_t              *entries;
    struct proj_enum_session  *next;
} proj_enum_session_t;

static struct {
    aex_t                             *fs;
    CRITICAL_SECTION                   lock;
    proj_enum_session_t               *sessions;
    PFN_PrjFillDirEntryBuffer          FillDirEntryBuffer;
    PFN_PrjFileNameMatch               FileNameMatch;
    PFN_PrjFileNameCompare             FileNameCompare;
    PFN_PrjWritePlaceholderInfo        WritePlaceholderInfo;
    PFN_PrjAllocateAlignedBuffer       AllocateAlignedBuffer;
    PFN_PrjFreeAlignedBuffer           FreeAlignedBuffer;
    PFN_PrjWriteFileData               WriteFileData;
    volatile LONG                      stop_requested;
} g_prj;

static void unix_to_large_filetime(int64_t sec, LARGE_INTEGER *out) {
    if (sec <= 0) { out->QuadPart = 0; return; }
    out->QuadPart = (LONGLONG)((sec + 11644473600LL) * 10000000LL);
}

static void wpath_to_utf8_posix(PCWSTR wpath, char *out_utf8, size_t cap) {
    if (!wpath || !wpath[0]) {
        snprintf(out_utf8, cap, "/");
        return;
    }
    char raw[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, raw, (int)sizeof(raw), NULL, NULL);
    if (n <= 0) { snprintf(out_utf8, cap, "/"); return; }
    size_t p = 0;
    out_utf8[p++] = '/';
    for (int i = 0; raw[i] && p + 1U < cap; i++) {
        out_utf8[p++] = (raw[i] == '\\') ? '/' : raw[i];
    }
    out_utf8[p] = '\0';
}

static void attr_to_basic_info(const aex_attr_t *a, PRJ_FILE_BASIC_INFO *bi) {
    memset(bi, 0, sizeof(*bi));
    bi->IsDirectory = (a->ftype == AEX_FT_DIR) ? TRUE : FALSE;
    bi->FileSize = (a->ftype == AEX_FT_DIR) ? 0 : (INT64)a->size;
    unix_to_large_filetime(a->crtime_sec ? a->crtime_sec : a->mtime_sec, &bi->CreationTime);
    unix_to_large_filetime(a->atime_sec ? a->atime_sec : a->mtime_sec, &bi->LastAccessTime);
    unix_to_large_filetime(a->mtime_sec, &bi->LastWriteTime);
    unix_to_large_filetime(a->ctime_sec ? a->ctime_sec : a->mtime_sec, &bi->ChangeTime);
    bi->FileAttributes = (a->ftype == AEX_FT_DIR) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
}

static int collect_enum_cb(void *user_ctx, const char *name, uint64_t ino, aex_ftype_t ftype) {
    proj_enum_session_t *s = (proj_enum_session_t *)user_ctx;
    if (s->count == s->cap) {
        size_t ncap = s->cap ? s->cap * 2U : 64U;
        proj_entry_t *ne = (proj_entry_t *)realloc(s->entries, ncap * sizeof(proj_entry_t));
        if (!ne) return 1;
        s->entries = ne;
        s->cap = ncap;
    }
    proj_entry_t *e = &s->entries[s->count++];
    memset(e, 0, sizeof(*e));
    MultiByteToWideChar(CP_UTF8, 0, name, -1, e->wname, 255);
    e->ino = ino;
    e->ftype = ftype;
    (void)aex_getattr(g_prj.fs, ino, &e->attr);
    return 0;
}

static int cmp_proj_entries(const void *a, const void *b) {
    const proj_entry_t *ea = (const proj_entry_t *)a;
    const proj_entry_t *eb = (const proj_entry_t *)b;
    return g_prj.FileNameCompare(ea->wname, eb->wname);
}

static HRESULT CALLBACK prj_start_enum(const PRJ_CALLBACK_DATA *cbData, const GUID *enumId) {
    (void)cbData;
    EnterCriticalSection(&g_prj.lock);
    proj_enum_session_t *s = (proj_enum_session_t *)calloc(1, sizeof(proj_enum_session_t));
    if (!s) { LeaveCriticalSection(&g_prj.lock); return E_OUTOFMEMORY; }
    s->id = *enumId;
    s->next = g_prj.sessions;
    g_prj.sessions = s;
    LeaveCriticalSection(&g_prj.lock);
    return S_OK;
}

static HRESULT CALLBACK prj_end_enum(const PRJ_CALLBACK_DATA *cbData, const GUID *enumId) {
    (void)cbData;
    EnterCriticalSection(&g_prj.lock);
    proj_enum_session_t **pp = &g_prj.sessions;
    while (*pp) {
        if (IsEqualGUID(&(*pp)->id, enumId)) {
            proj_enum_session_t *victim = *pp;
            *pp = victim->next;
            free(victim->entries);
            free(victim);
            break;
        }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&g_prj.lock);
    return S_OK;
}

static HRESULT CALLBACK prj_get_enum(
    const PRJ_CALLBACK_DATA *cbData,
    const GUID *enumId,
    PCWSTR searchExpr,
    PRJ_DIR_ENTRY_BUFFER_HANDLE dirBuf
) {
    EnterCriticalSection(&g_prj.lock);
    proj_enum_session_t *s = g_prj.sessions;
    while (s && !IsEqualGUID(&s->id, enumId)) s = s->next;
    if (!s) { LeaveCriticalSection(&g_prj.lock); return E_INVALIDARG; }

    if (!s->populated) {
        char path_utf8[1024];
        wpath_to_utf8_posix(cbData->FilePathName, path_utf8, sizeof(path_utf8));
        uint64_t dir_ino = 0;
        if (aex_lookup_path(g_prj.fs, path_utf8, &dir_ino) == 0) {
            (void)aex_readdir(g_prj.fs, dir_ino, collect_enum_cb, s);
            if (s->count > 1U && g_prj.FileNameCompare) {
                qsort(s->entries, s->count, sizeof(proj_entry_t), cmp_proj_entries);
            }
        }
        s->populated = 1;
        s->cursor = 0;
    }

    while (s->cursor < s->count) {
        proj_entry_t *e = &s->entries[s->cursor];
        if (searchExpr && searchExpr[0] && g_prj.FileNameMatch && !g_prj.FileNameMatch(e->wname, searchExpr)) {
            s->cursor++;
            continue;
        }
        PRJ_FILE_BASIC_INFO bi;
        attr_to_basic_info(&e->attr, &bi);
        HRESULT hr = g_prj.FillDirEntryBuffer(e->wname, &bi, dirBuf);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            break;
        }
        s->cursor++;
    }
    LeaveCriticalSection(&g_prj.lock);
    return S_OK;
}

static HRESULT CALLBACK prj_get_placeholder(const PRJ_CALLBACK_DATA *cbData) {
    EnterCriticalSection(&g_prj.lock);
    char path_utf8[1024];
    wpath_to_utf8_posix(cbData->FilePathName, path_utf8, sizeof(path_utf8));
    uint64_t ino = 0;
    aex_attr_t attr;
    if (aex_lookup_path(g_prj.fs, path_utf8, &ino) != 0 || aex_getattr(g_prj.fs, ino, &attr) != 0) {
        LeaveCriticalSection(&g_prj.lock);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    PRJ_PLACEHOLDER_INFO pi;
    memset(&pi, 0, sizeof(pi));
    attr_to_basic_info(&attr, &pi.FileBasicInfo);
    HRESULT hr = g_prj.WritePlaceholderInfo(cbData->NamespaceVirtualizationContext, cbData->FilePathName, &pi, sizeof(pi));
    LeaveCriticalSection(&g_prj.lock);
    return hr;
}

static HRESULT CALLBACK prj_get_file_data(const PRJ_CALLBACK_DATA *cbData, UINT64 byteOffset, UINT32 length) {
    EnterCriticalSection(&g_prj.lock);
    char path_utf8[1024];
    wpath_to_utf8_posix(cbData->FilePathName, path_utf8, sizeof(path_utf8));
    uint64_t ino = 0;
    if (aex_lookup_path(g_prj.fs, path_utf8, &ino) != 0) {
        LeaveCriticalSection(&g_prj.lock);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    void *abuf = g_prj.AllocateAlignedBuffer(cbData->NamespaceVirtualizationContext, length);
    if (!abuf) {
        LeaveCriticalSection(&g_prj.lock);
        return E_OUTOFMEMORY;
    }
    memset(abuf, 0, length);
    size_t got = 0;
    int rc = aex_read(g_prj.fs, ino, byteOffset, length, abuf, &got);
    HRESULT hr = (rc == 0)
        ? g_prj.WriteFileData(cbData->NamespaceVirtualizationContext, &cbData->DataStreamId, abuf, byteOffset, length)
        : HRESULT_FROM_WIN32(ERROR_IO_DEVICE);
    g_prj.FreeAlignedBuffer(abuf);
    LeaveCriticalSection(&g_prj.lock);
    return hr;
}

static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_prj.stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

int aex_win_mount(aex_t *fs, const char *mountpoint, const char *backend) {
    if (!fs || !mountpoint) return -1;
    int is_drive_letter = (strlen(mountpoint) >= 2 &&
                           ((mountpoint[0] >= 'A' && mountpoint[0] <= 'Z') || (mountpoint[0] >= 'a' && mountpoint[0] <= 'z')) &&
                           mountpoint[1] == ':' && (mountpoint[2] == '\0' || (mountpoint[2] == '\\' && mountpoint[3] == '\0')));

    if (strcmp(backend, "winfsp") == 0 || (strcmp(backend, "auto") == 0 && is_drive_letter)) {
        HMODULE wf = LoadLibraryW(L"winfsp-x64.dll");
        if (!wf) wf = LoadLibraryW(L"C:\\Program Files (x86)\\WinFsp\\bin\\winfsp-x64.dll");
        if (!wf) wf = LoadLibraryW(L"C:\\Program Files (x86)\\WinFsp\\bin\\winfsp-a64.dll");
        if (!wf) {
            fprintf(stderr,
                    "WinFsp runtime not found for drive letter %s.\n"
                    "  Option 1 (zero-driver): pass a folder path instead (e.g. C:\\Mounts\\AppleVol) to mount via built-in Windows ProjFS.\n"
                    "  Option 2 (drive letter): install WinFsp via 'winget install WinFsp.WinFsp'.\n",
                    mountpoint);
            return -1;
        }
        FreeLibrary(wf);
    }

    HMODULE hprj = LoadLibraryW(L"ProjectedFSLib.dll");
    if (!hprj) {
        fprintf(stderr,
                "Windows ProjectedFSLib.dll is not enabled on this system.\n"
                "Enable it once in Administrator PowerShell:\n"
                "  Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS -NoRestart\n"
                "Or use AppleExplorer.exe GUI / 'apple-explorer extract' with zero system features required.\n");
        return -1;
    }

    PFN_PrjMarkDirectoryAsPlaceholder MarkDir = (PFN_PrjMarkDirectoryAsPlaceholder)(void *)GetProcAddress(hprj, "PrjMarkDirectoryAsPlaceholder");
    PFN_PrjStartVirtualizing StartVirt = (PFN_PrjStartVirtualizing)(void *)GetProcAddress(hprj, "PrjStartVirtualizing");
    PFN_PrjStopVirtualizing StopVirt = (PFN_PrjStopVirtualizing)(void *)GetProcAddress(hprj, "PrjStopVirtualizing");
    g_prj.FillDirEntryBuffer = (PFN_PrjFillDirEntryBuffer)(void *)GetProcAddress(hprj, "PrjFillDirEntryBuffer");
    g_prj.FileNameMatch = (PFN_PrjFileNameMatch)(void *)GetProcAddress(hprj, "PrjFileNameMatch");
    g_prj.FileNameCompare = (PFN_PrjFileNameCompare)(void *)GetProcAddress(hprj, "PrjFileNameCompare");
    g_prj.WritePlaceholderInfo = (PFN_PrjWritePlaceholderInfo)(void *)GetProcAddress(hprj, "PrjWritePlaceholderInfo");
    g_prj.AllocateAlignedBuffer = (PFN_PrjAllocateAlignedBuffer)(void *)GetProcAddress(hprj, "PrjAllocateAlignedBuffer");
    g_prj.FreeAlignedBuffer = (PFN_PrjFreeAlignedBuffer)(void *)GetProcAddress(hprj, "PrjFreeAlignedBuffer");
    g_prj.WriteFileData = (PFN_PrjWriteFileData)(void *)GetProcAddress(hprj, "PrjWriteFileData");

    if (!MarkDir || !StartVirt || !StopVirt || !g_prj.FillDirEntryBuffer || !g_prj.WritePlaceholderInfo || !g_prj.WriteFileData) {
        FreeLibrary(hprj);
        return -1;
    }

    WCHAR wroot[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, mountpoint, -1, wroot, MAX_PATH);
    CreateDirectoryW(wroot, NULL);

    GUID instId;
    (void)CoCreateGuid(&instId);
    HRESULT hr = MarkDir(wroot, NULL, NULL, &instId);
    if (FAILED(hr)) {
        fprintf(stderr, "PrjMarkDirectoryAsPlaceholder failed on %s (HRESULT 0x%08lX)\n", mountpoint, (unsigned long)hr);
        FreeLibrary(hprj);
        return -1;
    }

    g_prj.fs = fs;
    g_prj.stop_requested = 0;
    InitializeCriticalSection(&g_prj.lock);

    PRJ_CALLBACKS cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.StartDirectoryEnumerationCallback = prj_start_enum;
    cbs.EndDirectoryEnumerationCallback = prj_end_enum;
    cbs.GetDirectoryEnumerationCallback = prj_get_enum;
    cbs.GetPlaceholderInfoCallback = prj_get_placeholder;
    cbs.GetFileDataCallback = prj_get_file_data;

    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT ctx = NULL;
    hr = StartVirt(wroot, &cbs, NULL, NULL, &ctx);
    if (FAILED(hr)) {
        DeleteCriticalSection(&g_prj.lock);
        FreeLibrary(hprj);
        fprintf(stderr, "PrjStartVirtualizing failed on %s (HRESULT 0x%08lX)\n", mountpoint, (unsigned long)hr);
        return -1;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    printf("Projected \"%s\" at %s via Windows ProjFS (Press Ctrl+C to unmount)...\n", aex_volume_name(fs), mountpoint);
    while (!g_prj.stop_requested) {
        Sleep(250);
    }

    StopVirt(ctx);
    DeleteCriticalSection(&g_prj.lock);
    FreeLibrary(hprj);
    printf("Stopped virtualization at %s.\n", mountpoint);
    return 0;
}
#endif
