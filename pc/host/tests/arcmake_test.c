/*
 * The disc archive maker's test (arcmake.c, sha1.c, movietable.c), on fakes: a disc made here, with nothing of the game in it
 * (the files hold a generated pattern; the table is the test's own). Also the launcher test's fixture maker:
 *
 *     host_arcmake_test WORKDIR           the cases below
 *     host_arcmake_test --fixtures DIR    the fake discs and tables the launcher's headless cases use (see fixtures())
 *     host_arcmake_test --run SOURCE OUT TABLE [FREE]   one job, its result raw
 *     host_arcmake_test --folder DIR      a good fake disc as a folder
 *     host_arcmake_test --fake-disc DIR   the same, and DIR/../table.txt (the manual proof of a full disk)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcmake.h"
#include "hostfs.h"
#include "movietable.h"
#include "sha1.h"

static int g_fail;

static void check(int ok, const char *fmt, ...)
{
    if (ok)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_fail++;
}

/* ---------------------------------------------------------------- the fake disc */

typedef struct Fake {
    const char *path;
    uint32_t lba, size;
} Fake;

/* The names of the real disc's files (the built-in table's), with small sizes: a sector's end, a sector and a byte, one byte, ... */
static const Fake k_fake[8] = {
    {"SLPM_662.80", 30, 5000},        {"0FLIST.DIR", 40, 43},    {"NTGUIDVD/BIN/BG.RGB", 41, 2048}, {"IOP/MODULES/MH2DRIVE.DAT", 50, 4097},
    {"DATA.BIN", 60, 10000},          {"AFS01.AFS", 80, 1},      {"AFS02.AFS", 90, 2200},           {"AFS00.AFS", 100, 2047},
};
enum { AFS02 = 6 };

/* File K of the fake disc: a pattern, and for AFS02 a header: no movies, or one (garbage the movie library cannot open). */
static void gen(int k, int movies, uint8_t *buf)
{
    for (uint32_t i = 0; i < k_fake[k].size; i++)
        buf[i] = (uint8_t)(k * 31 + i * 7 + (i >> 8));
    if (k == AFS02) {
        memcpy(buf, "AFS\0", 4);
        memset(buf + 4, 0, 12);
        if (movies) {
            buf[4] = 1;
            buf[8] = 16;    /* the entry: offset 16, size 100 */
            buf[12] = 100;
        }
    }
}

static void mkdirs(const char *dir)
{
    SDL_CreateDirectory(dir);
}

static void put_file(const char *dir, const char *rel, const void *data, size_t n)
{
    char path[1500], parent[1500];
    snprintf(path, sizeof(path), "%s/%s", dir, rel);
    snprintf(parent, sizeof(parent), "%s", path);
    char *cut = strrchr(parent, '/');
    if (cut) {
        *cut = 0;
        mkdirs(parent);
    }
    FILE *f = hfs_fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fwrite(data, 1, n, f);
    fclose(f);
}

enum { F_LOWER = 1, F_DAMAGE = 2, F_LONG = 4, F_SHORT = 8, F_NOCNF = 16 };

/* The files in a folder. FLAGS: lower case names; one byte of DATA.BIN changed; DATA.BIN a byte too long; AFS00 a byte too short. */
static void write_folder(const char *dir, int movies, int flags)
{
    mkdirs(dir);
    if (!(flags & F_NOCNF))
        put_file(dir, flags & F_LOWER ? "system.cnf" : "SYSTEM.CNF", "BOOT2 = fake\n", 13);
    for (int k = 0; k < 8; k++) {
        uint8_t *b = calloc(1, k_fake[k].size + 8);
        gen(k, movies, b);
        size_t n = k_fake[k].size;
        if ((flags & F_DAMAGE) && !strcmp(k_fake[k].path, "DATA.BIN"))
            b[100] ^= 1;
        if ((flags & F_LONG) && !strcmp(k_fake[k].path, "DATA.BIN"))
            n++;
        if ((flags & F_SHORT) && !strcmp(k_fake[k].path, "AFS00.AFS"))
            n--;
        char rel[120];
        snprintf(rel, sizeof(rel), "%s", k_fake[k].path);
        if (flags & F_LOWER)
            for (char *p = rel; *p; p++)
                if (*p >= 'A' && *p <= 'Z')
                    *p = (char)(*p + 32);
        put_file(dir, rel, b, n);
        free(b);
    }
}

