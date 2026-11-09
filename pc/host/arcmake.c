/*
 * The disc archive's maker (arcmake.h). The archive's format and its integrity rule are tools/port/make_archive.py's, whose
 * output this writes byte for byte; the ISO 9660 reader is written from ECMA-119 (the volume descriptors of clause 8, the
 * directory records of clause 9, the file identifiers of clause 7.5) and reads only what the archive needs: the primary
 * volume descriptor, and the directories on the way to the files of the table.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <sys/types.h>
#endif

#include "arcmake.h"
#include "hostfs.h"
#include "sha1.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

#define SECTOR 2048u
#define CHUNK (8u << 20)
#define HEAD_SIZE 32u
#define ENTRY_SIZE 128u
#define MAGIC "MH2DARC"   /* and a zero byte */

/* ---------------------------------------------------------------- the tables */

static const struct {
    const char *path;
    uint32_t lba, size;
    const char *sha1;
} k_files[] = {
#include "arc_tables.inc"
};

static int hex20(const char *s, uint8_t out[20])
{
    for (int i = 0; i < 20; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            unsigned d;
            if (c >= '0' && c <= '9')
                d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = (unsigned)(c - 'A' + 10);
            else
                return -1;
            v = v << 4 | d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void to_hex(const uint8_t *b, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[2 * i] = digits[b[i] >> 4];
        out[2 * i + 1] = digits[b[i] & 15];
    }
    out[40] = 0;
}

void arc_builtin(ArcTable *t)
{
    memset(t, 0, sizeof(*t));
    for (size_t i = 0; i < sizeof(k_files) / sizeof(k_files[0]) && i < ARC_MAXFILES; i++) {
        ArcFile *f = &t->f[t->n++];
        snprintf(f->path, sizeof(f->path), "%s", k_files[i].path);
        f->lba = k_files[i].lba;
        f->size = k_files[i].size;
        hex20(k_files[i].sha1, f->sha1);
    }
}


/* ---------------------------------------------------------------- the plan: the header, the offsets, the size */

typedef struct Plan {
    int n;
    ArcFile f[ARC_MAXFILES];   /* by sector */
    uint64_t off[ARC_MAXFILES];
    uint64_t start, total, grand;
} Plan;

static uint32_t sectors_of(uint32_t size)
{
    return (uint32_t)(((uint64_t)size + SECTOR - 1) / SECTOR);
}

static int plan_make(const ArcTable *t, Plan *p)
{
    memset(p, 0, sizeof(*p));
    if (t->n < 1 || t->n > ARC_MAXFILES)
        return -1;
    p->n = t->n;
    memcpy(p->f, t->f, sizeof(ArcFile) * (size_t)t->n);
    for (int i = 1; i < p->n; i++) {   /* by sector (the stable insertion sort) */
        ArcFile x = p->f[i];
        int k = i;
        while (k > 0 && p->f[k - 1].lba > x.lba) {
            p->f[k] = p->f[k - 1];
            k--;
        }
        p->f[k] = x;
    }
    for (int i = 0; i + 1 < p->n; i++)
        if ((uint64_t)p->f[i].lba + sectors_of(p->f[i].size) > p->f[i + 1].lba)
            return -1;
    for (int i = 0; i < p->n; i++)
        if (!p->f[i].path[0] || strlen(p->f[i].path) > 78)
            return -1;
    p->start = ((uint64_t)HEAD_SIZE + (uint64_t)ENTRY_SIZE * (uint64_t)p->n + SECTOR - 1) / SECTOR * SECTOR;
    uint64_t pos = p->start;
    for (int i = 0; i < p->n; i++) {
        p->off[i] = pos;
        pos += (uint64_t)sectors_of(p->f[i].size) * SECTOR;
        p->grand += p->f[i].size;
    }
    p->total = pos;
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

/* The header and entries (HEAD_SIZE + n * ENTRY_SIZE bytes) into B. */
static size_t plan_head(const Plan *p, uint8_t *b)
{
    size_t len = HEAD_SIZE + (size_t)ENTRY_SIZE * (size_t)p->n;
    memset(b, 0, len);
    memcpy(b, MAGIC, 7);
    put32(b + 8, 1);
    put32(b + 12, (uint32_t)p->n);
    put32(b + 16, ENTRY_SIZE);
    put32(b + 20, (uint32_t)p->start);
    put64(b + 24, p->total);
    for (int i = 0; i < p->n; i++) {
        uint8_t *e = b + HEAD_SIZE + (size_t)ENTRY_SIZE * (size_t)i;
        e[0] = '/';
        for (const char *s = p->f[i].path; *s; s++)
            e[1 + (s - p->f[i].path)] = (uint8_t)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);   /* the disc path, upper case */
        put32(e + 80, p->f[i].lba);
        put32(e + 84, p->f[i].size);
        put64(e + 88, p->off[i]);
        memcpy(e + 96, p->f[i].sha1, 20);
    }
    return len;
}

uint64_t arc_size(const ArcTable *t)
{
    Plan p;
    return plan_make(t, &p) == 0 ? p.total : 0;
}

size_t arc_head(const ArcTable *t, uint8_t *head, size_t cap)
{
    Plan p;
    if (plan_make(t, &p) != 0 || cap < HEAD_SIZE + (size_t)ENTRY_SIZE * (size_t)p.n)
        return 0;
    return plan_head(&p, head);
}

/* ---------------------------------------------------------------- files and folders */

static int seek64(FILE *f, uint64_t pos)
{
#ifdef _WIN32
    return _fseeki64(f, (__int64)pos, SEEK_SET);
#else
    return fseeko(f, (off_t)pos, SEEK_SET);
#endif
}

static int is_file(const char *path, uint64_t *size)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
        return 0;
    if (size)
        *size = (uint64_t)info.size;
    return 1;
}

