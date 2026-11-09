/*
 * The text pack's pack.ini (pc/ipc/packini.h) and the check mh2 makes of a chosen pack before the game starts (textpack.c).
 * Folders are made in a temporary folder, one of them named in Japanese (a UTF-8 path, on Windows too). Only placeholder
 * words are written.
 *   host_textpack_test [TEMPORARY_FOLDER]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "hostfs.h"
#include "ipc/packini.h"
#include "textpack.h"

static int bad;

static void Check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        bad++;
    }
}

static int parse(const char *text, PackIni *pi, char *why)
{
    return PackIni_Parse(text, (unsigned long)strlen(text), pi, why, 300);
}

static void put(const char *dir, const char *name, const char *text)
{
    char path[1400];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = hfs_fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    PackIni pi;
    char why[300], root[1024], dir[1200], sub[1300];
    const char *tmp = argc > 1 ? argv[1] : SDL_getenv("TMPDIR");
    if (!tmp)
        tmp = SDL_getenv("TEMP");
    if (!tmp)
        tmp = "/tmp";

    /* the parser */
    Check(parse("name = One\n", &pi, why) == 0 && !strcmp(pi.name, "One") && !pi.hasCharmap && !strcmp(pi.charmap, "charmap.txt"),
          "a name alone");
    Check(parse("\xEF\xBB\xBF# a comment\r\n; another\r\n\r\n  name\t=  Two words \r\nlanguage=pt-BR\r\nversion = 1.2\r\ncharmap = map.txt",
                &pi, why) == 0 && !strcmp(pi.name, "Two words") && !strcmp(pi.language, "pt-BR") && !strcmp(pi.version, "1.2") &&
              pi.hasCharmap && !strcmp(pi.charmap, "map.txt"),
          "a BOM, comments, CRLF, blanks around the = and no last line feed");
    Check(parse("name = Caf\xC3\xA9 \xE3\x83\x86\n", &pi, why) == 0, "UTF-8 in a name");
    Check(parse("language = xx\n", &pi, why) != 0 && strstr(why, "no name"), "no name");
    Check(parse("name = A\nname = B\n", &pi, why) != 0 && strstr(why, "line 2") && strstr(why, "twice"), "a key twice");
    Check(parse("name = A\ncolour = red\n", &pi, why) != 0 && strstr(why, "line 2") && strstr(why, "colour"), "an unknown key");
    Check(parse("name = A\nno equals\n", &pi, why) != 0 && strstr(why, "line 2"), "a line with no =");
    Check(parse("name =\n", &pi, why) != 0 && strstr(why, "no value"), "an empty value");
    Check(parse("name = A\xFF\n", &pi, why) != 0 && strstr(why, "UTF-8"), "bytes that are not UTF-8");
    Check(parse("name = A\xC0\x80\n", &pi, why) != 0, "an overlong form");
    Check(parse("name = A\xED\xA0\x80\n", &pi, why) != 0, "a surrogate");
    Check(parse("name = A\x01\n", &pi, why) != 0 && strstr(why, "control"), "a control character");
    Check(parse("name = A\ncharmap = ../x.txt\n", &pi, why) != 0, "a charmap with a folder in it");
    Check(parse("name = A\ncharmap = a/b\n", &pi, why) != 0, "a charmap with a slash");
    Check(parse("name = A\ncharmap = a:b\n", &pi, why) != 0, "a charmap with a colon");
    Check(parse("name = A\ncharmap = .hid\n", &pi, why) != 0, "a charmap with a leading dot");
    {
        char longname[100];
        memcpy(longname, "name = ", 7);
        memset(longname + 7, 'x', 65);
        longname[72] = 0;
        Check(parse(longname, &pi, why) != 0 && strstr(why, "longer"), "a name of 65 bytes");
        longname[71] = 0;
        Check(parse(longname, &pi, why) == 0 && strlen(pi.name) == 64, "a name of 64 bytes");
    }
    {
        char nul[] = "name = A\0B\n";
        Check(PackIni_Parse(nul, sizeof(nul) - 1, &pi, why, sizeof(why)) != 0 && strstr(why, "NUL"), "a NUL byte");
    }

    /* the check of a folder */
    snprintf(root, sizeof(root), "%s/mh2-textpack-test", tmp);
    SDL_CreateDirectory(root);
    snprintf(dir, sizeof(dir), "%s/%s", root, "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88");
    SDL_CreateDirectory(dir);
    put(dir, "pack.ini", "name = Placeholder\ncharmap = charmap.txt\n");
    Check(textpack_check(dir, why, sizeof(why)) != 0 && strstr(why, "charmap.txt"), "a charmap that is not there");
    put(dir, "charmap.txt", "placeholder\n");
    snprintf(sub, sizeof(sub), "%s/files", dir);
    SDL_CreateDirectory(sub);
    snprintf(sub, sizeof(sub), "%s/files/DATA.BIN", dir);
    SDL_CreateDirectory(sub);
    put(sub, "x", "placeholder");
    Check(textpack_check(dir, why, sizeof(why)) == 0, "a pack in a folder with a Japanese name");
    put(dir, "pack.ini", "name = Placeholder\nbogus line\n");
    Check(textpack_check(dir, why, sizeof(why)) != 0 && strstr(why, "line 2") && strstr(why, dir), "a bad pack.ini names the folder and the line");
    {
        char missing[1300];
        snprintf(missing, sizeof(missing), "%s/nothing-here", root);
        Check(textpack_check(missing, why, sizeof(why)) != 0 && strstr(why, "doesn't exist"), "a folder that is not there");
        snprintf(missing, sizeof(missing), "%s/pack.ini", dir);
        Check(textpack_check(missing, why, sizeof(why)) != 0 && strstr(why, "not a folder"), "a file in place of the folder");
    }
    SDL_CreateDirectory(root);
    snprintf(sub, sizeof(sub), "%s/empty", root);
    SDL_CreateDirectory(sub);
    Check(textpack_check(sub, why, sizeof(why)) != 0 && strstr(why, "no pack.ini"), "a folder with no pack.ini");

    {   /* (the test's files are left in the temporary folder: they hold placeholder words only) */
        char path[1400];
        const char *files[] = {"pack.ini", "charmap.txt", "files/DATA.BIN/x"};
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
            hfs_remove(path);
        }
    }
    printf(bad ? "%d checks failed\n" : "textpack: all checks passed\n", bad);
    return bad ? 1 : 0;
}