static void table_of(int movies, ArcTable *t)
{
    memset(t, 0, sizeof(*t));
    for (int k = 0; k < 8; k++) {
        ArcFile *f = &t->f[t->n++];
        uint8_t *b = calloc(1, k_fake[k].size + 8);
        gen(k, movies, b);
        Sha1 h;
        sha1_init(&h);
        sha1_update(&h, b, k_fake[k].size);
        sha1_final(&h, f->sha1);
        free(b);
        snprintf(f->path, sizeof(f->path), "%s", k_fake[k].path);
        f->lba = k_fake[k].lba;
        f->size = k_fake[k].size;
    }
}

static void write_table(const char *file, const ArcTable *t)
{
    char *text = malloc(8192);
    size_t n = 0;
    n += (size_t)snprintf(text + n, 8192 - n, "# a test table: LBA SIZE SHA1 PATH\n");
    for (int i = 0; i < t->n; i++) {
        char hex[41];
        for (int k = 0; k < 20; k++)
            snprintf(hex + 2 * k, 3, "%02x", t->f[i].sha1[k]);
        n += (size_t)snprintf(text + n, 8192 - n, "%u %u %s %s\n", t->f[i].lba, t->f[i].size, hex, t->f[i].path);
    }
    char dir[1500];
    snprintf(dir, sizeof(dir), "%s", file);
    char *cut = strrchr(dir, '/');
    if (cut) {
        *cut = 0;
        mkdirs(dir);
    }
    FILE *f = hfs_fopen(file, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", file);
        exit(2);
    }
    fwrite(text, 1, n, f);
    fclose(f);
    free(text);
}

/* ---- the fake disc image: ISO 9660 as ECMA-119 lays it out */

enum { I_SHORT = 1, I_MOVE = 2, I_NOFILE = 4, I_TAIL = 8, I_DAMAGE = 16, I_BLOCK = 32, I_NOPVD = 64 };

static void le16be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void le32be32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
        p[7 - i] = (uint8_t)(v >> (8 * i));
    }
}

/* One directory record (9.1) at D[*OFF]. NAME NULL: the "." (0) or ".." (1) entry, given by DOT. */
static void put_rec(uint8_t *d, uint32_t *off, const char *name, int dot, uint32_t lba, uint32_t size, int is_dir)
{
    uint8_t *r = d + *off;
    char id[40];
    unsigned idlen;
    if (name) {
        idlen = (unsigned)snprintf(id, sizeof(id), is_dir ? "%s" : "%s;1", name);
    } else {
        id[0] = (char)dot;
        idlen = 1;
    }
    unsigned len = 33 + idlen + ((idlen & 1) ? 0 : 1);
    memset(r, 0, len);
    r[0] = (uint8_t)len;
    le32be32(r + 2, lba);
    le32be32(r + 10, size);
    r[18] = 126;   /* the date: 2026 */
    r[19] = 11;
    r[20] = 4;
    r[25] = is_dir ? 2 : 0;
    le16be16(r + 28, 1);
    r[32] = (uint8_t)idlen;
    memcpy(r + 33, id, idlen);
    *off += len;
}