static int is_dir(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    int slash = n && (dir[n - 1] == '/' || dir[n - 1] == '\\');
    snprintf(out, cap, "%s%s%s", dir, slash ? "" : SEP, name);
}

typedef struct Names {
    char (*v)[300];
    int n, cap;
    int oom;
} Names;

static SDL_EnumerationResult SDLCALL names_cb(void *ud, const char *dirname, const char *fname)
{
    Names *s = ud;
    (void)dirname;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        void *v = realloc(s->v, sizeof(s->v[0]) * (size_t)cap);
        if (!v) {
            s->oom = 1;
            return SDL_ENUM_FAILURE;
        }
        s->v = v;
        s->cap = cap;
    }
    snprintf(s->v[s->n++], sizeof(s->v[0]), "%s", fname);
    return SDL_ENUM_CONTINUE;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(a, b);
}

/* The names in DIR, in byte order (the order of a sorted listing). */
static void list_dir(const char *dir, Names *s)
{
    memset(s, 0, sizeof(*s));
    SDL_EnumerateDirectory(dir, names_cb, s);
    if (s->n)
        qsort(s->v, (size_t)s->n, sizeof(s->v[0]), cmp_names);
}

static int has_iso_suffix(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && !SDL_strcasecmp(name + n - 4, ".iso");
}

/* ---------------------------------------------------------------- the source */

typedef struct Source {
    int iso;
    char path[1200];   /* the image, or the folder with the disc's files */
} Source;

static void fail(ArcResult *r, int code, const char *file, const char *where)
{
    r->code = code;
    if (file)
        snprintf(r->file, sizeof(r->file), "%s", file);
    if (where)
        snprintf(r->where, sizeof(r->where), "%s", where);
}

static void fail_errno(ArcResult *r, int code, const char *where)
{
    fail(r, code, NULL, where);
    snprintf(r->detail, sizeof(r->detail), "%s", strerror(errno));
}

