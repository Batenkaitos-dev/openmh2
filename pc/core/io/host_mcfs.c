/* The memory card file system on a raw card image (PCSX2's .ps2 format):
 * the original's mcfs.c, adapted for the core as a
 * host file (io_mcfs.h): the host's C library, Host_Log, and the time stamps
 * from mcfs_set_clock (the kernel's calendar) instead of the original's
 * rt_calendar. The rest is the original's, unchanged. Its comment follows.
 *
 * PS2 memory card file system on a raw card image (PCSX2's .ps2 format).
 *
 * The image is a sequence of 528-byte pages: 512 data bytes, then 16 spare
 * bytes holding a Hamming ECC (3 bytes per 128-byte chunk, then 4 zeros).
 * On a standard 8 MB card, 2 pages make a 1 KiB cluster and 16 pages an
 * 8 KiB erase block:
 *   cluster 0        superblock ("Sony PS2 Memory Card Format 1.2.0.0")
 *   cluster 8        indirect FAT: the numbers of the FAT clusters
 *   clusters 9-40    FAT, one word per allocatable cluster: 0x7FFFFFFF
 *                    free, 0x80000000 | next, 0xFFFFFFFF end of chain
 *   clusters 41-8175 allocatable area. Cluster numbers in the FAT and in
 *                    directory entries are relative to it; the root
 *                    directory starts at relative cluster 0.
 *   blocks 1022/1023 backup blocks mcman uses while programming; 1022 is
 *                    kept erased, which means "nothing to restore".
 * A directory is a chain of 512-byte entries, one per page: "." and ".."
 * first, then one per file or subdirectory. A subdirectory's "." entry
 * points back to its own entry in the parent (cluster and index), and the
 * entry count is kept in that parent entry (the root keeps it in its own
 * "." entry). Deleting clears an entry's exists bit and frees its clusters;
 * the slot stays. Timestamps are JST, as on the PS2.
 *
 * mcfs_format writes the same layout as a real format, and entries are
 * created and deleted the same way; this was checked against cards made by
 * an independent tool. The whole image is held in
 * memory; changed pages get a fresh ECC and are written back by
 * mcfs_commit.
 *
 * Writing back (not the original's): the original game writes
 * the changed pages in place (fseek, fwrite, fflush, the file open for the
 * whole session), so a process stopped in a commit (a crash, a kill) can
 * leave the file with some of a commit's pages new and others old, and a
 * power cut after the fflush can lose any of them. Here a commit first
 * writes its pages to FILE.journal and flushes it to the disk, then writes
 * them in place and flushes the card, then removes the journal; a load that
 * finds a whole journal writes its pages again, and drops a partial one
 * (journal_recover). So the card a load finds is always the image after some
 * whole commit. The card's bytes are the same as the original's (the
 * changed pages with a fresh ECC, the others as loaded), and the game sees
 * the image in memory either way. A commit that fails keeps its pages
 * marked, so the next one writes them. A new card and the backup (FILE.bak,
 * the card as loaded, before a session's first commit) are written whole:
 * FILE.tmp, flushed, renamed over (host_mcfs_linux.c, host_mcfs_windows.c).
 * Two flushes of a commit's few pages cost far less than one of the whole
 * 8 MB image: the io tests print a commit's time. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io/io_mcfs.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;

static void mcfs_log(const char *fmt, ...)
{
    char line[1400] = "mc: ";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + 4, sizeof(line) - 4, fmt, ap);
    va_end(ap);
    Host_Log(line);
}
#define LOGI(tag, ...) mcfs_log(__VA_ARGS__)
#define LOGW(tag, ...) mcfs_log(__VA_ARGS__)
#define LOGE(tag, ...) mcfs_log(__VA_ARGS__)

#define PAGE 512
#define RAWPAGE 528
#define MAX_FD 16

/* Standard 8 MB card, as mcfs_format writes it */
#define STD_PAGES 16384u
#define STD_CLUSTERS 8192u
#define STD_ALLOC_OFFSET 41u
#define STD_ALLOC_END 8135u
#define STD_IFC 8u
#define STD_FAT_FIRST 9u
#define STD_FAT_CLUSTERS 32u
#define STD_BACKUP1 1023u
#define STD_BACKUP2 1022u

#define FAT_FREE 0x7FFFFFFFu
#define FAT_END 0xFFFFFFFFu
#define FAT_USED 0x80000000u

/* Directory entry fields */
#define E_MODE 0x00
#define E_LEN 0x04
#define E_CREATED 0x08
#define E_CLUSTER 0x10
#define E_DIRENT 0x14
#define E_MODIFIED 0x18
#define E_ATTR 0x20
#define E_NAME 0x40

static const char MAGIC[] = "Sony PS2 Memory Card Format 1.2.0.0";

typedef struct Handle {
    int used;
    u32 dir, index;   /* the file's entry: first cluster of its directory, index */
    u32 pos;
    int flags;
    int written;
} Handle;

/* A directory: its first cluster, and where its entry count lives. */
typedef struct Dir {
    u32 first;
    u32 count_dir, count_index;
} Dir;

struct McFs {
    char path[1024];
    u8 *img;
    size_t size;
    u32 npages;
    u8 *dirty;
    int formatted;
    int backed_up;
    int existed;
    u32 ppc, clusters, alloc_offset, alloc_end, ifc[32];
    u32 csize, fpc;
    char cwd[1024];
    Handle fh[MAX_FD];
    /* GetDir continuation */
    Dir gd_dir;
    u32 gd_next;
    char gd_pattern[64];
    int gd_valid;
};

static u32 rd32(const u8 *p, u32 o) { u32 v; memcpy(&v, p + o, 4); return v; }
static void wr32(u8 *p, u32 o, u32 v) { memcpy(p + o, &v, 4); }
static u16 rd16(const u8 *p, u32 o) { u16 v; memcpy(&v, p + o, 2); return v; }
static void wr16(u8 *p, u32 o, u16 v) { memcpy(p + o, &v, 2); }

