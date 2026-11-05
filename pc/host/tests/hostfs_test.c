/*
 * hostfs_test: mh2's files, folders and environment with names outside the ANSI code page (hostfs.h).
 *
 *   hostfs_test WORKDIR WORD
 *
 * WORD is a word of three katakana (TE SU TO), which the test's runner gives on the command line: on Windows it arrives as
 * UTF-16 and hfs_args hands it on as UTF-8, so it must equal the bytes this file holds. The test then makes a folder and
 * files with such names (hfs_fopen, hfs_rename, hfs_remove; SDL for the folder), sets an environment variable to a path with
 * such a name and reads it back (hfs_getenv), and, on Windows, reads a string in the ANSI code page (hfs_utf8).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hostfs.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

static int exists(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

int main(int argc, char **argv)
{
    static const char word[] = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";   /* TE SU TO */
    hfs_args(&argc, &argv);
    check(argc == 3, "argc");
    if (argc != 3)
        return 1;
    check(!strcmp(argv[2], word), "the command line's word arrives as UTF-8");

    char dir[1200], a[1300], b[1300];
    snprintf(dir, sizeof(dir), "%s/%s", argv[1], word);
    snprintf(a, sizeof(a), "%s/%s.txt", dir, word);
    snprintf(b, sizeof(b), "%s/%s2.txt", dir, word);
    check(SDL_CreateDirectory(dir), "make the folder");
    FILE *f = hfs_fopen(a, "wb");
    check(f != NULL, "open for writing");
    if (f) {
        fputs("one\n", f);
        fclose(f);
    }
    check(exists(a), "SDL sees the file hfs_fopen made");
    f = hfs_fopen(a, "wx");
    check(f == NULL, "wx refuses an existing file");
    if (f)
        fclose(f);
    check(hfs_rename(a, b) == 0, "rename");
    check(!exists(a) && exists(b), "the file moved");
    f = hfs_fopen(b, "rb");
    char line[16] = "";
    check(f && fgets(line, sizeof(line), f) && !strcmp(line, "one\n"), "read it back");
    if (f)
        fclose(f);
    check(hfs_remove(b) == 0 && !exists(b), "remove");
    check(hfs_fopen(b, "rb") == NULL, "a removed file does not open");
    SDL_RemovePath(dir);

    char value[1300];
    snprintf(value, sizeof(value), "%s/%s", argv[1], word);
#ifdef _WIN32
    wchar_t *wv = hfs_wide(value);
    check(wv && _wputenv_s(L"MH2_HFS_TEST", wv) == 0, "set the variable");
    free(wv);
#else
    check(setenv("MH2_HFS_TEST", value, 1) == 0, "set the variable");
#endif
    const char *got = hfs_getenv("MH2_HFS_TEST");
    check(got && !strcmp(got, value), "the variable is read as it was set");
    check(hfs_getenv("MH2_HFS_NO_SUCH_VARIABLE") == NULL, "an unset variable is NULL");

    char out[64];
    const char *same = hfs_utf8(word, out, sizeof(out));
    check(same == word, "valid UTF-8 is left alone");
    const char *ansi = hfs_utf8("caf\xE9", out, sizeof(out));
#ifdef _WIN32
    check(ansi == out && strlen(out) >= 4 && (unsigned char)out[0] == 'c', "an ANSI string is converted");
    check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out, -1, NULL, 0) > 0,
          "the conversion is valid UTF-8");
#else
    check(!strcmp(ansi, "caf\xE9"), "elsewhere a string is left alone");
#endif
    printf(fails ? "hostfs_test: %d FAILED\n" : "hostfs_test: ok\n", fails);
    return fails ? 1 : 0;
}
