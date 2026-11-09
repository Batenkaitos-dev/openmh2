#ifndef MH2_CORE_HOST_FILE_H
#define MH2_CORE_HOST_FILE_H

/*
 * The core's files (host side). Every path the core takes is UTF-8: the archive, the cards, the states and the files of
 * the development options. On Linux that is the file system's own encoding. On Windows the C library's ANSI calls
 * would lose every letter outside the code page, so the paths go through the wide calls (host_windows.c): converted
 * from UTF-8 to UTF-16 and opened with _wfopen, _wremove, _wrename and MoveFileExW. A path that is not valid UTF-8 is
 * read in the ANSI code page instead, so a caller that still hands over an ANSI name works as it did.
 *
 * Only the platform layer's host files include this (it includes <stdio.h>); game-facing files use host.h.
 */
#include <stdio.h>

#include "host.h"

/* fopen with a UTF-8 path. */
HOST FILE *HostFile_Open(const char *path, const char *mode);
/* remove, rename (which fails on Windows when TO exists, as the C library's does), and a rename that replaces TO and, on
 * Windows, is written through to the disk. 0 when done. */
HOST int HostFile_Remove(const char *path);
HOST int HostFile_Rename(const char *from, const char *to);
HOST int HostFile_Replace(const char *from, const char *to);
/* The entries of folder DIR, in no order, without "." and "..": FN(name, is_folder, arg) for each, the names UTF-8. 0 when
 * the folder was read, -1 when it could not be. (A pack's files are listed to make its hash.) */
HOST int HostFile_List(const char *dir, void (*fn)(const char *name, int is_folder, void *arg), void *arg);

#endif