/* GIVEN: a file is a disc image; a folder is the disc's files if it has a SYSTEM.CNF, else the first image in it or in the folders in it. */
static int find_source(const char *given, Source *s, ArcResult *r)
{
    if (is_file(given, NULL)) {
        s->iso = 1;
        snprintf(s->path, sizeof(s->path), "%s", given);
        return 0;
    }
    if (!is_dir(given)) {
        fail(r, AM_E_NOSOURCE, NULL, given);
        return -1;
    }
    Names top;
    list_dir(given, &top);
    int found = 0;
    for (int i = 0; i < top.n && !found; i++)
        if (!SDL_strcasecmp(top.v[i], "SYSTEM.CNF")) {
            s->iso = 0;
            snprintf(s->path, sizeof(s->path), "%s", given);
            found = 1;
        }
    for (int i = 0; i < top.n && !found; i++) {
        char full[1500];
        join(full, sizeof(full), given, top.v[i]);
        if (has_iso_suffix(top.v[i]) && is_file(full, NULL)) {
            s->iso = 1;
            snprintf(s->path, sizeof(s->path), "%s", full);
            found = 1;
        } else if (is_dir(full)) {
            Names sub;
            list_dir(full, &sub);
            for (int k = 0; k < sub.n && !found; k++) {
                char inner[1800];
                join(inner, sizeof(inner), full, sub.v[k]);
                if (has_iso_suffix(sub.v[k]) && is_file(inner, NULL)) {
                    s->iso = 1;
                    snprintf(s->path, sizeof(s->path), "%s", inner);
                    found = 1;
                }
            }
            free(sub.v);
        }
    }
    free(top.v);
    if (!found) {
        fail(r, AM_E_NODISC, NULL, given);
        return -1;
    }
    return 0;
}

/* The file PATH ("A/B/C", any case) under ROOT in a folder of the disc's files: its host path, or 0 when there is none. */
static int find_extracted(const char *root, const char *path, char *out, size_t cap, uint64_t *size)
{
    char cur[1500];
    snprintf(cur, sizeof(cur), "%s", root);
    const char *p = path;
    while (*p) {
        char comp[120];
        size_t n = strcspn(p, "/");
        if (n == 0 || n >= sizeof(comp))
            return 0;
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;
        if (*p == '/')
            p++;
        int last = *p == 0;
        char next[1800];
        join(next, sizeof(next), cur, comp);
        if (last ? is_file(next, NULL) : is_dir(next)) {
            snprintf(cur, sizeof(cur), "%s", next);
            continue;
        }
        Names names;   /* not under this spelling: look for the name in any case */
        list_dir(cur, &names);
        int hit = -1;
        for (int i = 0; i < names.n && hit < 0; i++)
            if (!SDL_strcasecmp(names.v[i], comp))
                hit = i;
        if (hit < 0) {
            free(names.v);
            return 0;
        }
        join(next, sizeof(next), cur, names.v[hit]);
        free(names.v);
        if (!(last ? is_file(next, NULL) : is_dir(next)))
            return 0;
        snprintf(cur, sizeof(cur), "%s", next);
    }
    if (!is_file(cur, size))
        return 0;
    snprintf(out, cap, "%s", cur);
    return 1;
}

/* ---------------------------------------------------------------- ISO 9660 (ECMA-119) */

typedef struct Iso {
    FILE *f;
    uint32_t root_lba, root_size;
} Iso;

static int iso_read(Iso *iso, uint64_t sector, void *buf, size_t n)
{
    if (seek64(iso->f, sector * SECTOR) != 0)
        return -1;
    return fread(buf, 1, n, iso->f) == n ? 0 : -1;
}

/* The primary volume descriptor (clause 8.4): the descriptors begin at sector 16, each with the identifier CD001 and a type;
 * the logical block size (8.4.12) must be 2048, and the root directory's record (8.4.18) says where the files begin. */
