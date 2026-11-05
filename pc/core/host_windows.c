/* The host seam on Windows (llvm-mingw, i686): see host.h. */
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

#include "host.h"
#include "host_file.h"
#include "host_wide.h"

extern IMAGE_DOS_HEADER __ImageBase;

static HostFatalHook sFatalHook;
static HostExitHook sExitHook;

HOST void Host_SetEndHooks(HostFatalHook fatal, HostExitHook onExit)
{
    sFatalHook = fatal;
    sExitHook = onExit;
}

HOST void Host_ExitHook(int code)
{
    HostExitHook h = sExitHook;
    sExitHook = 0;
    if (h)
        h(code);
}

/* Once: a fatal error inside the hook aborts plainly. */
static void FatalHook(const char *msg)
{
    HostFatalHook h = sFatalHook;
    sFatalHook = 0;
    if (h)
        h(msg);
}

HOST void Host_Fatal(const char *msg)
{
    fprintf(stderr, "mh2-core: %s\n", msg);
    fflush(stderr);
    FatalHook(msg);
    abort();
}

HOST void Host_StubFatal(const char *name)
{
    fprintf(stderr, "mh2-core: library function %s was called, and is not written yet\n", name);
    fflush(stderr);
    FatalHook("a library function was called that is not written yet");
    abort();
}

HOST void Host_Log(const char *msg)
{
    fprintf(stderr, "mh2-core: %s\n", msg);
}

HOST void *Host_Alloc(unsigned int size)
{
    return calloc(1, size);
}

HOST void Host_Free(void *p)
{
    free(p);
}

HOST int Host_Protect(void *p, unsigned int size, int readWrite)
{
    DWORD old;
    return VirtualProtect(p, size, readWrite ? PAGE_READWRITE : PAGE_NOACCESS, &old) ? 0 : -1;
}

HOST void Host_ImageRange(unsigned int *lo, unsigned int *hi)
{
    const BYTE *base = (const BYTE *)&__ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + __ImageBase.e_lfanew);
    *lo = (unsigned int)(size_t)base;
    *hi = (unsigned int)(size_t)base + nt->OptionalHeader.SizeOfImage;
}

HOST const char *Host_Getenv(const char *name)
{
    wchar_t *wname = HostWide_Alloc(name), *wval;
    DWORD n;
    char *val = NULL;
    if (!wname) {
        return NULL;
    }
    n = GetEnvironmentVariableW(wname, NULL, 0);
    wval = n ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (wval && GetEnvironmentVariableW(wname, wval, n) == n - 1) {
        val = HostWide_ToUtf8(wval);   /* kept: the callers hold the pointer and ask a few times at start-up */
    }
    free(wval);
    free(wname);
    return val;
}

HOST void Host_Args(int *argc, char ***argv)
{
    int n = 0, i;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **a;
    if (!w || n < 1) {
        return;
    }
    a = (char **)calloc((size_t)n + 1, sizeof(char *));
    for (i = 0; a && i < n; i++) {
        a[i] = HostWide_ToUtf8(w[i]);
        if (!a[i]) {
            return;   /* the C library's argv stays */
        }
    }
    if (a) {
        *argc = n;
        *argv = a;
    }
}

HOST FILE *HostFile_Open(const char *path, const char *mode)
{
    wchar_t *wp = HostWide_Alloc(path), wm[8];
    FILE *f = NULL;
    int i;
    if (!wp) {
        return NULL;
    }
    for (i = 0; mode[i] && i < 7; i++) {
        wm[i] = (wchar_t)(unsigned char)mode[i];
    }
    wm[i] = 0;
    f = _wfopen(wp, wm);
    free(wp);
    return f;
}

HOST int HostFile_Remove(const char *path)
{
    wchar_t *wp = HostWide_Alloc(path);
    int r = wp ? _wremove(wp) : -1;
    free(wp);
    return r;
}

HOST int HostFile_Rename(const char *from, const char *to)
{
    wchar_t *wf = HostWide_Alloc(from), *wt = HostWide_Alloc(to);
    int r = wf && wt ? _wrename(wf, wt) : -1;
    free(wf);
    free(wt);
    return r;
}

HOST int HostFile_Replace(const char *from, const char *to)
{
    wchar_t *wf = HostWide_Alloc(from), *wt = HostWide_Alloc(to);
    int r = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
    free(wf);
    free(wt);
    return r;
}

HOST unsigned int Host_PageSize(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

HOST void *Host_LowMap(unsigned int addr, unsigned int size)
{
    return VirtualAlloc((void *)(size_t)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
