/*
 * The kind of core the host starts (core_kind.h). The content core carries a mark in its own file; the host looks for it before the core
 * runs, because the renderer's memory is chosen first. Part of the port (CC0 1.0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_kind.h"
#include "hostfs.h"

int core_is_content(const char *path)
{
    /* the text is put together here so that this program's own file does not hold it whole */
    char mark[40];
    snprintf(mark, sizeof(mark), "%s%s%s", "MH2-CORE", "-KIND-", "CONTENT-1");
    size_t m = strlen(mark);
    FILE *f = path ? hfs_fopen(path, "rb") : NULL;
    if (!f)
        return 0;
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = (unsigned char *)malloc(CHUNK + 64);
    int found = 0;
    size_t keep = 0;   /* the end of the last chunk that a mark could begin in */
    while (buf && !found) {
        size_t n = fread(buf + keep, 1, CHUNK, f);
        if (n == 0)
            break;
        size_t total = keep + n;
        for (size_t i = 0; i + m <= total && !found; i++)
            if (buf[i] == (unsigned char)mark[0] && !memcmp(buf + i, mark, m))
                found = 1;
        keep = total < m - 1 ? total : m - 1;
        memmove(buf, buf + total - keep, keep);
    }
    free(buf);
    fclose(f);
    return found;
}