static int iso_open(Iso *iso, const char *path, ArcResult *r)
{
    iso->f = hfs_fopen(path, "rb");
    if (!iso->f) {
        fail_errno(r, AM_E_OPEN, path);
        return -1;
    }
    uint8_t d[SECTOR];
    for (uint64_t s = 16; s < 16 + 32; s++) {
        if (iso_read(iso, s, d, sizeof(d)) != 0 || memcmp(d + 1, "CD001", 5) != 0 || d[6] != 1)
            break;
        if (d[0] == 255)
            break;   /* the terminator, with no primary descriptor before it */
        if (d[0] != 1)
            continue;
        uint32_t block = (uint32_t)d[128] | (uint32_t)d[129] << 8;
        const uint8_t *root = d + 156;
        if (block != SECTOR || root[0] < 34 || !(root[25] & 2))
            break;
        iso->root_lba = get32(root + 2);
        iso->root_size = get32(root + 10);
        return 0;
    }
    fclose(iso->f);
    iso->f = NULL;
    fail(r, AM_E_NOTISO, NULL, path);
    return -1;
}

static char up(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* A file identifier (7.5): NAME.EXT;VERSION. Equal to COMP without its version, and without a lone dot at the end. */
static int ident_is(const uint8_t *id, unsigned len, const char *comp)
{
    unsigned n = 0;
    while (n < len && id[n] != ';')
        n++;
    if (n && id[n - 1] == '.')
        n--;
    size_t cn = strlen(comp);
    if (n != cn)
        return 0;
    for (size_t i = 0; i < cn; i++)
        if (up((char)id[i]) != up(comp[i]))
            return 0;
    return 1;
}

/* PATH ("DIR/DIR/NAME") on the image: 1 found (LBA and SIZE filled), 0 not there, -1 not readable. */
static int iso_find(Iso *iso, const char *path, uint32_t *lba, uint32_t *size)
{
    uint32_t dir_lba = iso->root_lba, dir_size = iso->root_size;
    const char *p = path;
    while (*p) {
        char comp[120];
        size_t n = strcspn(p, "/");
        if (n == 0 || n >= sizeof(comp))
            return 0;
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;
        if (*p == '/')
            p++;
        int last = *p == 0;
        if (dir_size == 0 || dir_size > (16u << 20))
            return -1;
        uint8_t *dir = malloc(dir_size);
        if (!dir)
            return -1;
        if (iso_read(iso, dir_lba, dir, dir_size) != 0) {
            free(dir);
            return -1;
        }
        int hit = 0;
        for (uint32_t off = 0; off < dir_size;) {   /* the directory records (9.1): none crosses a sector */
            unsigned len = dir[off];
            if (len == 0) {
                off = (off / SECTOR + 1) * SECTOR;
                continue;
            }
            if (len < 34 || off + len > dir_size || 33u + dir[off + 32] > len)
                break;
            const uint8_t *rec = dir + off;
            unsigned idlen = rec[32];
            if (!(idlen == 1 && rec[33] <= 1) && ident_is(rec + 33, idlen, comp)) {
                int is_directory = (rec[25] & 2) != 0;
                if (last != !is_directory) {
                    off += len;
                    continue;
                }
                dir_lba = get32(rec + 2);
                dir_size = get32(rec + 10);
                hit = 1;
                break;
            }
            off += len;
        }
        free(dir);
        if (!hit)
            return 0;
    }
    *lba = dir_lba;
    *size = dir_size;
    return 1;
}

/* ---------------------------------------------------------------- the job */

typedef struct Ctx {
    const ArcJob *job;
    ArcResult *r;
    uint8_t *buf;
    uint64_t done, grand;
    int nfiles;
} Ctx;

static int report(Ctx *c, int file, const char *name)
{
    return c->job->progress ? c->job->progress(c->job->ud, file, c->nfiles, c->done, c->grand, name) : 0;
}

static void dir_of(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    char *cut = NULL;
    for (char *p = out; *p; p++)
        if (*p == '/' || *p == '\\')
            cut = p;
    if (!cut)
        snprintf(out, cap, ".");
    else
        cut[cut == out ? 1 : 0] = 0;
}

uint64_t arc_free_space(const char *path)
{
    char dir[1400];
    snprintf(dir, sizeof(dir), "%s", path);
    for (int tries = 0; tries < 64; tries++) {
#ifdef _WIN32
        wchar_t *w = hfs_wide(dir);
        ULARGE_INTEGER avail;
        int ok = w && GetDiskFreeSpaceExW(w, &avail, NULL, NULL);
        free(w);
        if (ok)
            return avail.QuadPart;
#else
        struct statvfs vs;
        if (statvfs(dir, &vs) == 0)
            return (uint64_t)vs.f_bavail * (uint64_t)vs.f_frsize;
#endif
        char *cut = NULL;
        for (char *s = dir; *s; s++)
            if (*s == '/' || *s == '\\')
                cut = s;
        if (!cut)
            break;
        if (cut == dir) {   /* the root itself */
            cut[1] = 0;
            tries = 62;
        } else {
            *cut = 0;
        }
    }
    return 0;
}

int arc_find(const char *archive, const char *name, uint32_t *lba, uint32_t *size, uint64_t *offset)
{
    FILE *f = hfs_fopen(archive, "rb");
    if (!f)
        return -1;
    int rc = -1;
    uint8_t h[HEAD_SIZE];
    uint64_t fsize = 0;
    if (fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, MAGIC, 8) == 0 && get32(h + 8) == 1 && get32(h + 16) == ENTRY_SIZE &&
        get32(h + 12) <= 4096 && is_file(archive, &fsize) && fsize == get64(h + 24)) {
        uint32_t n = get32(h + 12);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t e[ENTRY_SIZE];
            if (fread(e, 1, sizeof(e), f) != sizeof(e))
                break;
            if (strncmp((const char *)e, name, 80) == 0) {
                *lba = get32(e + 80);
                *size = get32(e + 84);
                *offset = get64(e + 88);
                rc = 0;
                break;
            }
        }
    }
    fclose(f);
    return rc;
}