static void write_iso(const char *path, int movies, int flags)
{
    uint32_t last = 0;
    for (int k = 0; k < 8; k++) {
        uint32_t end = k_fake[k].lba + (k_fake[k].size + 2047) / 2048;
        if (end > last)
            last = end;
    }
    size_t total = (size_t)(last + 1) * 2048;
    uint8_t *img = calloc(1, total);
    uint8_t *pvd = img + 16 * 2048;
    if (!(flags & I_NOPVD)) {
        pvd[0] = 1;
        memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 1;
        memset(pvd + 8, ' ', 32);
        memcpy(pvd + 40, "FAKEDISC", 8);
        le32be32(pvd + 80, last + 1);
        le16be16(pvd + 120, 1);
        le16be16(pvd + 124, 1);
        le16be16(pvd + 128, flags & I_BLOCK ? 512 : 2048);
        uint32_t o = 0;
        put_rec(pvd + 156, &o, NULL, 0, 18, 2048, 1);
        uint8_t *term = img + 17 * 2048;
        term[0] = 255;
        memcpy(term + 1, "CD001", 5);
        term[6] = 1;
    }
    /* sectors 18 root, 19 IOP, 20 IOP/MODULES, 21 NTGUIDVD, 22 NTGUIDVD/BIN */
    uint8_t *root = img + 18 * 2048, *iop = img + 19 * 2048, *mod = img + 20 * 2048, *ntg = img + 21 * 2048, *bin = img + 22 * 2048;
    uint32_t o0 = 0, o1 = 0, o2 = 0, o3 = 0, o4 = 0;
    put_rec(root, &o0, NULL, 0, 18, 2048, 1);
    put_rec(root, &o0, NULL, 1, 18, 2048, 1);
    put_rec(iop, &o1, NULL, 0, 19, 2048, 1);
    put_rec(iop, &o1, NULL, 1, 18, 2048, 1);
    put_rec(mod, &o2, NULL, 0, 20, 2048, 1);
    put_rec(mod, &o2, NULL, 1, 19, 2048, 1);
    put_rec(ntg, &o3, NULL, 0, 21, 2048, 1);
    put_rec(ntg, &o3, NULL, 1, 18, 2048, 1);
    put_rec(bin, &o4, NULL, 0, 22, 2048, 1);
    put_rec(bin, &o4, NULL, 1, 21, 2048, 1);
    put_rec(root, &o0, "IOP", 0, 19, 2048, 1);
    put_rec(root, &o0, "NTGUIDVD", 0, 21, 2048, 1);
    put_rec(iop, &o1, "MODULES", 0, 20, 2048, 1);
    put_rec(ntg, &o3, "BIN", 0, 22, 2048, 1);
    for (int k = 0; k < 8; k++) {
        const char *path_k = k_fake[k].path;
        if ((flags & I_NOFILE) && !strcmp(path_k, "DATA.BIN"))
            continue;
        uint32_t lba = k_fake[k].lba + ((flags & I_MOVE) && k == 0 ? 1000 : 0);
        const char *slash = strrchr(path_k, '/');
        const char *leaf = slash ? slash + 1 : path_k;
        if (!strncmp(path_k, "IOP/MODULES/", 12))
            put_rec(mod, &o2, leaf, 0, lba, k_fake[k].size, 0);
        else if (!strncmp(path_k, "NTGUIDVD/BIN/", 13))
            put_rec(bin, &o4, leaf, 0, lba, k_fake[k].size, 0);
        else
            put_rec(root, &o0, leaf, 0, lba, k_fake[k].size, 0);
        uint8_t *b = calloc(1, k_fake[k].size + 8);
        gen(k, movies, b);
        if ((flags & I_DAMAGE) && !strcmp(path_k, "DATA.BIN"))
            b[100] ^= 1;
        if (!((flags & I_MOVE) && k == 0))
            memcpy(img + (size_t)k_fake[k].lba * 2048, b, k_fake[k].size);
        free(b);
    }
    if (flags & I_TAIL)
        img[(size_t)k_fake[0].lba * 2048 + k_fake[0].size + 3] = 1;   /* the rest of SLPM_662.80's last sector */
    if (flags & I_SHORT)
        total = (size_t)k_fake[7].lba * 2048 + 1000;   /* cut off inside AFS00.AFS */
    char dir[1500];
    snprintf(dir, sizeof(dir), "%s", path);
    char *cut = strrchr(dir, '/');
    if (cut) {
        *cut = 0;
        mkdirs(dir);
    }
    FILE *f = hfs_fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fwrite(img, 1, total, f);
    fclose(f);
    free(img);
}

/* ---------------------------------------------------------------- helpers */

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = hfs_fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    for (size_t got; (got = fread(b + len, 1, cap - len, f)) > 0;) {
        len += got;
        if (len == cap)
            b = realloc(b, cap *= 2);
    }
    fclose(f);
    *n = len;
    return b;
}

static int exists(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info);
}

/* The archive the format asks for, built here once more from the table and the fake disc's bytes (the format is the header
 * of tools/port/make_archive.py): the entries by sector, the data on 2048-byte boundaries. */
