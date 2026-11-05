/*
 * mh2's files, folders and environment (hostfs.h): UTF-8 paths, opened through the wide calls on Windows.
 */
/* (the build defines MH2_HOSTFS_IMPL for this file: the C library's names are its to use) */
#include "hostfs.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

/* S as UTF-16: UTF-8 when it is valid, else the ANSI code page. */
wchar_t *hfs_wide(const char *s)
{
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) {
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, s, -1, NULL, 0);
    }
    if (n <= 0)
        return NULL;
    w = malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(cp, flags, s, -1, w, n) <= 0) {
        free(w);
        w = NULL;
    }
    return w;
}

/* A UTF-16 string as malloc'd UTF-8, or NULL. */
static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0)
        return NULL;
    s = malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        s = NULL;
    }
    return s;
}

FILE *hfs_fopen(const char *path, const char *mode)
{
    wchar_t *wp = hfs_wide(path), wm[8];
    FILE *f = NULL;
    int i;
    if (!wp)
        return NULL;
    for (i = 0; mode[i] && i < 7; i++)
        wm[i] = (wchar_t)(unsigned char)mode[i];
    wm[i] = 0;
    f = _wfopen(wp, wm);
    free(wp);
    return f;
}

int hfs_remove(const char *path)
{
    wchar_t *wp = hfs_wide(path);
    int r = wp ? _wremove(wp) : -1;
    free(wp);
    return r;
}

int hfs_rename(const char *from, const char *to)
{
    wchar_t *wf = hfs_wide(from), *wt = hfs_wide(to);
    int r = wf && wt ? _wrename(wf, wt) : -1;
    free(wf);
    free(wt);
    return r;
}

const char *hfs_getenv(const char *name)
{
    static char ring[15][4096];
    static unsigned next;
    wchar_t *wn = hfs_wide(name), *wv = NULL;
    char *s = NULL, *slot = ring[next++ % 15];
    DWORD n;
    if (!wn)
        return NULL;
    n = GetEnvironmentVariableW(wn, NULL, 0);
    if (n > 0 && (wv = malloc((size_t)n * sizeof(wchar_t))) != NULL) {
        if (GetEnvironmentVariableW(wn, wv, n) == n - 1)
            s = narrow(wv);
        free(wv);
    }
    free(wn);
    if (!s)
        return NULL;
    snprintf(slot, sizeof(ring[0]), "%s", s);
    free(s);
    return slot;
}

void hfs_args(int *argc, char ***argv)
{
    int n = 0, i;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **a;
    if (!w || n < 1)
        return;
    a = calloc((size_t)n + 1, sizeof(char *));
    for (i = 0; a && i < n; i++) {
        a[i] = narrow(w[i]);
        if (!a[i])
            return;   /* the C library's argv stays */
    }
    LocalFree(w);
    if (a) {
        *argc = n;
        *argv = a;
    }
}

const char *hfs_utf8(const char *s, char *out, size_t cap)
{
    wchar_t *w;
    char *u;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) > 0)
        return s;
    w = hfs_wide(s);   /* the ANSI code page */
    u = w ? narrow(w) : NULL;
    free(w);
    if (!u)
        return s;
    snprintf(out, cap, "%s", u);
    free(u);
    return out;
}

#else

FILE *hfs_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int hfs_remove(const char *path)
{
    return remove(path);
}

int hfs_rename(const char *from, const char *to)
{
    return rename(from, to);
}

const char *hfs_getenv(const char *name)
{
    return getenv(name);
}

void hfs_args(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
}

const char *hfs_utf8(const char *s, char *out, size_t cap)
{
    (void)out;
    (void)cap;
    return s;
}

#endif