/* Is OUT already the archive HEAD (its header and entry table) describes? */
static int up_to_date(const char *out, const uint8_t *head, size_t len, uint64_t total)
{
    uint64_t fsize;
    if (!is_file(out, &fsize) || fsize != total)
        return 0;
    FILE *f = hfs_fopen(out, "rb");
    if (!f)
        return 0;
    uint8_t *b = malloc(len);
    int same = b && fread(b, 1, len, f) == len && memcmp(b, head, len) == 0;
    free(b);
    fclose(f);
    return same;
}

/* A stop asked for while the job waits (the test's slow file) or works. */
static int wait_ms(Ctx *c, int file, const char *name, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        SDL_Delay(10);
        if (report(c, file, name))
            return 1;
    }
    return 0;
}

/* One file's bytes (SRC positioned at its first) to OUT, hashed on the way. 0 done, 1 stopped, -1 failed (R filled). */
static int copy_file(Ctx *c, FILE *src, FILE *out, const ArcFile *f, int idx, const char *from)
{
    Sha1 h;
    sha1_init(&h);
    uint32_t left = f->size;
    while (left) {
        size_t want = left < CHUNK ? left : CHUNK;
        size_t got = fread(c->buf, 1, want, src);
        if (got == 0 && ferror(src)) {
            fail_errno(c->r, AM_E_READ, from);
            snprintf(c->r->file, sizeof(c->r->file), "%s", f->path);
            return -1;
        }
        if (got == 0)
            break;
        sha1_update(&h, c->buf, got);
        if (fwrite(c->buf, 1, got, out) != got) {
            fail_errno(c->r, AM_E_WRITE, c->job->out);
            return -1;
        }
        left -= (uint32_t)got;
        c->done += got;
        if (report(c, idx + 1, f->path))
            return 1;
    }
    if (left) {
        fail(c->r, AM_E_SHORT, f->path, from);
        snprintf(c->r->detail, sizeof(c->r->detail), "short by %u bytes", (unsigned)left);
        return -1;
    }
    uint8_t sum[20];
    sha1_final(&h, sum);
    if (memcmp(sum, f->sha1, 20) != 0) {
        fail(c->r, AM_E_BADSUM, f->path, from);
        to_hex(sum, c->r->detail);
        return -1;
    }
    uint8_t zeros[SECTOR] = {0};
    size_t pad = (size_t)(-(int64_t)f->size & (int64_t)(SECTOR - 1));
    if (pad && fwrite(zeros, 1, pad, out) != pad) {
        fail_errno(c->r, AM_E_WRITE, c->job->out);
        return -1;
    }
    return 0;
}

