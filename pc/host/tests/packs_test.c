/*
 * The texture packs' list (packs.c): the folders of a packs folder, which are on, and in what order. Folders are made
 * in a temporary folder, one of them named in Japanese (a UTF-8 path, on Windows too).
 *   host_packs_test [TEMPORARY_FOLDER]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "packs.h"

static int bad;

static void Check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        bad++;
    }
}

static int list(const char *dir, char (*names)[PACK_NAME_MAX], char *joined, size_t cap)
{
    int n = packs_enabled(dir, names, 16);
    joined[0] = 0;
    for (int i = 0; i < n; i++) {
        if (i)
            strncat(joined, ",", cap - strlen(joined) - 1);
        strncat(joined, names[i], cap - strlen(joined) - 1);
    }
    return n;
}

int main(int argc, char **argv)
{
    char root[1024], packs[1100], path[1200], joined[1024];
    static char names[16][PACK_NAME_MAX];
    const char *tmp = argc > 1 ? argv[1] : SDL_getenv("TMPDIR");
    if (!tmp)
        tmp = SDL_getenv("TEMP");
    if (!tmp)
        tmp = "/tmp";
    snprintf(root, sizeof(root), "%s/mh2-packs-test", tmp);
    snprintf(packs, sizeof(packs), "%s/packs", root);
    /* (a run that failed halfway leaves its folders: delete mh2-packs-test by hand) */
    const char *dirs[] = {"zebra", "Alpha", "beta", "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88", "off one", ".hidden"};
    SDL_CreateDirectory(root);
    SDL_CreateDirectory(packs);
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", packs, dirs[i]);
        SDL_CreateDirectory(path);
    }
    snprintf(path, sizeof(path), "%s/loose.png", packs);   /* a file is not a pack */
    SDL_IOStream *f = SDL_IOFromFile(path, "wb");
    if (f)
        SDL_CloseIO(f);

    /* no keys: all on, by name (bytes of the UTF-8) */
    packs_reset();
    int n = list(packs, names, joined, sizeof(joined));
    Check(n == 5, "five folders, no key");
    Check(!strcmp(joined, "Alpha,beta,off one,zebra,\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"), "by the bytes of the names");

    /* keys: the file's order first, off ones out, case of ASCII letters not mattering (settings.c lower-cases keys) */
    Check(packs_setting("pack.zebra", "on") == 0, "key zebra");
    Check(packs_setting("pack.off one", "off") == 0, "key off one");
    Check(packs_setting("pack.alpha", "yes") == 0, "key alpha (lower case)");
    Check(packs_setting("pack.\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88", "1") == 0, "key in Japanese");
    Check(packs_setting("pack.gone", "on") == 0, "key with no folder");
    n = list(packs, names, joined, sizeof(joined));
    Check(!strcmp(joined, "zebra,Alpha,\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88,beta"), "the file's order, then the others");
    Check(packs_setting("pack.zebra", "off") == 0, "zebra off, same place");
    n = list(packs, names, joined, sizeof(joined));
    Check(!strcmp(joined, "Alpha,\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88,beta"), "zebra off");
    Check(packs_setting("pack.zebra", "maybe") == -2, "a bad value");
    Check(packs_setting("texture_packs", "on") == -1, "not a pack key");
    Check(packs_setting("pack.", "on") == -1, "no name");

    /* the Content page's view: every pack in order, off ones and a key with no folder too; the folders' own spelling */
    {
        PackInfo all[16];
        int m = packs_list(packs, all, 16);
        Check(m == 6, "six entries: five keys and the folder with none");
        Check(m == 6 && !strcmp(all[0].name, "zebra") && !all[0].on && all[0].present, "zebra, off");
        Check(m == 6 && !strcmp(all[1].name, "off one") && !all[1].on && all[1].present, "off one, off");
        Check(m == 6 && !strcmp(all[2].name, "Alpha") && all[2].on && all[2].present, "Alpha, in the folder's spelling");
        Check(m == 6 && !strcmp(all[3].name, "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88") && all[3].on, "the Japanese name");
        Check(m == 6 && !strcmp(all[4].name, "gone") && all[4].on && !all[4].present, "a key whose folder is gone stays");
        Check(m == 6 && !strcmp(all[5].name, "beta") && all[5].on && all[5].present, "a folder with no key follows, on");
        Check(!packs_changed(), "reading keys is no change");
        /* the page moves Alpha first and turns beta off: the keys follow, and the renderer's list with them */
        PackInfo mine[6] = {all[2], all[0], all[1], all[3], all[4], all[5]};
        mine[5].on = 0;
        packs_set(mine, 6);
        Check(packs_changed() && packs_key_count() == 6, "packs_set: six keys, changed");
        int on = -1;
        Check(!strcmp(packs_key(0, &on), "Alpha") && on == 1, "the first key");
        Check(!strcmp(packs_key(5, &on), "beta") && on == 0, "the last key");
        n = list(packs, names, joined, sizeof(joined));
        Check(!strcmp(joined, "Alpha,\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"), "the renderer's list follows the page's");
        packs_saved();
        Check(!packs_changed(), "saved");
        Check(packs_name_ok("Sharper armour") && packs_name_ok("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"), "names a key can hold");
        Check(!packs_name_ok("a=b") && !packs_name_ok("a #b") && !packs_name_ok("a;b") && !packs_name_ok(" a") && !packs_name_ok("a ") &&
                  !packs_name_ok("[a") && !packs_name_ok(""), "names a key cannot hold");
    }

    /* the limit */
    n = packs_enabled(packs, names, 2);
    Check(n == 2, "at most max");

    /* no folder at all */
    snprintf(path, sizeof(path), "%s/nowhere", root);
    n = packs_enabled(path, names, 16);
    Check(n == 0, "a folder that is not there");

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", packs, dirs[i]);
        SDL_RemovePath(path);
    }
    snprintf(path, sizeof(path), "%s/loose.png", packs);
    SDL_RemovePath(path);
    SDL_RemovePath(packs);
    SDL_RemovePath(root);
    printf(bad ? "packs_test: %d FAILED\n" : "packs_test: ok\n", bad);
    return bad ? 1 : 0;
}
