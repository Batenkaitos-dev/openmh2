/*
 * Texture packs (packs.c): the folders under <textures>/packs, each one pack, and which of them are on.
 * mh2.ini has one key per pack, `pack.NAME = on|off` (NAME: the folder's name; a pack with no key is on).
 * The order of the keys in the file is the load order: when two packs have a file for one texture, the earlier
 * pack's is drawn. Packs with no key follow, in the byte order of their names. The renderer takes the list
 * (mh2gs_config.texture_pack_dirs); the launcher's Content page reads and writes the same keys.
 */
#ifndef MH2_PACKS_H
#define MH2_PACKS_H

#define PACK_NAME_MAX 256

/* One ini line. key is the line's key as settings.c holds it (lower case); 0 taken, -1 not a pack key,
 * -2 a value that is not on or off. */
int packs_setting(const char *key, const char *value);
/* Forget the file's keys (the settings are read again). */
void packs_reset(void);
/* The enabled packs under dir (a UTF-8 path), in load order: the folders' names, as the file system spells them.
 * Names starting with a dot, and files, are not packs. Returns how many (at most max). */
int packs_enabled(const char *dir, char (*names)[PACK_NAME_MAX], int max);

/* The launcher's Content page edits the same list. */
typedef struct PackInfo {
    char name[PACK_NAME_MAX];   /* the folder's name as the file system spells it (a pack with no folder: as the file spells it) */
    int on;
    int present;                /* 0: the file has a line for it and the folder is gone */
} PackInfo;
/* Every pack in load order, on or off: the file's keys first, in the file's order (a key whose folder is gone stays, with
 * present 0), then the folders the file does not name, on, by name. Returns how many (at most max). */
int packs_list(const char *dir, PackInfo *out, int max);
/* The player's list replaces the file's keys: every entry gets a key, in this order. Marks the keys changed. */
void packs_set(const PackInfo *list, int n);
/* Whether packs_set changed the keys since they were read or saved (settings_save writes them, then packs_saved). */
int packs_changed(void);
void packs_saved(void);
/* The keys as they stand, in order: for the writer. */
int packs_key_count(void);
const char *packs_key(int i, int *on);
/* Whether a name can be a key of mh2.ini (no =, #, ;, line break or edge spaces, and not a [ section). */
int packs_name_ok(const char *name);

#endif
