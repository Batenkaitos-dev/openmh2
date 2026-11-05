#ifndef MH2_HOSTFS_H
#define MH2_HOSTFS_H

/*
 * mh2's files, folders and environment. Every path mh2 takes is UTF-8: on Linux that is the file system's own encoding.
 * On Windows the C library's ANSI calls (fopen, remove, rename, getenv, and the argv main is given) would lose every letter
 * outside the code page, so the paths go through the wide calls: converted from UTF-8 to UTF-16 here, and the command line
 * and the environment are read as UTF-16 and handed on as UTF-8. SDL's file and folder functions (SDL_GetPathInfo,
 * SDL_CreateDirectory, SDL_EnumerateDirectory, SDL_RenamePath, SDL_CopyFile, SDL_IOFromFile, ...) take UTF-8 on every
 * system and are used as they are.
 *
 * A string that is not valid UTF-8 (a path in a file written in the ANSI code page, in an older mh2.ini) is read in the
 * ANSI code page on Windows (hfs_utf8), as the core reads one.
 *
 * One way to open: the host's files are opened with these functions and no other. This header ends by making the C
 * library's own names an error, so that a later change cannot add an ANSI open by mistake (the guard); every file of mh2 is compiled with it first (the build's -include), after
 * the system headers and SDL's it needs itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>   /* (its storage interface has a member named rename: before the guard) */

#ifdef _WIN32
#include <wchar.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* fopen, remove and rename with UTF-8 paths. As the C library's: hfs_rename fails on Windows when TO exists. */
FILE *hfs_fopen(const char *path, const char *mode);
int hfs_remove(const char *path);
int hfs_rename(const char *from, const char *to);

/* getenv: the value as UTF-8 (valid), or NULL. The string is kept until fifteen later calls have been made. */
const char *hfs_getenv(const char *name);

/* The command line as UTF-8, replacing main's argc and argv on Windows (the C library's argv is in the ANSI code page);
 * elsewhere it leaves them. */
void hfs_args(int *argc, char ***argv);

/* S as valid UTF-8, in place when it is already (every system) and otherwise, on Windows, read in the ANSI code page and
 * written as UTF-8 into OUT (CAP bytes). Returns the string to use: S or OUT. */
const char *hfs_utf8(const char *s, char *out, size_t cap);

#ifdef _WIN32
/* S (UTF-8, else the ANSI code page) as a malloc'd UTF-16 string, or NULL. */
wchar_t *hfs_wide(const char *s);
#endif

#ifdef __cplusplus
}
#endif

/* The guard: the C library's ANSI names are an error in the host's code (use the hfs_ ones above). hostfs.c, which
 * implements them, defines MH2_HOSTFS_IMPL before this header. */
#ifndef MH2_HOSTFS_IMPL
#pragma GCC poison fopen freopen remove rename getenv _wfopen
#endif

#endif