/* ------------------------------------------------------------ ECC */

static u8 g_colpar[256];   /* column-parity bits of each byte value */
static int g_ecc_ready;

static int parity8(u32 b)
{
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return (int)(b & 1);
}

static void ecc_init(void)
{
    static const u8 masks[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
    for (u32 b = 0; b < 256; b++) {
        u8 m = 0;
        for (int i = 0; i < 7; i++)
            if (parity8(b & masks[i]))
                m |= (u8)(1u << i);
        g_colpar[b] = m;
    }
    g_ecc_ready = 1;
}

/* 3-byte ECC of a 128-byte chunk: column parity, line parities. */
static void ecc128(const u8 *d, u8 *out)
{
    u8 cp = 0x77, lp0 = 0x7F, lp1 = 0x7F;
    for (u32 i = 0; i < 128; i++) {
        cp ^= g_colpar[d[i]];
        if (parity8(d[i])) {
            lp0 ^= (u8)~i;
            lp1 ^= (u8)i;
        }
    }
    out[0] = cp;
    out[1] = lp0 & 0x7F;
    out[2] = lp1;
}

static void page_ecc(u8 *raw)
{
    for (u32 c = 0; c < 4; c++)
        ecc128(raw + c * 128, raw + PAGE + c * 3);
    memset(raw + PAGE + 12, 0, 4);
}

/* ------------------------------------------------------------ pages, clusters */

static u8 *page_ptr(McFs *fs, u32 page) { return fs->img + (size_t)page * RAWPAGE; }

static void page_touch(McFs *fs, u32 page)
{
    if (page < fs->npages)
        fs->dirty[page] = 1;
}

static int cluster_ok(const McFs *fs, u32 abs) { return abs < fs->clusters; }

static void cl_read(McFs *fs, u32 abs, u32 off, void *dst, u32 n)
{
    u8 *d = dst;
    while (n) {
        u32 page = abs * fs->ppc + off / PAGE, po = off % PAGE;
        u32 k = PAGE - po < n ? PAGE - po : n;
        memcpy(d, page_ptr(fs, page) + po, k);
        d += k;
        off += k;
        n -= k;
    }
}

static void cl_write(McFs *fs, u32 abs, u32 off, const void *src, u32 n)
{
    const u8 *s = src;
    while (n) {
        u32 page = abs * fs->ppc + off / PAGE, po = off % PAGE;
        u32 k = PAGE - po < n ? PAGE - po : n;
        memcpy(page_ptr(fs, page) + po, s, k);
        page_touch(fs, page);
        s += k;
        off += k;
        n -= k;
    }
}

static void cl_fill(McFs *fs, u32 abs, u8 v)
{
    for (u32 p = 0; p < fs->ppc; p++) {
        memset(page_ptr(fs, abs * fs->ppc + p), v, PAGE);
        page_touch(fs, abs * fs->ppc + p);
    }
}

static u32 cl_r32(McFs *fs, u32 abs, u32 index)
{
    u32 v = FAT_END;
    if (cluster_ok(fs, abs))
        cl_read(fs, abs, index * 4, &v, 4);
    return v;
}

static void cl_w32(McFs *fs, u32 abs, u32 index, u32 v)
{
    if (cluster_ok(fs, abs))
        cl_write(fs, abs, index * 4, &v, 4);
}

/* ------------------------------------------------------------ FAT */

static u32 fat_get(McFs *fs, u32 rel)
{
    u32 fpc = fs->fpc, i = rel / (fpc * fpc), j = (rel / fpc) % fpc;
    if (i >= 32 || rel >= fs->alloc_end)
        return FAT_END;
    return cl_r32(fs, cl_r32(fs, fs->ifc[i], j), rel % fpc);
}

static void fat_set(McFs *fs, u32 rel, u32 v)
{
    u32 fpc = fs->fpc, i = rel / (fpc * fpc), j = (rel / fpc) % fpc;
    if (i >= 32 || rel >= fs->alloc_end)
        return;
    cl_w32(fs, cl_r32(fs, fs->ifc[i], j), rel % fpc, v);
}

static int is_used(u32 v) { return (v & FAT_USED) != 0; }

static int alloc_cluster(McFs *fs, u32 *out)
{
    for (u32 r = 0; r < fs->alloc_end; r++) {
        if (!is_used(fat_get(fs, r))) {
            fat_set(fs, r, FAT_END);
            cl_fill(fs, fs->alloc_offset + r, 0);
            *out = r;
            return MC_OK;
        }
    }
    return MC_ERR_FULL;
}

static void free_chain(McFs *fs, u32 rel)
{
    for (u32 guard = 0; guard < fs->alloc_end && rel < fs->alloc_end; guard++) {
        u32 v = fat_get(fs, rel);
        fat_set(fs, rel, FAT_FREE);
        if (v == FAT_END || !is_used(v))
            break;
        rel = v & ~FAT_USED;
    }
}

/* The n-th cluster of the chain starting at `first`; with grow, clusters
 * are appended as needed. */
static int chain_nth(McFs *fs, u32 first, u32 n, int grow, u32 *out)
{
    u32 c = first;
    if (c >= fs->alloc_end)
        return MC_ERR_NOENT;
    for (u32 k = 0; k < n; k++) {
        u32 v = fat_get(fs, c);
        if (v == FAT_END || !is_used(v)) {
            if (!grow)
                return MC_ERR_NOENT;
            u32 nc;
            int r = alloc_cluster(fs, &nc);
            if (r)
                return r;
            fat_set(fs, c, FAT_USED | nc);
            c = nc;
        } else {
            c = v & ~FAT_USED;
            if (c >= fs->alloc_end)
                return MC_ERR_NOENT;
        }
    }
    *out = c;
    return MC_OK;
}

/* ------------------------------------------------------------ directory entries */

/* Entry k of the directory starting at `dir` (each entry is one page). */
static u8 *entry(McFs *fs, u32 dir, u32 k, int grow, u32 *page_out)
{
    u32 c;
    if (chain_nth(fs, dir, k / 2, grow, &c))
        return NULL;
    u32 page = (fs->alloc_offset + c) * fs->ppc + (k % 2);
    if (page_out)
        *page_out = page;
    return page_ptr(fs, page);
}

static void entry_touch(McFs *fs, u32 dir, u32 k)
{
    u32 page;
    if (entry(fs, dir, k, 0, &page))
        page_touch(fs, page);
}

static u8 g_clock[8];   /* JST, sceMcStDateTime (mcfs_set_clock) */

HOST void mcfs_set_clock(const unsigned char jst[8])
{
    memcpy(g_clock, jst, 8);
}

static void now_jst(u8 *t)
{
    memcpy(t, g_clock, 8);
}

static u32 dir_count(McFs *fs, const Dir *d)
{
    u8 *e = entry(fs, d->count_dir, d->count_index, 0, NULL);
    return e ? rd32(e, E_LEN) : 0;
}

static void dir_set_count(McFs *fs, const Dir *d, u32 n)
{
    u8 *e = entry(fs, d->count_dir, d->count_index, 0, NULL);
    if (e) {
        wr32(e, E_LEN, n);
        entry_touch(fs, d->count_dir, d->count_index);
    }
}

static Dir root_dir(void)
{
    Dir d = {0, 0, 0};
    return d;
}

static int name_eq(const u8 *e, const char *name)
{
    return strncmp((const char *)e + E_NAME, name, 32) == 0;
}

/* Index of an existing entry called `name`, or -1. */
static int dir_find(McFs *fs, const Dir *d, const char *name)
{
    u32 n = dir_count(fs, d);
    for (u32 k = 0; k < n; k++) {
        u8 *e = entry(fs, d->first, k, 0, NULL);
        if (!e)
            break;
        if ((rd16(e, E_MODE) & MCM_EXISTS) && name_eq(e, name))
            return (int)k;
    }
    return -1;
}

/* A slot for a new entry: a deleted one, or a new one at the end. */
static int dir_new_slot(McFs *fs, const Dir *d, u32 *index)
{
    u32 n = dir_count(fs, d);
    for (u32 k = 2; k < n; k++) {
        u8 *e = entry(fs, d->first, k, 0, NULL);
        if (e && !(rd16(e, E_MODE) & MCM_EXISTS)) {
            *index = k;
            return MC_OK;
        }
    }
    if (!entry(fs, d->first, n, 1, NULL))
        return MC_ERR_FULL;
    dir_set_count(fs, d, n + 1);
    *index = n;
    return MC_OK;
}

static void entry_init(u8 *e, u16 mode, u32 len, u32 cluster, u32 dirent, const char *name)
{
    memset(e, 0, PAGE);
    wr16(e, E_MODE, mode);
    wr32(e, E_LEN, len);
    now_jst(e + E_CREATED);
    wr32(e, E_CLUSTER, cluster);
    wr32(e, E_DIRENT, dirent);
    memcpy(e + E_MODIFIED, e + E_CREATED, 8);
    strncpy((char *)e + E_NAME, name, 31);
}

/* ------------------------------------------------------------ paths */

typedef struct Loc {
    Dir parent;       /* directory that holds (or would hold) the target */
    int is_root;      /* the path names the root directory itself */
    int index;        /* the target's entry in parent, or -1 */
    char name[32];
} Loc;

/* Split an absolute or cwd-relative path into components, resolving "."
 * and "..". Returns the number of components or -1. */
static int split_path(McFs *fs, const char *path, char comp[][32], int max)
{
    char full[2100];
    if (path[0] == '/')
        snprintf(full, sizeof(full), "%s", path);
    else
        snprintf(full, sizeof(full), "%s/%s", fs->cwd, path);
    int n = 0;
    for (char *s = full; *s;) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        char *e = s;
        while (*e && *e != '/')
            e++;
        size_t len = (size_t)(e - s);
        if (len == 1 && s[0] == '.') {
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (n > 0)
                n--;
        } else {
            if (len > 31 || n == max)
                return -1;
            memcpy(comp[n], s, len);
            comp[n][len] = 0;
            n++;
        }
        s = e;
    }
    return n;
}

