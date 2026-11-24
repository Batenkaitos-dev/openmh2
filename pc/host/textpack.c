/*
 * The check of a chosen text pack, before the game starts: the folder is there, its
 * pack.ini is well-formed, the character map it names is there, its text table (text.tsv) loads, and every file of the folder
 * can be read. The core opens the pack again and refuses it on the same grounds; this check says what is wrong without
 * starting a process. The text table is read by the core's own code (pc/core/io/host_text.c, compiled into mh2 too), so
 * the launcher and the core agree on every line of it.
 *
 * The launcher also lists the folders of text_packs (textpack_list) and asks the check about the chosen one.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc/packini.h"
#include "hostfs.h"
#include "textpack.h"

/* The core's text table (pc/core/io/host_text.c, compiled into mh2 with host_sha256.c): the host-side names it calls are
 * mh2's own files and environment. */
int Host_TextLoad(const char *dir, const char *charmap);
const char *Host_TextWhy(void);
void Host_TextClose(void);
FILE *HostFile_Open(const char *path, const char *mode);
const char *Host_Getenv(const char *name);
void Host_Log(const char *msg);

FILE *HostFile_Open(const char *path, const char *mode)
{
    return hfs_fopen(path, mode);
}

const char *Host_Getenv(const char *name)
{
    return hfs_getenv(name);
}

void Host_Log(const char *msg)
{
    (void)msg;   /* (the table's log of new strings is a development option of the core's) */
}

typedef struct Walk {
    char *why;
    size_t cap;
    int bad;
    int depth;
} Walk;

static SDL_EnumerationResult walk_entry(void *ud, const char *dirname, const char *fname)
{
    Walk *w = (Walk *)ud;
    char path[2048];
    SDL_PathInfo info;
    snprintf(path, sizeof(path), "%s%s", dirname, fname);
    if (!SDL_GetPathInfo(path, &info)) {
        snprintf(w->why, w->cap, "the file %s can't be examined", path);
        w->bad = 1;
        return SDL_ENUM_FAILURE;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (w->depth < 16) {
            w->depth++;
            if (!SDL_EnumerateDirectory(path, walk_entry, w) && !w->bad) {
                snprintf(w->why, w->cap, "the folder %s can't be read", path);
                w->bad = 1;
            }
            w->depth--;
        }
        return w->bad ? SDL_ENUM_FAILURE : SDL_ENUM_CONTINUE;
    }
    {
        FILE *f = hfs_fopen(path, "rb");
        if (!f) {
            snprintf(w->why, w->cap, "the file %s can't be read", path);
            w->bad = 1;
            return SDL_ENUM_FAILURE;
        }
        fclose(f);
    }
    return SDL_ENUM_CONTINUE;
}