static uint8_t *expected_archive(const ArcTable *t, int movies, size_t *n)
{
    int order[8];
    for (int i = 0; i < t->n; i++)
        order[i] = i;
    for (int i = 0; i < t->n; i++)
        for (int k = i + 1; k < t->n; k++)
            if (t->f[order[k]].lba < t->f[order[i]].lba) {
                int x = order[i];
                order[i] = order[k];
                order[k] = x;
            }
    size_t start = (32 + 128 * (size_t)t->n + 2047) / 2048 * 2048, total = start;
    for (int i = 0; i < t->n; i++)
        total += (t->f[i].size + 2047) / 2048 * 2048;
    uint8_t *a = calloc(1, total);
    memcpy(a, "MH2DARC", 7);
    a[8] = 1;
    a[12] = (uint8_t)t->n;
    a[16] = 128;
    a[20] = (uint8_t)(start & 255);
    a[21] = (uint8_t)(start >> 8);
    for (int i = 0; i < 8; i++)
        a[24 + i] = (uint8_t)((uint64_t)total >> (8 * i));
    size_t pos = start;
    for (int i = 0; i < t->n; i++) {
        const ArcFile *f = &t->f[order[i]];
        uint8_t *e = a + 32 + 128 * (size_t)i;
        e[0] = '/';
        for (size_t k = 0; f->path[k]; k++)
            e[1 + k] = (uint8_t)(f->path[k] >= 'a' && f->path[k] <= 'z' ? f->path[k] - 32 : f->path[k]);
        for (int b = 0; b < 4; b++) {
            e[80 + b] = (uint8_t)(f->lba >> (8 * b));
            e[84 + b] = (uint8_t)(f->size >> (8 * b));
        }
        for (int b = 0; b < 8; b++)
            e[88 + b] = (uint8_t)((uint64_t)pos >> (8 * b));
        memcpy(e + 96, f->sha1, 20);
        int k;
        for (k = 0; k < 8; k++)
            if (!strcmp(k_fake[k].path, f->path))
                break;
        gen(k, movies, a + pos);
        pos += (f->size + 2047) / 2048 * 2048;
    }
    *n = total;
    return a;
}

typedef struct Stop {
    int calls, at;   /* stop at the call number AT (counted from 0); -1: never */
    int files_seen;
} Stop;

static int stop_cb(void *ud, int file, int nfiles, uint64_t done, uint64_t total, const char *name)
{
    Stop *s = ud;
    (void)nfiles;
    (void)done;
    (void)total;
    (void)name;
    if (file > s->files_seen)
        s->files_seen = file;
    return s->calls++ == s->at;
}

static int run(const char *source, const char *out, const ArcTable *t, int force, uint64_t free_bytes, Stop *stop, ArcResult *r)
{
    ArcJob job = {.table = t, .source = source, .out = out, .force = force, .free_bytes = free_bytes};
    if (stop) {
        job.progress = stop_cb;
        job.ud = stop;
    }
    return arc_make(&job, r);
}