static int resolve(McFs *fs, const char *path, Loc *loc)
{
    char comp[16][32];
    int n = split_path(fs, path, comp, 16);
    if (n < 0)
        return MC_ERR_NOENT;
    memset(loc, 0, sizeof(*loc));
    loc->parent = root_dir();
    loc->index = -1;
    if (n == 0) {
        loc->is_root = 1;
        return MC_OK;
    }
    for (int i = 0; i < n - 1; i++) {
        int k = dir_find(fs, &loc->parent, comp[i]);
        if (k < 0)
            return MC_ERR_NOENT;
        u8 *e = entry(fs, loc->parent.first, (u32)k, 0, NULL);
        if (!(rd16(e, E_MODE) & MCM_DIR))
            return MC_ERR_NOENT;
        Dir sub = {rd32(e, E_CLUSTER), loc->parent.first, (u32)k};
        loc->parent = sub;
    }
    snprintf(loc->name, sizeof(loc->name), "%s", comp[n - 1]);
    loc->index = dir_find(fs, &loc->parent, loc->name);
    return MC_OK;
}

/* The directory a Loc names (root, or an existing subdirectory entry). */
static int loc_dir(McFs *fs, const Loc *loc, Dir *out)
{
    if (loc->is_root) {
        *out = root_dir();
        return MC_OK;
    }
    if (loc->index < 0)
        return MC_ERR_NOENT;
    u8 *e = entry(fs, loc->parent.first, (u32)loc->index, 0, NULL);
    if (!e || !(rd16(e, E_MODE) & MCM_DIR))
        return MC_ERR_NOENT;
    Dir d = {rd32(e, E_CLUSTER), loc->parent.first, (u32)loc->index};
    *out = d;
    return MC_OK;
}