int arc_make(const ArcJob *job, ArcResult *r)
{
    memset(r, 0, sizeof(*r));
    Plan plan;
    if (plan_make(job->table, &plan) != 0) {
        fail(r, AM_E_TABLE, NULL, NULL);
        return r->code;
    }
    r->total = plan.total;
    uint8_t head[HEAD_SIZE + ENTRY_SIZE * ARC_MAXFILES];
    size_t head_len = plan_head(&plan, head);
    Ctx c = {.job = job, .r = r, .grand = plan.grand, .nfiles = plan.n};
    if (report(&c, 0, "")) {
        r->code = AM_CANCELLED;
        return r->code;
    }
    if (!job->force && up_to_date(job->out, head, head_len, plan.total)) {
        r->up_to_date = 1;
        return AM_OK;
    }
    Source src;
    if (find_source(job->source, &src, r) != 0)
        return r->code;

    /* The source against the table, before anything is written. */
    Iso iso = {0};
    uint64_t folder_size[ARC_MAXFILES];
    char folder_path[ARC_MAXFILES][1500];
    if (src.iso) {
        if (iso_open(&iso, src.path, r) != 0)
            return r->code;
        for (int i = 0; i < plan.n; i++) {
            uint32_t lba = 0, size = 0;
            int found = iso_find(&iso, plan.f[i].path, &lba, &size);
            if (found < 0) {
                fail_errno(r, AM_E_READ, src.path);
                snprintf(r->detail, sizeof(r->detail), "the image's directory cannot be read");
            } else if (!found) {
                fail(r, AM_E_NOTONDISC, plan.f[i].path, src.path);
            } else if (lba != plan.f[i].lba || size != plan.f[i].size) {
                fail(r, AM_E_LAYOUT, plan.f[i].path, src.path);
            }
            if (r->code) {
                fclose(iso.f);
                return r->code;
            }
        }
    } else {
        for (int i = 0; i < plan.n; i++) {
            if (!find_extracted(src.path, plan.f[i].path, folder_path[i], sizeof(folder_path[i]), &folder_size[i])) {
                fail(r, AM_E_MISSING, plan.f[i].path, src.path);
                return r->code;
            }
            if (folder_size[i] != plan.f[i].size) {
                fail(r, AM_E_SIZE, plan.f[i].path, src.path);
                return r->code;
            }
        }
    }

    char tmp[1400], out_dir[1400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", job->out);
    dir_of(job->out, out_dir, sizeof(out_dir));
    FILE *out = NULL;
    uint8_t *zero = NULL;
    char made[6][1400];   /* the folders this run makes, the deepest first: a run that fails takes them away again */
    int nmade = 0;
    c.buf = malloc(CHUNK);
    if (!c.buf) {
        fail(r, AM_E_READ, NULL, NULL);
        snprintf(r->detail, sizeof(r->detail), "out of memory");
        goto done;
    }
    {
        uint64_t have = job->free_bytes ? job->free_bytes : arc_free_space(out_dir);
        if (have && have < plan.total + (plan.total >> 4)) {   /* the file, and a little over */
            fail(r, AM_E_SPACE, NULL, out_dir);
            r->need = plan.total;
            r->have = have;
            goto done;
        }
    }
    {
        char up[1400];
        snprintf(up, sizeof(up), "%s", out_dir);
        SDL_PathInfo pi;
        int n = 0;
        while (n < 6 && up[0] && !SDL_GetPathInfo(up, &pi)) {   /* the folders that are not there yet */
            char parent[1400];
            snprintf(made[n++], sizeof(made[0]), "%s", up);
            dir_of(up, parent, sizeof(parent));
            if (strlen(parent) >= strlen(up))
                break;
            snprintf(up, sizeof(up), "%s", parent);
        }
        if (!SDL_CreateDirectory(out_dir)) {
            fail(r, AM_E_WRITE, NULL, out_dir);
            snprintf(r->detail, sizeof(r->detail), "%s", SDL_GetError());
            for (int i = 0; i < n; i++)   /* (some of them may have been made before it failed) */
                SDL_RemovePath(made[i]);
            goto done;
        }
        nmade = n;
    }
    out = hfs_fopen(tmp, "wb");
    if (!out) {
        fail_errno(r, AM_E_WRITE, tmp);
        goto done;
    }
    zero = calloc(1, (size_t)plan.start);
    if (!zero) {
        fail(r, AM_E_WRITE, NULL, tmp);
        goto done;
    }
    memcpy(zero, head, head_len);
    if (fwrite(zero, 1, (size_t)plan.start, out) != (size_t)plan.start) {
        fail_errno(r, AM_E_WRITE, tmp);
        goto done;
    }
    for (int i = 0; i < plan.n && !r->code; i++) {
        if (report(&c, i + 1, plan.f[i].path) || (job->slow_file == i + 1 && job->slow_ms && wait_ms(&c, i + 1, plan.f[i].path, job->slow_ms))) {
            r->code = AM_CANCELLED;
            break;
        }
        FILE *s;
        int rc;
        if (src.iso) {
            seek64(iso.f, (uint64_t)plan.f[i].lba * SECTOR);
            rc = copy_file(&c, iso.f, out, &plan.f[i], i, src.path);
            if (rc == 0) {   /* the rest of its last sector is zero on the disc too */
                uint8_t tail[SECTOR];
                size_t pad = (size_t)(-(int64_t)plan.f[i].size & (int64_t)(SECTOR - 1));
                size_t got = pad ? fread(tail, 1, pad, iso.f) : 0;
                for (size_t k = 0; k < got; k++)
                    if (tail[k]) {
                        fail(r, AM_E_TAIL, plan.f[i].path, src.path);
                        rc = -1;
                        break;
                    }
            }
        } else {
            s = hfs_fopen(folder_path[i], "rb");
            if (!s) {
                fail_errno(r, AM_E_OPEN, folder_path[i]);
                snprintf(r->file, sizeof(r->file), "%s", plan.f[i].path);
                rc = -1;
            } else {
                rc = copy_file(&c, s, out, &plan.f[i], i, folder_path[i]);
                fclose(s);
            }
        }
        if (rc > 0)
            r->code = AM_CANCELLED;
        else if (rc < 0 && !r->code)
            r->code = AM_E_READ;
    }
    if (!r->code) {
        int bad = fflush(out) != 0 || ferror(out);
        bad |= fclose(out) != 0;
        out = NULL;
        if (bad) {
            fail_errno(r, AM_E_WRITE, tmp);
        } else if (!SDL_RenamePath(tmp, job->out)) {
            fail(r, AM_E_WRITE, NULL, job->out);
            snprintf(r->detail, sizeof(r->detail), "%s", SDL_GetError());
        }
    }
done:
    if (out)
        fclose(out);
    if (r->code) {
        hfs_remove(tmp);
        for (int i = 0; i < nmade; i++)   /* a folder that holds something else stays (SDL removes only an empty one) */
            SDL_RemovePath(made[i]);
    }
    if (iso.f)
        fclose(iso.f);
    free(zero);
    free(c.buf);
    return r->code;
}
