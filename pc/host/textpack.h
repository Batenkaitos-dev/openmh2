/* The check of a chosen text pack before the game starts (textpack.c). */
#ifndef MH2_TEXTPACK_H
#define MH2_TEXTPACK_H

#include <stddef.h>

/* 0 when the folder is a pack the core can open: it exists, holds a well-formed pack.ini (pc/ipc/packini.h), has the
 * character map that file names, and every file below it can be read. Otherwise -1 and a sentence in why that names the
 * pack and what is wrong. */
int textpack_check(const char *dir, char *why, size_t cap);

#endif