/* ------------------------------------------------------------ image */

static void load_superblock(McFs *fs)
{
    const u8 *sb = page_ptr(fs, 0);
    fs->formatted = 0;
    if (memcmp(sb, MAGIC, 28) != 0)
        return;
    u16 page_len = rd16(sb, 0x28), ppc = rd16(sb, 0x2A);
    u32 clusters = rd32(sb, 0x30);
    if (page_len != PAGE || ppc != 2 || (u64)clusters * ppc > fs->npages) {
        LOGW("mc", "%s: unsupported card geometry (page %u, %u pages/cluster, %u clusters)", fs->path, page_len, ppc,
             clusters);
        return;
    }
    fs->ppc = ppc;
    fs->clusters = clusters;
    fs->alloc_offset = rd32(sb, 0x34);
    fs->alloc_end = rd32(sb, 0x38);
    for (int i = 0; i < 32; i++)
        fs->ifc[i] = rd32(sb, 0x50 + 4 * (u32)i);
    fs->csize = PAGE * ppc;
    fs->fpc = fs->csize / 4;
    if (fs->alloc_offset + fs->alloc_end > clusters)
        return;
    fs->formatted = 1;
}

int mcfs_format(McFs *fs)
{
    if (fs->npages < STD_PAGES)
        return MC_ERR_CHANGED;
    for (int i = 0; i < MAX_FD; i++)
        fs->fh[i].used = 0;
    /* Everything zero-filled (with ECC), except backup block 1022: erased. */
    for (u32 p = 0; p < fs->npages; p++) {
        u8 *raw = page_ptr(fs, p);
        if (p / 16 == STD_BACKUP2)
            memset(raw, 0xFF, RAWPAGE);
        else
            memset(raw, 0, PAGE);
        fs->dirty[p] = 1;
    }
    u8 *sb = page_ptr(fs, 0);
    memcpy(sb, MAGIC, sizeof(MAGIC));
    wr16(sb, 0x28, PAGE);
    wr16(sb, 0x2A, 2);
    wr16(sb, 0x2C, 16);
    wr16(sb, 0x2E, 0xFF00);
    wr32(sb, 0x30, STD_CLUSTERS);
    wr32(sb, 0x34, STD_ALLOC_OFFSET);
    wr32(sb, 0x38, STD_ALLOC_END);
    wr32(sb, 0x3C, 0);   /* root directory cluster */
    wr32(sb, 0x40, STD_BACKUP1);
    wr32(sb, 0x44, STD_BACKUP2);
    wr32(sb, 0x50, STD_IFC);
    memset(sb + 0xD0, 0xFF, 32 * 4);   /* no bad blocks */
    sb[0x150] = 2;      /* PS2 card */
    sb[0x151] = 0x2B;   /* card flags, as the card reports them */
    load_superblock(fs);

    /* Indirect FAT, then the FAT: the root directory's cluster is in use. */
    for (u32 i = 0; i < fs->fpc; i++)
        cl_w32(fs, STD_IFC, i, i < STD_FAT_CLUSTERS ? STD_FAT_FIRST + i : FAT_END);
    for (u32 i = 0; i < STD_FAT_CLUSTERS * fs->fpc; i++) {
        u32 v = i == 0 ? FAT_END : i < STD_ALLOC_END ? FAT_FREE : FAT_END;
        cl_w32(fs, STD_FAT_FIRST + i / fs->fpc, i % fs->fpc, v);
    }
    u8 *dot = entry(fs, 0, 0, 0, NULL), *dotdot = entry(fs, 0, 1, 0, NULL);
    entry_init(dot, MCM_EXISTS | MCM_0400 | MCM_DIR | MCM_READ | MCM_WRITE | MCM_EXEC, 2, 0, 0, ".");
    entry_init(dotdot, MCM_EXISTS | MCM_HIDDEN | MCM_0400 | MCM_DIR | MCM_WRITE | MCM_EXEC, 0, 0, 0, "..");
    snprintf(fs->cwd, sizeof(fs->cwd), "/");
    fs->gd_valid = 0;
    return MC_OK;
}

int mcfs_unformat(McFs *fs)
{
    memset(fs->img, 0xFF, fs->size);
    memset(fs->dirty, 1, fs->npages);
    for (int i = 0; i < MAX_FD; i++)
        fs->fh[i].used = 0;
    fs->formatted = 0;
    fs->gd_valid = 0;
    return MC_OK;
}

static void journal_recover(McFs *fs);

McFs *mcfs_load(const char *path, int create)
{
    if (!g_ecc_ready)
        ecc_init();
    McFs *fs = calloc(1, sizeof(*fs));
    if (!fs)
        return NULL;
    snprintf(fs->path, sizeof(fs->path), "%s", path);
    snprintf(fs->cwd, sizeof(fs->cwd), "/");
    /* The file is read whole and closed: each commit replaces it (write_whole). */
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < (long)RAWPAGE * 1024 || sz % RAWPAGE) {
            LOGW("mc", "%s: %ld bytes is not a PS2 memory card image (528-byte pages)", path, sz);
            fclose(f);
            free(fs);
            return NULL;
        }
        fs->size = (size_t)sz;
        fs->img = malloc(fs->size);
        if (!fs->img || fread(fs->img, 1, fs->size, f) != fs->size) {
            LOGW("mc", "%s: read failed", path);
            fclose(f);
            free(fs->img);
            free(fs);
            return NULL;
        }
        fclose(f);
        fs->existed = 1;
    } else {
        if (!create) {
            free(fs);
            return NULL;
        }
        fs->size = (size_t)STD_PAGES * RAWPAGE;
        fs->img = calloc(1, fs->size);
        if (!fs->img) {
            free(fs);
            return NULL;
        }
    }
    fs->npages = (u32)(fs->size / RAWPAGE);
    fs->dirty = calloc(fs->npages, 1);
    if (!fs->dirty) {
        mcfs_free(fs);
        return NULL;
    }
    if (fs->existed) {
        load_superblock(fs);
        journal_recover(fs);
    } else {
        mcfs_format(fs);
        if (mcfs_commit(fs) != MC_OK) {
            LOGW("mc", "cannot create %s", path);
            mcfs_free(fs);
            return NULL;
        }
        LOGI("mc", "created a new formatted 8 MB memory card: %s", path);
    }
    return fs;
}

