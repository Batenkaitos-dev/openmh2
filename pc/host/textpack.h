/* The check of a chosen text pack before the game starts (textpack.c). */
#ifndef MH2_TEXTPACK_H
#define MH2_TEXTPACK_H

#include <stddef.h>

/* 0 when the folder is a pack the core can open: it exists, holds a well-formed pack.ini (pc/ipc/packini.h), has the
 * character map that file names, and every file below it can be read. Otherwise -1 and a sentence in why that names the
 * pack and what is wrong. */
int textpack_check(const char *dir, char *why, size_t cap);
/* The same check for the launcher: the sentence names the pack as SHOWN (its folder's name) and no path of this machine, and
 * a file it names is a path below the pack's folder. */
int textpack_check_as(const char *dir, const char *shown, char *why, size_t cap);

/* A folder of text_packs: the folder's name, and the name its pack.ini gives (the folder's name when it has no pack.ini that
 * can be read, or one that is not well-formed: textpack_check says what is wrong with it). */
typedef struct TextPackInfo {
    char folder[256];
    char name[65];
    int named;   /* name is the pack.ini's */
} TextPackInfo;
/* The folders of ROOT (a UTF-8 path) that do not start with a dot, in the byte order of their names; files are not packs. At
 * most max; returns how many (0 when ROOT cannot be read). */
int textpack_list(const char *root, TextPackInfo *out, int max);

#endif
