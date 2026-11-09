/*
 * The check of a chosen text pack, before the game starts: the folder is there, its
 * pack.ini is well-formed, the character map it names is there, and every file of the folder can be read. The core opens the
 * pack again and refuses it on the same grounds; this check says what is wrong without starting a process.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "ipc/packini.h"
#include "hostfs.h"
#include "textpack.h"

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

int textpack_check(const char *dir, char *why, size_t cap)
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
