/*
 * core_kind_test: core_is_content finds the content core's mark in a file (core_kind.h) and only there.
 *
 *   core_kind_test WORKDIR
 *
 * The test writes four files of its own into WORKDIR: with the mark in the middle, with the mark across a 1 MB chunk boundary (the
 * reader's overlap), at the file's very end, and without it (the control: a file that holds the start of the mark and the rest
 * changed). It then asks for a file that does not exist. Nothing of the game is in them. Part of the port (CC0 1.0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_kind.h"

static int failures;

static void expect(const char *what, int got, int want)
{
    if (got != want) {
        printf("core_kind_test: %s: %d, expected %d\n", what, got, want);
        failures++;
    }
}

/* A file of SIZE bytes of a repeating pattern with TEXT put at OFFSET. */
static void make(const char *path, size_t size, size_t offset, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("core_kind_test: cannot write %s\n", path);
        exit(2);
    }
    unsigned char *b = (unsigned char *)malloc(size);
    for (size_t i = 0; i < size; i++)
        b[i] = (unsigned char)("MH2-CORE-KIND-"[i % 14]);   /* looks like the start of the mark, over and over */
    memcpy(b + offset, text, strlen(text));
    fwrite(b, 1, size, f);
    free(b);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: core_kind_test WORKDIR\n");
        return 2;
    }
    char mark[40];
    snprintf(mark, sizeof(mark), "%s%s%s", "MH2-CORE", "-KIND-", "CONTENT-1");
    size_t m = strlen(mark);
    char p[1200];
    const size_t MB = 1u << 20;
    snprintf(p, sizeof(p), "%s/mid.bin", argv[1]);
    make(p, 3 * MB + 77, MB + 1000, mark);
    expect("the mark in the middle", core_is_content(p), 1);
    snprintf(p, sizeof(p), "%s/edge.bin", argv[1]);
    make(p, 3 * MB + 77, MB - m / 2, mark);   /* across the first chunk's end */
    expect("the mark across a chunk boundary", core_is_content(p), 1);
    snprintf(p, sizeof(p), "%s/end.bin", argv[1]);
    make(p, 2 * MB + 5, 2 * MB + 5 - m, mark);
    expect("the mark at the end", core_is_content(p), 1);
    snprintf(p, sizeof(p), "%s/none.bin", argv[1]);
    make(p, 3 * MB + 77, MB - m / 2, "MH2-CORE-KIND-BASE-1");   /* the control: the same place, another text */
    expect("a file without the mark", core_is_content(p), 0);
    snprintf(p, sizeof(p), "%s/missing.bin", argv[1]);
    expect("a file that is not there", core_is_content(p), 0);
    expect("no path", core_is_content(NULL), 0);
    printf(failures ? "core_kind_test: FAILED (%d)\n" : "core_kind_test: OK\n", failures);
    return failures ? 1 : 0;
}