int mcfs_save_copy(const McFs *fs, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(fs->img, 1, fs->size, f) == fs->size;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

void mcfs_free(McFs *fs)
{
    if (!fs)
        return;
    free(fs->img);
    free(fs->dirty);
    free(fs);
}

const char *mcfs_path(const McFs *fs) { return fs->path; }
int mcfs_formatted(const McFs *fs) { return fs->formatted; }

/* The platform's part (host_mcfs_linux.c, host_mcfs_windows.c): flushes an
 * open file's data to the disk, and renames from over to (replacing it, the
 * rename itself made durable). Each returns 0 on success. */
int HostMcfs_SyncFile(FILE *f);
int HostMcfs_Replace(const char *from, const char *to);

/* The tests' stop (mcfs_test_stop_write): -1 none; else the bytes the next
 * writes may still make before they stop, as a process killed there would. */
static long g_stop_after = -1;

void mcfs_test_stop_write(long bytes) { g_stop_after = bytes; }

/* fwrite under the tests' stop: 0 when all n bytes were written; -2 when the
 * stop came (the bytes before it written), -1 on a failure. */
static int put(FILE *f, const void *data, size_t n)
{
    if (g_stop_after >= 0 && (size_t)g_stop_after < n) {
        fwrite(data, 1, (size_t)g_stop_after, f);
        g_stop_after = 0;
        return -2;
    }
    if (g_stop_after >= 0)
        g_stop_after -= (long)n;
    return fwrite(data, 1, n, f) == n ? 0 : -1;
}

/* Writes n bytes as the whole of path: to path.tmp, flushed to the disk, then
 * renamed over path (a new card, the backup). 0 on success, else -1 (the .tmp
 * removed) or -2 (the tests' stop: the partial .tmp left). */
static int write_whole(const char *path, const u8 *data, size_t n)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    int r = put(f, data, n);
    if (r == -2) {
        fclose(f);
        return -2;
    }
    int ok = r == 0;
    ok = fflush(f) == 0 && ok;
    ok = HostMcfs_SyncFile(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || HostMcfs_Replace(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/* The journal, PATH.journal: a commit's pages, written and flushed before the
 * card is written in place, removed after the card is flushed. Its form: the
 * magic, the card's page count and the journal's page count (u32 each), then
 * per page its number (u32) and its 528 bytes, then an FNV-1a (u32) of all
 * that. A whole journal at a load is a commit stopped while the card was
 * written: its pages are written again. A partial one is a commit stopped
 * before the card was touched: it is dropped. */
static const char JMAGIC[8] = "MH2MCJ1";

static void journal_path(const McFs *fs, char *out, size_t cap)
{
    snprintf(out, cap, "%s.journal", fs->path);
}

static u32 fnv(u32 h, const void *data, size_t n)
{
    const u8 *p = data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Writes the marked pages in place and flushes the card; 0, -1 or -2 (put). */
static int write_pages(McFs *fs)
{
    FILE *f = fopen(fs->path, "r+b");
    if (!f)
        return -1;
    int r = 0;
    for (u32 p = 0; p < fs->npages && r == 0; p++) {
        if (!fs->dirty[p])
            continue;
        u32 run = 1;
        while (p + run < fs->npages && fs->dirty[p + run])
            run++;
        if (fseek(f, (long)((size_t)p * RAWPAGE), SEEK_SET) != 0)
            r = -1;
        else
            r = put(f, page_ptr(fs, p), (size_t)run * RAWPAGE);
        p += run - 1;
    }
    if (r == -2) {
        fclose(f);
        return -2;
    }
    int ok = r == 0;
    ok = fflush(f) == 0 && ok;
    ok = HostMcfs_SyncFile(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : -1;
}

/* Writes the journal of the marked pages and flushes it; 0, -1 or -2 (put). */
static int write_journal(McFs *fs, const char *jpath)
{
    u32 count = 0, h = 2166136261u;
    for (u32 p = 0; p < fs->npages; p++)
        count += fs->dirty[p] != 0;
    FILE *f = fopen(jpath, "wb");
    if (!f)
        return -1;
    u32 sizes[2] = { fs->npages, count };
    int r = put(f, JMAGIC, 8);
    h = fnv(h, JMAGIC, 8);
    if (r == 0)
        r = put(f, sizes, sizeof(sizes));
    h = fnv(h, sizes, sizeof(sizes));
    for (u32 p = 0; p < fs->npages && r == 0; p++) {
        if (!fs->dirty[p])
            continue;
        r = put(f, &p, 4);
        if (r == 0)
            r = put(f, page_ptr(fs, p), RAWPAGE);
        h = fnv(fnv(h, &p, 4), page_ptr(fs, p), RAWPAGE);
    }
    if (r == 0)
        r = put(f, &h, 4);
    if (r == -2) {
        fclose(f);
        return -2;
    }
    int ok = r == 0;
    ok = fflush(f) == 0 && ok;
    ok = HostMcfs_SyncFile(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : -1;
}

/* At a load: a whole journal's pages go into the image and the card; any
 * journal is then removed. */
static void journal_recover(McFs *fs)
{
    char jpath[1100];
    journal_path(fs, jpath, sizeof(jpath));
    FILE *f = fopen(jpath, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *j = n > 0 ? malloc((size_t)n) : NULL;
    int ok = j && fread(j, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    u32 count = 0;
    if (ok && n >= 20 && memcmp(j, JMAGIC, 8) == 0 && rd32(j, 8) == fs->npages) {
        count = rd32(j, 12);
        ok = count <= fs->npages && (u64)n == 20 + (u64)count * (4 + RAWPAGE) &&
             fnv(2166136261u, j, (size_t)n - 4) == rd32(j, (u32)n - 4);
    } else {
        ok = 0;
    }
    for (u32 i = 0; ok && i < count; i++) {
        u32 p = rd32(j, 16 + i * (4 + RAWPAGE));
        ok = p < fs->npages;
        if (ok) {
            memcpy(page_ptr(fs, p), j + 16 + i * (4 + RAWPAGE) + 4, RAWPAGE);
            fs->dirty[p] = 1;
        }
    }
    free(j);
    if (ok) {
        if (write_pages(fs) == 0) {
            memset(fs->dirty, 0, fs->npages);
            remove(jpath);
            LOGI("mc", "%s: completed the commit a stop interrupted (%u pages from %s)", fs->path, count, jpath);
        } else {
            LOGW("mc", "%s: cannot complete the interrupted commit; %s is kept", fs->path, jpath);
        }
        load_superblock(fs);
        return;
    }
    memset(fs->dirty, 0, fs->npages);
    remove(jpath);
    LOGI("mc", "%s: dropped %s, a commit stopped before the card was written", fs->path, jpath);
}

/* Before the first commit of a session: the card as loaded to FILE.bak (the
 * file still holds it: nothing was committed yet). */
static void backup_once(McFs *fs)
{
    if (fs->backed_up || !fs->existed)
        return;
    fs->backed_up = 1;   /* tried once, as the original game does */
    char bak[1100];
    snprintf(bak, sizeof(bak), "%s.bak", fs->path);
    FILE *f = fopen(fs->path, "rb");
    u8 *old = f ? malloc(fs->size) : NULL;
    int ok = old && fread(old, 1, fs->size, f) == fs->size;
    if (f)
        fclose(f);
    if (ok)
        ok = write_whole(bak, old, fs->size) == 0;
    free(old);
    if (!ok) {
        LOGW("mc", "cannot write the backup %s", bak);
        return;
    }
    LOGI("mc", "backed up %s to %s before the first change", fs->path, bak);
}

int mcfs_commit(McFs *fs)
{
    int any = 0;
    for (u32 p = 0; p < fs->npages && !any; p++)
        any = fs->dirty[p];
    if (!any)
        return MC_OK;
    backup_once(fs);
    for (u32 p = 0; p < fs->npages; p++) {
        if (!fs->dirty[p])
            continue;
        u8 *raw = page_ptr(fs, p);
        /* An erased page keeps an erased spare area. */
        int erased = 1;
        for (u32 i = 0; i < PAGE && erased; i++)
            erased = raw[i] == 0xFF;
        if (!(erased && raw[PAGE] == 0xFF))
            page_ecc(raw);
    }
    int r;
    if (!fs->existed) {
        /* A new card: the whole image, made at once. */
        r = write_whole(fs->path, fs->img, fs->size);
        if (r == 0)
            fs->existed = 1;
        fs->backed_up = 1;
    } else {
        char jpath[1100];
        journal_path(fs, jpath, sizeof(jpath));
        r = write_journal(fs, jpath);
        if (r == -1)
            remove(jpath);
        if (r == 0)
            r = write_pages(fs);
        if (r == 0)
            remove(jpath);   /* its removal need not reach the disk: a whole journal's pages are the card's */
    }
    g_stop_after = -1;   /* a test's stop is for one commit */
    if (r != 0) {
        /* The pages stay marked: the next commit writes them. */
        LOGE("mc", "writing %s failed", fs->path);
        return MC_ERR_CHANGED;
    }
    memset(fs->dirty, 0, fs->npages);
    return MC_OK;
}

u32 mcfs_free_clusters(McFs *fs)
{
    if (!fs->formatted)
        return 0;
    u32 n = 0;
    for (u32 r = 0; r < fs->alloc_end; r++)
        if (!is_used(fat_get(fs, r)))
            n++;
    return n;
}

/* ------------------------------------------------------------ files */

static Handle *handle(McFs *fs, int fd)
{
    if (fd < 0 || fd >= MAX_FD || !fs->fh[fd].used)
        return NULL;
    return &fs->fh[fd];
}

static int busy(McFs *fs, u32 dir, u32 index)
{
    for (int i = 0; i < MAX_FD; i++)
        if (fs->fh[i].used && fs->fh[i].dir == dir && fs->fh[i].index == index)
            return 1;
    return 0;
}

static int make_dir(McFs *fs, Loc *loc)
{
    u32 k, c;
    int r = dir_new_slot(fs, &loc->parent, &k);
    if (r)
        return r;
    r = alloc_cluster(fs, &c);
    if (r)
        return r;
    u8 *e = entry(fs, loc->parent.first, k, 0, NULL);
    entry_init(e, MCM_EXISTS | MCM_0400 | MCM_DIR | MCM_READ | MCM_WRITE | MCM_EXEC, 2, c, 0, loc->name);
    entry_touch(fs, loc->parent.first, k);
    u8 *dot = entry(fs, c, 0, 0, NULL), *dotdot = entry(fs, c, 1, 0, NULL);
    entry_init(dot, MCM_EXISTS | MCM_0400 | MCM_DIR | MCM_READ | MCM_WRITE | MCM_EXEC, 0, loc->parent.first, k, ".");
    entry_init(dotdot, MCM_EXISTS | MCM_0400 | MCM_DIR | MCM_READ | MCM_WRITE | MCM_EXEC, 0, 0, 0, "..");
    entry_touch(fs, c, 0);
    entry_touch(fs, c, 1);
    return MC_OK;
}

int mcfs_open(McFs *fs, const char *path, int flags)
{
    if (!fs->formatted)
        return MC_ERR_NOFORMAT;
    Loc loc;
    int r = resolve(fs, path, &loc);
    if (r)
        return r;
    if (flags & MCO_MKDIR) {
        if (loc.is_root || loc.index >= 0)
            return MC_ERR_NOENT;   /* already exists */
        return make_dir(fs, &loc);
    }
    if (loc.is_root)
        return MC_ERR_PERM;
    if (loc.index < 0) {
        if (!(flags & MCO_CREAT))
            return MC_ERR_NOENT;
        u32 k;
        r = dir_new_slot(fs, &loc.parent, &k);
        if (r)
            return r;
        u8 *e = entry(fs, loc.parent.first, k, 0, NULL);
        entry_init(e, MCM_EXISTS | MCM_0400 | MCM_FILE | MCM_READ | MCM_WRITE | MCM_EXEC, 0, FAT_END, 0, loc.name);
        entry_touch(fs, loc.parent.first, k);
        loc.index = (int)k;
    } else {
        u8 *e = entry(fs, loc.parent.first, (u32)loc.index, 0, NULL);
        if (!(rd16(e, E_MODE) & MCM_FILE))
            return MC_ERR_PERM;
        if ((flags & MCO_WRONLY) && !(rd16(e, E_MODE) & MCM_WRITE))
            return MC_ERR_PERM;
    }
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (!fs->fh[fd].used) {
            Handle *h = &fs->fh[fd];
            memset(h, 0, sizeof(*h));
            h->used = 1;
            h->dir = loc.parent.first;
            h->index = (u32)loc.index;
            h->flags = flags;
            h->written = (flags & MCO_CREAT) != 0;
            return fd;
        }
    }
    return MC_ERR_MAXHANDLE;
}

int mcfs_close(McFs *fs, int fd)
{
    Handle *h = handle(fs, fd);
    if (!h)
        return MC_ERR_NOENT;
    if (h->written) {
        u8 *e = entry(fs, h->dir, h->index, 0, NULL);
        if (e) {
            wr16(e, E_MODE, (u16)(rd16(e, E_MODE) | MCM_CLOSED));
            now_jst(e + E_MODIFIED);
            entry_touch(fs, h->dir, h->index);
        }
    }
    h->used = 0;
    return MC_OK;
}

int mcfs_read(McFs *fs, int fd, void *dst, u32 size)
{
    Handle *h = handle(fs, fd);
    if (!h)
        return MC_ERR_NOENT;
    if (!(h->flags & MCO_RDONLY))
        return MC_ERR_PERM;
    u8 *e = entry(fs, h->dir, h->index, 0, NULL);
    u32 len = rd32(e, E_LEN), first = rd32(e, E_CLUSTER);
    if (h->pos >= len)
        return 0;
    u32 n = len - h->pos < size ? len - h->pos : size, done = 0;
    u8 *d = dst;
    while (done < n) {
        u32 ci = (h->pos + done) / fs->csize, off = (h->pos + done) % fs->csize, c;
        u32 k = fs->csize - off < n - done ? fs->csize - off : n - done;
        if (chain_nth(fs, first, ci, 0, &c))
            break;
        cl_read(fs, fs->alloc_offset + c, off, d + done, k);
        done += k;
    }
    h->pos += done;
    return (int)done;
}

int mcfs_write(McFs *fs, int fd, const void *src, u32 size)
{
    Handle *h = handle(fs, fd);
    if (!h)
        return MC_ERR_NOENT;
    if (!(h->flags & MCO_WRONLY))
        return MC_ERR_PERM;
    u8 *e = entry(fs, h->dir, h->index, 0, NULL);
    u32 first = rd32(e, E_CLUSTER), done = 0;
    const u8 *s = src;
    if (size && first == FAT_END) {
        int r = alloc_cluster(fs, &first);
        if (r)
            return r;
        e = entry(fs, h->dir, h->index, 0, NULL);
        wr32(e, E_CLUSTER, first);
        entry_touch(fs, h->dir, h->index);
    }
    while (done < size) {
        u32 ci = (h->pos + done) / fs->csize, off = (h->pos + done) % fs->csize, c;
        u32 k = fs->csize - off < size - done ? fs->csize - off : size - done;
        if (chain_nth(fs, first, ci, 1, &c))
            break;
        cl_write(fs, fs->alloc_offset + c, off, s + done, k);
        done += k;
    }
    h->pos += done;
    h->written = 1;
    e = entry(fs, h->dir, h->index, 0, NULL);
    if (h->pos > rd32(e, E_LEN)) {
        wr32(e, E_LEN, h->pos);
        entry_touch(fs, h->dir, h->index);
    }
    if (size && !done)
        return MC_ERR_FULL;
    return (int)done;
}

int mcfs_seek(McFs *fs, int fd, s32 offset, int origin)
{
    Handle *h = handle(fs, fd);
    if (!h)
        return MC_ERR_NOENT;
    u8 *e = entry(fs, h->dir, h->index, 0, NULL);
    s64 base = origin == 1 ? (s64)h->pos : origin == 2 ? (s64)rd32(e, E_LEN) : 0;
    s64 pos = base + offset;
    if (pos < 0)
        pos = 0;
    h->pos = (u32)pos;
    return (int)h->pos;
}

int mcfs_flush(McFs *fs, int fd)
{
    return handle(fs, fd) ? mcfs_commit(fs) : MC_ERR_NOENT;
}

int mcfs_chdir(McFs *fs, const char *path, char *cwd, unsigned int cap)
{
    char comp[16][32];
    int n = split_path(fs, path, comp, 16);
    Loc loc;
    Dir d;
    if (n < 0 || resolve(fs, path, &loc) || loc_dir(fs, &loc, &d))
        return MC_ERR_NOENT;
    size_t len = 0;
    fs->cwd[0] = 0;
    for (int i = 0; i < n; i++)
        len += (size_t)snprintf(fs->cwd + len, sizeof(fs->cwd) - len, "/%s", comp[i]);
    if (!n)
        snprintf(fs->cwd, sizeof(fs->cwd), "/");
    if (cwd)
        snprintf(cwd, cap, "%s", fs->cwd);
    return MC_OK;
}

static int wild(const char *p, const char *s)
{
    for (;;) {
        if (*p == '*') {
            p++;
            for (;;) {
                if (wild(p, s))
                    return 1;
                if (!*s)
                    return 0;
                s++;
            }
        }
        if (!*p)
            return !*s;
        if (!*s || (*p != '?' && *p != *s))
            return 0;
        p++;
        s++;
    }
}

static void fill_info(const u8 *e, McDirInfo *o)
{
    memset(o, 0, sizeof(*o));
    memcpy(o->created, e + E_CREATED, 8);
    memcpy(o->modified, e + E_MODIFIED, 8);
    o->size = rd32(e, E_LEN);
    o->mode = rd16(e, E_MODE);
    o->attr = rd32(e, E_ATTR);
    memcpy(o->name, e + E_NAME, 32);
    o->name[31] = 0;
}

int mcfs_getdir(McFs *fs, const char *pattern, int cont, int maxent, McDirInfo *out)
{
    if (!fs->formatted)
        return MC_ERR_NOFORMAT;
    if (!cont) {
        fs->gd_valid = 0;
        Loc loc;
        int r = resolve(fs, pattern, &loc);
        if (r)
            return r;
        if (loc.is_root) {
            /* "/" itself: list the root's entries */
            fs->gd_dir = root_dir();
            snprintf(fs->gd_pattern, sizeof(fs->gd_pattern), "*");
        } else {
            fs->gd_dir = loc.parent;
            snprintf(fs->gd_pattern, sizeof(fs->gd_pattern), "%s", loc.name);
        }
        fs->gd_next = 0;
        fs->gd_valid = 1;
    }
    if (!fs->gd_valid)
        return 0;
    int n = 0;
    u32 count = dir_count(fs, &fs->gd_dir);
    while (fs->gd_next < count && n < maxent) {
        u8 *e = entry(fs, fs->gd_dir.first, fs->gd_next, 0, NULL);
        fs->gd_next++;
        if (!e)
            break;
        if (!(rd16(e, E_MODE) & MCM_EXISTS))
            continue;
        char name[33];
        memcpy(name, e + E_NAME, 32);
        name[32] = 0;
        if (!wild(fs->gd_pattern, name))
            continue;
        if (out)
            fill_info(e, &out[n]);
        n++;
    }
    return n;
}

static int dir_empty(McFs *fs, const Dir *d)
{
    u32 n = dir_count(fs, d);
    for (u32 k = 2; k < n; k++) {
        u8 *e = entry(fs, d->first, k, 0, NULL);
        if (e && (rd16(e, E_MODE) & MCM_EXISTS))
            return 0;
    }
    return 1;
}

int mcfs_delete(McFs *fs, const char *path)
{
    if (!fs->formatted)
        return MC_ERR_NOFORMAT;
    Loc loc;
    int r = resolve(fs, path, &loc);
    if (r)
        return r;
    if (loc.is_root || loc.index < 0)
        return MC_ERR_NOENT;
    u8 *e = entry(fs, loc.parent.first, (u32)loc.index, 0, NULL);
    u16 mode = rd16(e, E_MODE);
    if (busy(fs, loc.parent.first, (u32)loc.index))
        return MC_ERR_PERM;
    if (mode & MCM_DIR) {
        Dir d;
        loc_dir(fs, &loc, &d);
        if (!dir_empty(fs, &d))
            return MC_ERR_NOTEMPTY;
    }
    u32 first = rd32(e, E_CLUSTER);
    wr16(e, E_MODE, (u16)(mode & ~MCM_EXISTS));
    entry_touch(fs, loc.parent.first, (u32)loc.index);
    if (first != FAT_END)
        free_chain(fs, first);
    fs->gd_valid = 0;
    return MC_OK;
}

int mcfs_setinfo(McFs *fs, const char *path, const McDirInfo *info, u32 flags)
{
    if (!fs->formatted)
        return MC_ERR_NOFORMAT;
    Loc loc;
    int r = resolve(fs, path, &loc);
    if (r)
        return r;
    if (loc.is_root || loc.index < 0)
        return MC_ERR_NOENT;
    if ((flags & 0x10) && info->name[0]) {   /* rename: the new name must be free */
        char nm[32];
        snprintf(nm, sizeof(nm), "%.31s", info->name);
        int other = dir_find(fs, &loc.parent, nm);
        if (other >= 0 && other != loc.index)
            return MC_ERR_NOENT;
    }
    u8 *e = entry(fs, loc.parent.first, (u32)loc.index, 0, NULL);
    if (flags & 0x01)
        memcpy(e + E_CREATED, info->created, 8);
    if (flags & 0x02)
        memcpy(e + E_MODIFIED, info->modified, 8);
    if (flags & 0x04) {   /* user-settable mode bits */
        const u16 user = MCM_READ | MCM_WRITE | MCM_EXEC | MCM_DUPPROHIBIT | MCM_PSX | MCM_HIDDEN | 0x0800;
        wr16(e, E_MODE, (u16)((rd16(e, E_MODE) & ~user) | (info->mode & user)));
    }
    if ((flags & 0x10) && info->name[0]) {
        memset(e + E_NAME, 0, 32);
        strncpy((char *)e + E_NAME, info->name, 31);
    }
    entry_touch(fs, loc.parent.first, (u32)loc.index);
    return MC_OK;
}

int mcfs_getentspace(McFs *fs, const char *path)
{
    if (!fs->formatted)
        return MC_ERR_NOFORMAT;
    Loc loc;
    Dir d;
    if (resolve(fs, path, &loc) || loc_dir(fs, &loc, &d))
        return MC_ERR_NOENT;
    u32 n = dir_count(fs, &d), freeslots = n & 1;   /* the other half of the last cluster */
    for (u32 k = 2; k < n; k++) {
        u8 *e = entry(fs, d.first, k, 0, NULL);
        if (e && !(rd16(e, E_MODE) & MCM_EXISTS))
            freeslots++;
    }
    return (int)freeslots;
}