static int check_dir(const char *dir, char *why, size_t cap)
{
    SDL_PathInfo info;
    char path[2048];
    static char ini[16385];
    PackIni pi;
    char reason[300];
    FILE *f;
    size_t got;
    Walk w = {why, cap, 0, 0};

    why[0] = 0;
    if (!SDL_GetPathInfo(dir, &info)) {
        snprintf(why, cap, "The translation pack folder %s doesn't exist.", dir);
        return -1;
    }
    if (info.type != SDL_PATHTYPE_DIRECTORY) {
        snprintf(why, cap, "The translation pack %s is not a folder.", dir);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/pack.ini", dir);
    f = hfs_fopen(path, "rb");
    if (!f) {
        snprintf(why, cap, "The translation pack folder %s has no pack.ini that can be read.", dir);
        return -1;
    }
    got = fread(ini, 1, sizeof(ini), f);
    fclose(f);
    if (got > sizeof(ini) - 1) {
        snprintf(why, cap, "The pack.ini of the translation pack %s is over 16 KB.", dir);
        return -1;
    }
    if (PackIni_Parse(ini, (unsigned long)got, &pi, reason, sizeof(reason)) != 0) {
        snprintf(why, cap, "The translation pack %s can't be used: %s.", dir, reason);
        return -1;
    }
    if (pi.hasCharmap) {
        snprintf(path, sizeof(path), "%s/%s", dir, pi.charmap);
        f = hfs_fopen(path, "rb");
        if (!f) {
            snprintf(why, cap, "The translation pack %s can't be used: the character map %s that its pack.ini names is not there.", dir,
                     pi.charmap);
            return -1;
        }
        fclose(f);
    }
    {   /* the text table, loaded as the core loads it: the character map is the file pack.ini names, else charmap.txt if there is one */
        FILE *c;
        snprintf(path, sizeof(path), "%s/%s", dir, pi.charmap);
        c = hfs_fopen(path, "rb");
        if (c)
            fclose(c);
        if (Host_TextLoad(dir, c || pi.hasCharmap ? pi.charmap : NULL) != 0) {
            snprintf(why, cap, "The translation pack %s can't be used: %s.", pi.name, Host_TextWhy());
            Host_TextClose();
            return -1;
        }
        Host_TextClose();
    }
    if (!SDL_EnumerateDirectory(dir, walk_entry, &w) || w.bad) {
        if (!w.bad)
            snprintf(why, cap, "The translation pack folder %s can't be read.", dir);
        else {
            char first[1024];
            snprintf(first, sizeof(first), "%s", why);
            snprintf(why, cap, "The translation pack %s can't be used: %s.", pi.name, first);
        }
        return -1;
    }
    return 0;
}

int textpack_check(const char *dir, char *why, size_t cap)
{
    return check_dir(dir, why, cap);
}

/* Every occurrence of FROM in S becomes TO (S holds cap bytes). */
static void replace_all(char *s, size_t cap, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to);
    char *p = s;
    if (!fl)
        return;
    while ((p = strstr(p, from)) != NULL) {
        size_t rest = strlen(p + fl);
        if (strlen(s) - fl + tl >= cap)
            return;
        memmove(p + tl, p + fl, rest + 1);
        memcpy(p, to, tl);
        p += tl;
    }
}

int textpack_check_as(const char *dir, const char *shown, char *why, size_t cap)
{
    int rc = check_dir(dir, why, cap);
    if (rc != 0) {   /* the pack's folder is named as the player knows it: its path is the machine's */
        char with_slash[2100];
        snprintf(with_slash, sizeof(with_slash), "%s/", dir);
        replace_all(why, cap, with_slash, "");
        replace_all(why, cap, dir, shown);
    }
    return rc;
}

typedef struct List {
    TextPackInfo *out;
    int max, n;
    const char *root;
} List;

static SDL_EnumerationResult list_entry(void *ud, const char *dirname, const char *fname)
{
    List *l = (List *)ud;
    char path[2300];
    SDL_PathInfo info;
    TextPackInfo *t;
    (void)dirname;
    if (fname[0] == '.' || strlen(fname) >= sizeof(t->folder))
        return SDL_ENUM_CONTINUE;
    snprintf(path, sizeof(path), "%s/%s", l->root, fname);
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_DIRECTORY || l->n >= l->max)
        return SDL_ENUM_CONTINUE;
    t = &l->out[l->n++];
    memset(t, 0, sizeof(*t));
    snprintf(t->folder, sizeof(t->folder), "%s", fname);
    snprintf(t->name, sizeof(t->name), "%s", fname);
    {
        static char ini[16385];
        PackIni pi;
        char reason[300];
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s/pack.ini", l->root, fname);
        f = hfs_fopen(path, "rb");
        if (f) {
            size_t got = fread(ini, 1, sizeof(ini), f);
            fclose(f);
            if (got < sizeof(ini) && PackIni_Parse(ini, (unsigned long)got, &pi, reason, sizeof(reason)) == 0) {
                snprintf(t->name, sizeof(t->name), "%s", pi.name);
                t->named = 1;
            }
        }
    }
    return SDL_ENUM_CONTINUE;
}

static int by_folder(const void *a, const void *b)
{
    return strcmp(((const TextPackInfo *)a)->folder, ((const TextPackInfo *)b)->folder);
}

int textpack_list(const char *root, TextPackInfo *out, int max)
{
    List l = {out, max, 0, root};
    if (!root || !*root || !SDL_EnumerateDirectory(root, list_entry, &l))
        return 0;
    qsort(out, (size_t)l.n, sizeof(*out), by_folder);
    return l.n;
}
