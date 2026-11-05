#ifndef MH2_CORE_HOST_WIDE_H
#define MH2_CORE_HOST_WIDE_H

/* Windows only: UTF-8 to UTF-16 for the wide calls (host_file.h). */
#include <stdlib.h>
#include <windows.h>

/* A malloc'd UTF-16 copy of S, or NULL. A string that is not valid UTF-8 is converted in the ANSI code page. */
static inline wchar_t *HostWide_Alloc(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    wchar_t *w;
    if (n <= 0) {
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, s, -1, NULL, 0);
    }
    if (n <= 0) {
        return NULL;
    }
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(cp, flags, s, -1, w, n) <= 0) {
        free(w);
        w = NULL;
    }
    return w;
}

/* A malloc'd UTF-8 copy of the UTF-16 string W, or NULL. */
static inline char *HostWide_ToUtf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) {
        return NULL;
    }
    s = (char *)malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        s = NULL;
    }
    return s;
}

#endif