static int clean(const char *out)
{
    char tmp[1600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", out);
    return !exists(out) && !exists(tmp);
}

/* ---------------------------------------------------------------- the cases */

static void test_sha1(void)
{
    static const struct {
        const char *msg;
        int repeat;
        const char *hex;
    } v[] = {
        {"", 1, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", 1, "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {"a", 1000000, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
        {"0123456701234567012345670123456701234567012345670123456701234567", 10, "dea356a2cddd90c7a7ecedc5ebb563934f460452"},
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        Sha1 h;
        uint8_t d[20];
        char hex[41];
        sha1_init(&h);
        for (int k = 0; k < v[i].repeat; k++)
            sha1_update(&h, v[i].msg, strlen(v[i].msg));
        sha1_final(&h, d);
        for (int k = 0; k < 20; k++)
            snprintf(hex + 2 * k, 3, "%02x", d[k]);
        check(!strcmp(hex, v[i].hex), "sha1 vector %zu: %s", i, hex);
    }
    /* the same message in pieces of every size */
    uint8_t msg[300];
    for (size_t i = 0; i < sizeof(msg); i++)
        msg[i] = (uint8_t)(i * 13 + 5);
    Sha1 whole;
    uint8_t want[20];
    sha1_init(&whole);
    sha1_update(&whole, msg, sizeof(msg));
    sha1_final(&whole, want);
    for (size_t piece = 1; piece < 140; piece++) {
        Sha1 h;
        uint8_t d[20];
        sha1_init(&h);
        for (size_t at = 0; at < sizeof(msg); at += piece)
            sha1_update(&h, msg + at, sizeof(msg) - at < piece ? sizeof(msg) - at : piece);
        sha1_final(&h, d);
        check(!memcmp(d, want, 20), "sha1 in pieces of %zu", piece);
    }
}

static void test_repr(void)
{
    static const struct {
        double x;
        const char *s;
    } v[] = {
        {0.0, "0.0"}, {1.5, "1.5"}, {100.0, "100.0"}, {0.1, "0.1"}, {0.0001, "0.0001"}, {0.00001, "1e-05"}, {1e16, "1e+16"},
        {1e15, "1000000000000000.0"}, {123456789012345680.0, "1.2345678901234568e+17"}, {33.36666666666667, "33.36666666666667"},
        {1.0 / 3.0, "0.3333333333333333"}, {2.0 / 3.0, "0.6666666666666666"}, {12345.678, "12345.678"}, {-2.5, "-2.5"},
        {5e-324, "5e-324"}, {1.7976931348623157e308, "1.7976931348623157e+308"}, {1.0, "1.0"}, {0.5, "0.5"},
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        char s[60];
        movietable_repr(v[i].x, s, sizeof(s));
        check(!strcmp(s, v[i].s), "repr %s, wanted %s", s, v[i].s);
    }
    check(movietable_shown(0.0) == 0 && movietable_shown(-1.0) == 0, "shown 0");
    check(movietable_shown(1.0) == 48000, "shown 1.0");
    check(movietable_shown(1.0 / 48000.0) == 1, "shown 1/48000");
    check(movietable_shown(0.5 / 48000.0) == 1, "shown half a frame");
    long long c = movietable_shown(33.36666666666667);
    check((double)c / 48000.0 >= 33.36666666666667 && (double)(c - 1) / 48000.0 < 33.36666666666667, "shown 33.37: %lld", c);
}

static void test_make(const char *work)
{
    char disc[700], folder_up[700], iso[700], out[700], tab[700], dir[700];
    snprintf(disc, sizeof(disc), "%s/disc", work);
    snprintf(iso, sizeof(iso), "%s/disc.iso", work);
    snprintf(out, sizeof(out), "%s/out/mh2-disc.arc", work);
    snprintf(tab, sizeof(tab), "%s/table.txt", work);
    snprintf(folder_up, sizeof(folder_up), "%s/disc_with_iso", work);
    write_folder(disc, 0, 0);
    write_iso(iso, 0, 0);
    ArcTable t;
    table_of(0, &t);
    write_table(tab, &t);
    ArcTable loaded;
    char err[300];
    check(arc_table_load(&loaded, tab, err, sizeof(err)) == 0 && loaded.n == 8 && !memcmp(&loaded.f[3], &t.f[3], sizeof(ArcFile)), "table file");
    ArcResult r;
    size_t want_n, got_n;
    uint8_t *want = expected_archive(&t, 0, &want_n);
    check(arc_size(&t) == want_n, "arc_size %llu, %zu", (unsigned long long)arc_size(&t), want_n);

    /* a folder, an image, a folder that holds an image, a folder with lower case names and no SYSTEM.CNF in capitals */
    const char *sources[] = {disc, iso, folder_up};
    mkdirs(folder_up);
    char inner[800];
    snprintf(inner, sizeof(inner), "%s/sub/game.ISO", folder_up);
    write_iso(inner, 0, 0);
    char lower[700];
    snprintf(lower, sizeof(lower), "%s/lower", work);
    write_folder(lower, 0, F_LOWER);
    for (int i = 0; i < 4; i++) {
        const char *src = i < 3 ? sources[i] : lower;
        int rc = run(src, out, &t, 0, 0, NULL, &r);
        uint8_t *got = rc == AM_OK ? slurp(out, &got_n) : NULL;
        check(rc == AM_OK && got && got_n == want_n && !memcmp(got, want, want_n), "source %d: code %d, size %zu of %zu", i, rc, got ? got_n : 0, want_n);
        free(got);
        check(!exists(out) || run(src, out, &t, 0, 0, NULL, &r) == AM_OK, "again");
        check(r.up_to_date, "source %d: the second run is up to date", i);
        hfs_remove(out);
    }
    /* the sectors of the header, once more: arc_head and arc_find */
    run(disc, out, &t, 0, 0, NULL, &r);
    uint8_t head[32 + 128 * ARC_MAXFILES];
    size_t hn = arc_head(&t, head, sizeof(head));
    check(hn == 32 + 128 * 8 && !memcmp(head, want, hn), "arc_head");
    uint32_t lba, size;
    uint64_t off;
    check(arc_find(out, "/AFS02.AFS", &lba, &size, &off) == 0 && lba == 90 && size == 2200 && off % 2048 == 0, "arc_find");
    check(arc_find(out, "/NOPE", &lba, &size, &off) != 0, "arc_find of nothing");
    /* a stale archive is rewritten with force, and replaced by a new one if its table is another's */
    check(run(disc, out, &t, 1, 0, NULL, &r) == AM_OK && !r.up_to_date, "force");

    /* the movie table: no movies (no library needed) */
    char text[300];
    check(movietable_make(out, NULL, NULL, NULL, NULL, text, sizeof(text)) == MT_OK, "movie table of no movies: %s", text);
    char mt[800];
    snprintf(mt, sizeof(mt), "%s/out/mh2-movies.txt", work);
    size_t mn;
    uint8_t *mtb = slurp(mt, &mn);
    char wantmt[400];
    snprintf(wantmt, sizeof(wantmt),
             "# The movies' timing (tools/pc/movie_table.py): derived from the disc, build/ only.\nmh2-movies 1 afs02 90 2200 0\n");
    check(mtb && mn == strlen(wantmt) && !memcmp(mtb, wantmt, mn), "the movie table's text");
    free(mtb);
    hfs_remove(mt);
    check(movietable_make(disc, NULL, NULL, NULL, NULL, text, sizeof(text)) == MT_E_ARCHIVE, "a folder is not an archive");
    hfs_remove(out);

    /* one movie, no library there: no table, and the words say why */
    ArcTable t1;
    table_of(1, &t1);
    char disc1[700], out1[700], nolib[700];
    snprintf(disc1, sizeof(disc1), "%s/disc1", work);
    snprintf(out1, sizeof(out1), "%s/out1/mh2-disc.arc", work);
    snprintf(nolib, sizeof(nolib), "%s/no-such-folder", work);
    write_folder(disc1, 1, 0);
    check(run(disc1, out1, &t1, 0, 0, NULL, &r) == AM_OK, "a disc with a movie");
    int mrc = movietable_make(out1, nolib, NULL, NULL, NULL, text, sizeof(text));
    check(mrc == MT_E_NOLIB && strstr(text, "movie library"), "no library: %d %s", mrc, text);
    snprintf(mt, sizeof(mt), "%s/out1/mh2-movies.txt", work);
    check(!exists(mt), "no table without the library");

    /* the failures: each names its file, and leaves nothing behind */
    struct {
        const char *name;
        int movies, flags;
        int iso;
        int want;
        const char *file;
    } bad[] = {
        {"folder: no SYSTEM.CNF", 0, F_NOCNF, 0, AM_E_NODISC, NULL},
        {"folder: file too long", 0, F_LONG, 0, AM_E_SIZE, "DATA.BIN"},
        {"folder: file too short", 0, F_SHORT, 0, AM_E_SIZE, "AFS00.AFS"},
        {"folder: damaged", 0, F_DAMAGE, 0, AM_E_BADSUM, "DATA.BIN"},
        {"image: cut off", 0, I_SHORT, 1, AM_E_SHORT, "AFS00.AFS"},
        {"image: file elsewhere", 0, I_MOVE, 1, AM_E_LAYOUT, "SLPM_662.80"},
        {"image: file absent", 0, I_NOFILE, 1, AM_E_NOTONDISC, "DATA.BIN"},
        {"image: damaged", 0, I_DAMAGE, 1, AM_E_BADSUM, "DATA.BIN"},
        {"image: tail", 0, I_TAIL, 1, AM_E_TAIL, "SLPM_662.80"},
        {"image: another block size", 0, I_BLOCK, 1, AM_E_NOTISO, NULL},
        {"image: no descriptor", 0, I_NOPVD, 1, AM_E_NOTISO, NULL},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char src[800];
        snprintf(src, sizeof(src), "%s/bad%zu%s", work, i, bad[i].iso ? ".iso" : "");
        if (bad[i].iso)
            write_iso(src, bad[i].movies, bad[i].flags);
        else
            write_folder(src, bad[i].movies, bad[i].flags);
        int rc = run(src, out, &t, 0, 0, NULL, &r);
        check(rc == bad[i].want, "%s: code %d, wanted %d", bad[i].name, rc, bad[i].want);
        check(!bad[i].file || !strcmp(r.file, bad[i].file), "%s: names %s, wanted %s", bad[i].name, r.file, bad[i].file ? bad[i].file : "");
        check(clean(out), "%s: leaves something behind", bad[i].name);
    }
    /* a missing file of the folder (SYSTEM.CNF present) */
    char missing[800];
    snprintf(missing, sizeof(missing), "%s/missing", work);
    write_folder(missing, 0, 0);
    snprintf(dir, sizeof(dir), "%s/missing/AFS01.AFS", work);
    hfs_remove(dir);
    check(run(missing, out, &t, 0, 0, NULL, &r) == AM_E_MISSING && !strcmp(r.file, "AFS01.AFS") && clean(out), "folder: AFS01.AFS missing");
    /* a damaged file after good ones: the first files' bytes were written and are removed */
    char later[800];
    snprintf(later, sizeof(later), "%s/later", work);
    write_folder(later, 0, F_DAMAGE);
    check(run(later, out, &t, 0, 0, NULL, &r) == AM_E_BADSUM && clean(out), "damaged DATA.BIN, after SLPM_662.80");
    /* not a disc image, not a folder, a folder with nothing, a table that overlaps */
    char junk[800], empty[800], nothing[800];
    snprintf(junk, sizeof(junk), "%s/junk.iso", work);
    put_file(work, "junk.iso", "this is not a disc image", 24);
    snprintf(empty, sizeof(empty), "%s/empty", work);
    mkdirs(empty);
    snprintf(nothing, sizeof(nothing), "%s/nothing-here", work);
    check(run(junk, out, &t, 0, 0, NULL, &r) == AM_E_NOTISO && clean(out), "a text file as an image");
    check(run(empty, out, &t, 0, 0, NULL, &r) == AM_E_NODISC && clean(out), "an empty folder");
    check(run(nothing, out, &t, 0, 0, NULL, &r) == AM_E_NOSOURCE && clean(out), "no such source");
    ArcTable over = t;
    over.f[1].lba = over.f[0].lba;
    check(run(disc, out, &over, 0, 0, NULL, &r) == AM_E_TABLE && clean(out), "overlapping table");
    /* the room: needs the archive and a little over; the archive's folder is not made */
    char nospace[700];
    snprintf(nospace, sizeof(nospace), "%s/nospace/dir/mh2-disc.arc", work);
    check(run(disc, nospace, &t, 0, 20000, NULL, &r) == AM_E_SPACE && r.need == want_n && r.have == 20000, "not enough room");
    snprintf(dir, sizeof(dir), "%s/nospace", work);
    check(!exists(dir), "no folder made when there is no room");
    check(run(disc, out, &t, 0, want_n + (want_n >> 4), NULL, &r) == AM_OK, "exactly enough room");
    hfs_remove(out);
    /* a folder that cannot be made: its parent is a file */
    char blocked[700];
    snprintf(blocked, sizeof(blocked), "%s/junk.iso/sub/mh2-disc.arc", work);
    check(run(disc, blocked, &t, 0, 0, NULL, &r) == AM_E_WRITE, "the archive's folder cannot be made: %d", r.code);

    /* Cancel at every call of the progress callback: the job ends, and nothing is left */
    Stop full = {.at = -1};
    check(run(disc, out, &t, 0, 0, &full, &r) == AM_OK, "a counted run");
    hfs_remove(out);
    int calls = full.calls;
    check(calls > 9, "progress calls %d", calls);
    for (int k = 0; k < calls; k++) {
        Stop s = {.at = k};
        int rc = run(disc, out, &t, 0, 0, &s, &r);
        check(rc == AM_CANCELLED && clean(out), "cancel at call %d of %d: code %d", k, calls, rc);
        Stop si = {.at = k};
        rc = run(iso, out, &t, 0, 0, &si, &r);
        check(rc == AM_CANCELLED && clean(out), "image, cancel at call %d: code %d", k, rc);
    }
    /* a cancelled run does not touch an archive that was there */
    check(run(disc, out, &t, 0, 0, NULL, &r) == AM_OK, "make");
    uint8_t *before = slurp(out, &got_n);
    Stop mid = {.at = 5};
    check(run(disc, out, &t, 1, 0, &mid, &r) == AM_CANCELLED, "cancel a remake");
    uint8_t *after = slurp(out, &mn);
    check(before && after && got_n == mn && !memcmp(before, after, mn), "the old archive stays");
    free(before);
    free(after);
    char tmp[800];
    snprintf(tmp, sizeof(tmp), "%s.tmp", out);
    check(!exists(tmp), "no tmp after the cancel");
    hfs_remove(out);
    free(want);
}

/* The launcher's headless cases: fake discs, and the tables that go with them (see tests/launcher_test.cmake). */
static void fixtures(const char *dir)
{
    char p[900];
    ArcTable t;
    snprintf(p, sizeof(p), "%s", dir);
    mkdirs(p);
    table_of(0, &t);
    snprintf(p, sizeof(p), "%s/table.txt", dir);
    write_table(p, &t);
    table_of(1, &t);
    snprintf(p, sizeof(p), "%s/table_movie.txt", dir);
    write_table(p, &t);
    snprintf(p, sizeof(p), "%s/some_folder", dir);
    write_folder(p, 0, 0);
    snprintf(p, sizeof(p), "%s/movie_folder", dir);
    write_folder(p, 1, 0);
    snprintf(p, sizeof(p), "%s/bad_disc", dir);
    write_folder(p, 0, F_DAMAGE);
    snprintf(p, sizeof(p), "%s/slow_disc", dir);
    write_folder(p, 0, 0);
    snprintf(p, sizeof(p), "%s/empty_folder", dir);
    mkdirs(p);
    snprintf(p, sizeof(p), "%s/disc.iso", dir);
    write_iso(p, 0, 0);
    snprintf(p, sizeof(p), "%s/junk.iso", dir);
    put_file(dir, "junk.iso", "this is not a disc image", 24);
}

int main(int argc, char **argv)
{
    hfs_args(&argc, &argv);
    if (argc >= 3 && !strcmp(argv[1], "--fixtures")) {
        fixtures(argv[2]);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "--run")) {   /* SOURCE OUT TABLE [FREE]: one job, its result raw (the manual proofs) */
        ArcTable t;
        char err[300];
        if (arc_table_load(&t, argv[4], err, sizeof(err)) != 0) {
            fprintf(stderr, "%s\n", err);
            return 2;
        }
        ArcResult r;
        ArcJob job = {.table = &t, .source = argv[2], .out = argv[3], .free_bytes = argc > 5 ? strtoull(argv[5], NULL, 10) : 0};
        int rc = arc_make(&job, &r);
        printf("code %d file '%s' where '%s' detail '%s' need %llu have %llu\n", rc, r.file, r.where, r.detail, (unsigned long long)r.need,
               (unsigned long long)r.have);
        return rc != 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--folder")) {   /* the files of a good fake disc in DIR */
        write_folder(argv[2], 0, 0);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--fake-disc")) {
        char p[900];
        ArcTable t;
        write_folder(argv[2], 0, 0);
        table_of(0, &t);
        snprintf(p, sizeof(p), "%s/../table.txt", argv[2]);
        write_table(p, &t);
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: host_arcmake_test WORKDIR\n");
        return 2;
    }
    mkdirs(argv[1]);
    test_sha1();
    test_repr();
    test_make(argv[1]);
    if (g_fail) {
        fprintf(stderr, "arcmake_test: %d checks failed\n", g_fail);
        return 1;
    }
    printf("arcmake_test: ok\n");
    return 0;
}
