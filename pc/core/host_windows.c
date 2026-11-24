/* The host seam on Windows (llvm-mingw, i686): see host.h. */
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int sRefused;

HOST int Host_Refused(void)
{
    return sRefused;
}

HOST void Host_Refuse(const char *msg)
{
    fprintf(stderr, "mh2-core: %s\n", msg);
    fflush(stderr);
    sRefused = 1;
    FatalHook(msg);
    exit(HOST_EXIT_REFUSED);
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

/*
 * The command line split into arguments, by the rules of CommandLineToArgvW (the first word: up to the next quote when
 * it opens with one, else up to the first blank; then: blanks separate, a quote opens or closes a quoted run, a pair
 * of quotes inside a run is one quote and ends the run, 2n backslashes before a quote are n backslashes and the quote works, 2n+1 are
 * n backslashes and a literal quote, backslashes elsewhere stay). It is not that function: the core must not load
 * shell32.dll, which pulls in user32.dll, and user32 maps the desktop heap (about 20 MB, in the low addresses) before
 * the first line of the program runs: the low pool (pal_mem.c) lies under the image, from 0x02000000, and that
 * mapping covers part of it. The returned array and its strings are one block, never freed.
 */
static wchar_t **SplitCommandLine(const wchar_t *cl, int *count)
{
    size_t len = wcslen(cl), n = 0;
    wchar_t **v = (wchar_t **)calloc(len / 2 + 2, sizeof(wchar_t *));
    wchar_t *buf = (wchar_t *)calloc(len + 2, sizeof(wchar_t)), *o = buf;
    const wchar_t *p = cl;
    if (!v || !buf) {
        return NULL;
    }
    v[n++] = o;
    if (*p == L'"') {
        for (p++; *p && *p != L'"'; p++) {
            *o++ = *p;
        }
        if (*p) {
            p++;
        }
    } else {
        for (; *p && *p != L' ' && *p != L'\t'; p++) {
            *o++ = *p;
        }
    }
    *o++ = 0;
    for (;;) {
        int quoted = 0;
        while (*p == L' ' || *p == L'\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        v[n++] = o;
        for (; *p && (quoted || (*p != L' ' && *p != L'\t')); ) {
            size_t bs = 0;
            while (*p == L'\\') {
                bs++;
                p++;
            }
            if (*p == L'"') {
                size_t k;
                for (k = 0; k < bs / 2; k++) {
                    *o++ = L'\\';
                }
                if (bs & 1) {
                    *o++ = L'"';
                    p++;
                } else if (quoted && p[1] == L'"') {
                    *o++ = L'"';   /* a pair of quotes inside a quoted run: one quote, and the run ends */
                    p += 2;
                    quoted = 0;
                } else {
                    quoted = !quoted;
                    p++;
                }
            } else {
                size_t k;
                for (k = 0; k < bs; k++) {
                    *o++ = L'\\';
                }
                if (*p) {
                    *o++ = *p++;
                }
            }
        }
        *o++ = 0;
    }
    *count = (int)n;
    return v;
}

HOST void Host_Args(int *argc, char ***argv)
{
    int n = 0, i;
    wchar_t **w = SplitCommandLine(GetCommandLineW(), &n);
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

HOST int HostFile_List(const char *dir, void (*fn)(const char *name, int is_folder, void *arg), void *arg)
{
    size_t n = strlen(dir);
    char *pat = (char *)malloc(n + 3);
    wchar_t *wp;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!pat) {
        return -1;
    }
    memcpy(pat, dir, n);
    memcpy(pat + n, "/*", 3);
    wp = HostWide_Alloc(pat);
    free(pat);
    if (!wp) {
        return -1;
    }
    h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        char *u;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
            continue;
        }
        u = HostWide_ToUtf8(fd.cFileName);
        if (u) {
            fn(u, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0, arg);
            free(u);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
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

typedef struct ThreadStart {
    HostThreadFn fn;
    void *arg;
} ThreadStart;

static DWORD WINAPI ThreadMain(LPVOID p)
{
    ThreadStart t = *(ThreadStart *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}

HOST int Host_ThreadStart(HostThreadFn fn, void *arg, int low)
{
    ThreadStart *t = malloc(sizeof(*t));
    HANDLE h;
    if (!t)
        return 0;
    t->fn = fn;
    t->arg = arg;
    h = CreateThread(NULL, 8u << 20, ThreadMain, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) {
        free(t);
        return 0;
    }
    if (low)
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(h);
    return 1;
}

HOST void *Host_SemNew(void)
{
    return CreateSemaphoreW(NULL, 0, 0x7FFFFFFF, NULL);
}

HOST void Host_SemPost(void *sem)
{
    ReleaseSemaphore((HANDLE)sem, 1, NULL);
}

HOST int Host_SemWait(void *sem, unsigned int ms)
{
    return WaitForSingleObject((HANDLE)sem, ms) == WAIT_OBJECT_0;
}
