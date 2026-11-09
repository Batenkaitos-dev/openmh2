/*
 * Texture packs: which folders of <textures>/packs are on, and in what order (packs.h). Paths are UTF-8; the folder
 * is listed through SDL, which reads a Windows folder through the wide calls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "packs.h"

#define MAX_KEYS 512

/* The file's keys, in the file's order. settings.c lower-cases the key, so a name is matched without regard to
 * the case of ASCII letters (a Windows folder's name is not case-sensitive either). */
static struct {
    char name[PACK_NAME_MAX];
    int on;
} g_keys[MAX_KEYS];
static int g_nkeys;

static int g_changed;

void packs_reset(void)
{
    g_nkeys = 0;
    g_changed = 0;
}

int packs_setting(const char *key, const char *value)
{
    if (SDL_strncasecmp(key, "pack.", 5))
        return -1;
    const char *name = key + 5;
    int on;
    if (!SDL_strcasecmp(value, "on") || !SDL_strcasecmp(value, "yes") || !SDL_strcasecmp(value, "true") || !strcmp(value, "1"))
        on = 1;
    else if (!SDL_strcasecmp(value, "off") || !SDL_strcasecmp(value, "no") || !SDL_strcasecmp(value, "false") || !strcmp(value, "0"))
        on = 0;
    else
        return -2;
    if (!*name || strlen(name) >= PACK_NAME_MAX)
        return -1;
    for (int i = 0; i < g_nkeys; i++)
        if (!SDL_strcasecmp(g_keys[i].name, name)) {   /* a second line for one pack: it keeps its place */
            g_keys[i].on = on;
            return 0;
        }
    if (g_nkeys == MAX_KEYS)
        return -1;
    snprintf(g_keys[g_nkeys].name, PACK_NAME_MAX, "%s", name);
    g_keys[g_nkeys].on = on;
    g_nkeys++;
    return 0;
}

typedef struct {
    char (*names)[PACK_NAME_MAX];
    int n, max;
    const char *dir;
} Scan;

static SDL_EnumerationResult SDLCALL scan_cb(void *user, const char *dirname, const char *fname)
{
    Scan *s = (Scan *)user;
    char path[2048];
    SDL_PathInfo info;
    if (fname[0] == '.' || strlen(fname) >= PACK_NAME_MAX || s->n == s->max)
        return SDL_ENUM_CONTINUE;
    snprintf(path, sizeof(path), "%s%s%s", dirname, dirname[0] && dirname[strlen(dirname) - 1] != '/' &&
                                                         dirname[strlen(dirname) - 1] != '\\' ? "/" : "", fname);
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY)
        snprintf(s->names[s->n++], PACK_NAME_MAX, "%s", fname);
    return SDL_ENUM_CONTINUE;
}

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int packs_enabled(const char *dir, char (*names)[PACK_NAME_MAX], int max)
{
    enum { ALL = 1024 };
    static char all[ALL][PACK_NAME_MAX];
    Scan s = {all, 0, ALL, dir};
    SDL_EnumerateDirectory(dir, scan_cb, &s);
    qsort(all, (size_t)s.n, PACK_NAME_MAX, by_name);
    int n = 0;
    char used[ALL];
    memset(used, 0, sizeof(used));
    /* the file's keys first, in the file's order; a key with no folder is not a pack */
    for (int k = 0; k < g_nkeys && n < max; k++)
        for (int i = 0; i < s.n; i++)
            if (!used[i] && !SDL_strcasecmp(all[i], g_keys[k].name)) {
                used[i] = 1;
                if (g_keys[k].on)
                    snprintf(names[n++], PACK_NAME_MAX, "%s", all[i]);
                break;
            }
    /* then the folders the file does not name: on, by name */
    for (int i = 0; i < s.n && n < max; i++) {
        int named = 0;
        for (int k = 0; k < g_nkeys; k++)
            if (!SDL_strcasecmp(all[i], g_keys[k].name))
                named = 1;
        if (!named)
            snprintf(names[n++], PACK_NAME_MAX, "%s", all[i]);
    }
    return n;
}

int packs_list(const char *dir, PackInfo *out, int max)
{
    enum { ALL = 1024 };
    static char all[ALL][PACK_NAME_MAX];
    Scan s = {all, 0, ALL, dir};
    SDL_EnumerateDirectory(dir, scan_cb, &s);
    qsort(all, (size_t)s.n, PACK_NAME_MAX, by_name);
    int n = 0;
    char used[ALL];
    memset(used, 0, sizeof(used));
    for (int k = 0; k < g_nkeys && n < max; k++) {
        int found = -1;
        for (int i = 0; i < s.n && found < 0; i++)
            if (!used[i] && !SDL_strcasecmp(all[i], g_keys[k].name))
                found = i;
        if (found >= 0)
            used[found] = 1;
        snprintf(out[n].name, PACK_NAME_MAX, "%s", found >= 0 ? all[found] : g_keys[k].name);
        out[n].on = g_keys[k].on;
        out[n].present = found >= 0;
        n++;
    }
    for (int i = 0; i < s.n && n < max; i++)
        if (!used[i]) {
            snprintf(out[n].name, PACK_NAME_MAX, "%s", all[i]);
            out[n].on = 1;
            out[n].present = 1;
            n++;
        }
    return n;
}

void packs_set(const PackInfo *list, int n)
{
    g_nkeys = 0;
    for (int i = 0; i < n && g_nkeys < MAX_KEYS; i++) {
        snprintf(g_keys[g_nkeys].name, PACK_NAME_MAX, "%s", list[i].name);
        g_keys[g_nkeys].on = list[i].on != 0;
        g_nkeys++;
    }
    g_changed = 1;
}

int packs_changed(void)
{
    return g_changed;
}

void packs_saved(void)
{
    g_changed = 0;
}

int packs_key_count(void)
{
    return g_nkeys;
}

const char *packs_key(int i, int *on)
{
    if (i < 0 || i >= g_nkeys)
        return NULL;
    *on = g_keys[i].on;
    return g_keys[i].name;
}

int packs_name_ok(const char *name)
{
    size_t len = strlen(name);
    if (!len || len + 6 >= PACK_NAME_MAX || name[0] == ' ' || name[0] == '\t' || name[0] == '[' || name[len - 1] == ' ' ||
        name[len - 1] == '\t')
        return 0;
    for (const char *c = name; *c; c++)
        if (*c == '=' || *c == '#' || *c == ';' || (unsigned char)*c < 32)
            return 0;
    return 1;
}
