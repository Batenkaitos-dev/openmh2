/*
 * CRI ADX's file system side, ADXF, as Capcom's code calls it:
 * the handles, the reads and their server, the partition loader. Written from
 * the original's behaviour at the interface (what the library does when it
 * opens and closes a handle, reads, runs its server and loads a partition).
 * The handles and the loader's words are the library's own memory
 * (not the game's); a partition's record is Capcom's buffer, written as the
 * original game writes it. The reads go through the stream layer (adx.c), and the
 * handles' states change in ADX's server on the FS thread (Adxf_Server).
 */
#include "common.h"
#include "pal.h"
#include "data_c.h"
#include "adx.h"
#include "io/adx.h"
#include "io/io.h"

typedef struct AdxfHandle {
    u8 used;
    s8 state;
    AdxStm *stm;
    s32 fsct;       /* +0xC: the file's sectors */
    s32 fsize;      /* +0x10: its bytes */
    s32 pos;        /* +0x14: the read position, in sectors */
    s32 count;      /* +0x1C: the read in flight's sectors */
    s32 progress;   /* +0x20: what the server last saw read */
    u8 rest[0x44 - 0x1C];
} AdxfHandle;
_Static_assert(sizeof(AdxfHandle) == 0x44, "ADXF's records are 0x44 bytes");

#define ADXF_HANDLES 16
#define ADXF_PARTITIONS 0x100

/* The handles are the library's own records at their PS2 addresses (the
 * original's): 16 of 0x44 bytes at 0x19B410 in
 * ADX's work area, as in the original, so a handle is
 * the original's address. Their contents are the core's (the library's
 * private memory: no RAM image compares it). Cleared at the first use. The
 * records lie in main's .data (the libraries' part), laid out by the data
 * layer's DataC_main_Data: DataC_At gives them. */
#define ADXF_TABLE 0x0019B410u
static AdxfHandle *Handles(void)
{
    static int cleared;
    AdxfHandle *t = (AdxfHandle *)DataC_At(ADXF_TABLE);
    if (!cleared) {
        __builtin_memset(t, 0, ADXF_HANDLES * sizeof(*t));
        cleared = 1;
    }
    return t;
}
static u8 *g_ptab[ADXF_PARTITIONS];   /* the partition table: each id's record */

/* Where each partition's archive came from, for a pack's key: the disc file's
 * name ("DATA.BIN"), or its parent's chain and the entry's number in five digits ("DATA.BIN/00000"). Empty: none. The
 * record is the game's; this is the library's own. */
#define ADXF_CHAIN 64
static char g_psrc[ADXF_PARTITIONS][ADXF_CHAIN];
/* The partition's archive when a pack has it (the pack's name for it): its entries are read from the pack's file, while
 * the game's record stays the disc's. Empty: the disc's. */
static char g_ppack[ADXF_PARTITIONS][ADXF_CHAIN];

/* DST (ADXF_CHAIN bytes): SRC, then "/" and the entry's number in five digits when FLID is not negative. */
static void ChainJoin(char *dst, const char *src, s32 flid)
{
    u32 n = 0;
    while (src[n] && n < ADXF_CHAIN - 8) {
        dst[n] = src[n];
        n++;
    }
    if (flid >= 0 && n && n < ADXF_CHAIN - 7) {
        dst[n++] = '/';
        for (u32 d = 10000; d; d /= 10)
            dst[n++] = (char)('0' + (u32)flid / d % 10);
    }
    dst[n] = 0;
}

/* A file name as a pack names it: the device and the leading separators dropped, upper case, cut at the version. */
static void ChainName(char *dst, const char *name)
{
    char tmp[ADXF_CHAIN];
    u32 n = 0;
    for (const char *c = name; *c; c++)
        if (*c == ':')
            name = c + 1;
    while (*name == '\\' || *name == '/')
        name++;
    for (; *name && *name != ';' && n < ADXF_CHAIN - 8; name++)
        tmp[n++] = *name == '\\' ? '/' : (*name >= 'a' && *name <= 'z') ? (char)(*name - 32) : *name;
    tmp[n] = 0;
    ChainJoin(dst, tmp, -1);
}

/* The pack's name for the file FLID of partition PTID ("" when its source is unknown). */
static void PackKey(char *key, s32 ptid, s32 flid)
{
    key[0] = 0;
    if ((u32)ptid < ADXF_PARTITIONS && g_psrc[ptid][0])
        ChainJoin(key, g_psrc[ptid], flid);
}

/* The partition loader's words (one load at a time). */
static struct {
    s32 done;       /* the entries parsed */
    AdxfHandle *h;  /* the archive's handle */
    s32 ptid;       /* the partition being loaded (-1: none) */
    s32 state;      /* 1 set up, then the handle's state; 4 failed */
    u8 *scr;        /* the scratch sector the table of contents is read into */
    s32 scrsct;     /* its size in sectors */
} g_ld = {0, 0, -1, 0, 0, 0};
static u8 g_scratch[0x800] __attribute__((aligned(64)));

/* ADXERR: the error function ADXT_EntryErrFunc registered (snd's ADXT calls
 * Adx_ErrEntry). The messages are the core's own words. */
static void (*g_errFn)(void *obj, const char *msg);
static void *g_errObj;

void Adx_ErrEntry(void (*fn)(void *obj, const char *msg), void *obj)
{
    g_errFn = fn;
    g_errObj = obj;
}

static void Err(const char *msg)
{
    if (g_errFn)
        g_errFn(g_errObj, msg);
}

static u32 Rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void Wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* ------------------------------------------------------------ partitions */

/* func_0015FE98's check: 0, or -3 (the error function). */
static s32 CheckFile(s32 ptid, s32 flid)
{
    if ((u32)ptid >= ADXF_PARTITIONS || !g_ptab[ptid]) {
        Err("E0001: ADXF: partition id");
        return -3;
    }
    if (flid < 0 || flid >= (s32)Rd32(g_ptab[ptid] + 8)) {
        Err("E0002: ADXF: file id");
        return -3;
    }
    return 0;
}

/* func_0015FF38: the AFS file's name, its attribute word, the file's first
 * sector in it and its sectors, from the partition's table. */
static s32 AfsFile(s32 ptid, s32 flid, char *name, u32 *w, s32 *ofs, s32 *sct)
{
    s32 r = CheckFile(ptid, flid);
    if (r < 0) {
        *w = 0;
        *ofs = -1;
        *sct = -1;
        return r;
    }
    const u8 *rec = g_ptab[ptid];
    s32 sum = 0;
    for (s32 i = 0; i <= flid; i++)
        sum += rec[0xF] == 1 ? (s32)Rd32(rec + 0x118 + 4 * i) : (s32)(rec[0x118 + 2 * i] | rec[0x119 + 2 * i] << 8);
    *sct = rec[0xF] == 1 ? (s32)Rd32(rec + 0x118 + 4 * (flid + 1))
                         : (s32)(rec[0x118 + 2 * (flid + 1)] | rec[0x119 + 2 * (flid + 1)] << 8);
    if (name) {
        u32 i = 0;
        for (; i < 0x100 && rec[0x10 + i]; i++)
            name[i] = (char)rec[0x10 + i];
        for (; i < 0x100; i++)
            name[i] = 0;
    }
    *w = Rd32(rec + 0x110);
    *ofs = (s32)Rd32(rec + 0x114) + sum;
    return 0;
}

/* For ADXT (snd): the AFS file of (ptid, flid). */
s32 Adxf_AfsFile(s32 ptid, s32 flid, char name[0x100], s32 *ofs, s32 *sct)
{
    u32 w;
    return AfsFile(ptid, flid, name, &w, ofs, sct);
}

/* ------------------------------------------------------------ handles */

static AdxfHandle *NewHandle(void)
{
    for (int i = 0; i < ADXF_HANDLES; i++) {
        if (!Handles()[i].used) {
            AdxfHandle *h = &Handles()[i];
            __builtin_memset(h, 0, sizeof(*h));
            h->used = 1;
            h->state = ADXF_STAT_STOP;
            return h;
        }
    }
    Err("E0003: ADXF: no handle");
    return 0;
}

static ADXF Opened(AdxfHandle *h, AdxStm *s)
{
    if (!s) {
        Err("E0004: ADXF: the file can't be opened");
        __builtin_memset(h, 0, sizeof(*h));
        return 0;
    }
    h->stm = s;
    h->fsize = (s32)AdxStm_SizeBytes(s);
    h->fsct = (s32)AdxStm_SizeSectors(s);
    return h;
}

ADXF ADXF_Open(char *fname, void *atr);
static ADXF ADXF_OpenWork(char *fname, void *atr)
{
    (void)atr;
    AdxfHandle *h = NewHandle();
    if (!h)
        return 0;
    if (!fname) {
        Err("E0005: ADXF: no file name");
        __builtin_memset(h, 0, sizeof(*h));
        return 0;
    }
    u32 plba, psize;
    if (Io_DiscPackFind(fname, &plba, &psize) == 0)   /* a file the pack replaces whole */
        return Opened(h, AdxStm_OpenKnown(fname, plba, psize, 0, -1));
    return Opened(h, AdxStm_Open(fname, 0, -1));
}

ADXF ADXF_OpenAfs(s32 ptid, s32 flid);
static ADXF ADXF_OpenAfsWork(s32 ptid, s32 flid)
{
    char name[0x100];
    u32 w;
    s32 ofs, sct;
    AdxfHandle *h = NewHandle();
    if (!h)
        return 0;
    if (AfsFile(ptid, flid, name, &w, &ofs, &sct) < 0) {
        __builtin_memset(h, 0, sizeof(*h));
        return 0;
    }
    char key[ADXF_CHAIN];
    u32 plba, psize;
    PackKey(key, ptid, flid);
    if (key[0] && Io_DiscPackFind(key, &plba, &psize) == 0)   /* the pack's file, in place of the disc's */
        return Opened(h, AdxStm_OpenKnown(key, plba, psize, 0, -1));
    if ((u32)ptid < ADXF_PARTITIONS && g_ppack[ptid][0] && Io_DiscPackFind(g_ppack[ptid], &plba, &psize) == 0)
        return Opened(h, AdxStm_OpenKnown(g_ppack[ptid], plba, psize, ofs - (s32)Rd32(g_ptab[ptid] + 0x114), sct));   /* the archive is the pack's */
    if (Io_DiscPackFind(name, &plba, &psize) == 0)   /* a file the pack replaces whole */
        return Opened(h, AdxStm_OpenKnown(name, plba, psize, ofs, sct));
    return Opened(h, AdxStm_Open(name, ofs, sct));
}

static void EndRead(AdxfHandle *h)
{
    h->count = 0;
}

s32 ADXF_Stop(ADXF adxf);
static s32 ADXF_StopWork(ADXF adxf)
{
    AdxfHandle *h = adxf;
    if (!h) {
        Err("E0006: ADXF_Stop: NULL handle");
        return -3;
    }
    if (h->state == ADXF_STAT_STOP)
        return h->pos;
    if (h->state == ADXF_STAT_READEND) {
        h->state = ADXF_STAT_STOP;
        return h->pos;
    }
    if (!h->stm) {
        Err("E0007: ADXF_Stop: no stream");
        return -1;
    }
    AdxStm_Stop(h->stm);
    Adx_Lock();
    h->progress = (s32)AdxStm_Done(h->stm);
    EndRead(h);
    h->state = ADXF_STAT_STOP;
    Adx_Unlock();
    return h->pos;
}

void ADXF_Close(ADXF adxf);
static void ADXF_CloseWork(ADXF adxf)
{
    AdxfHandle *h = adxf;
    if (!h)
        return;
    if (h->state == ADXF_STAT_READING)
        ADXF_Stop(h);
    AdxStm *s = h->stm;
    h->used = 0;
    h->stm = 0;
    if (s)
        AdxStm_Close(s);
    __builtin_memset(h, 0, sizeof(*h));
}

s32 ADXF_ReadNw(ADXF adxf, s32 nsct, void *buf);
static s32 ADXF_ReadNwWork(ADXF adxf, s32 nsct, void *buf)
{
    AdxfHandle *h = adxf;
    if ((u32)buf & 0x3Fu) {
        Err("E0008: ADXF_ReadNw: the buffer is not on 64 bytes");
        return -3;
    }
    if (!h || nsct < 0 || !buf) {
        Err("E0009: ADXF_ReadNw: a parameter");
        return -3;
    }
    if (h->state == ADXF_STAT_READING)
        return 0;
    /* The original's three lock pairs (ADXF_ReadNw's tail jump into
     * func_0015F738; --lib-tree on the village, VBlank
     * 2736): the SJ made on the buffer (0x1764F8), the handle's SJ, buffer
     * and size, then func_0015F590's start of the stream's read. The
     * stream itself starts at the next server pass. */
    Adx_Lock();
    Adx_Unlock();
    Adx_Lock();
    Adx_Unlock();
    Adx_Lock();
    s32 left = h->fsct - h->pos;
    s32 count = nsct < left ? nsct : left;
    h->count = count;
    if (count == 0) {
        h->state = ADXF_STAT_READEND;
        Adx_Unlock();
        Adx_Lock();     /* the SJ destroyed (func_0015F738, a result of 0) */
        Adx_Unlock();
        return 0;
    }
    h->state = ADXF_STAT_READING;
    AdxStm_Read(h->stm, (u32)h->pos, (u32)count, buf);
    Adx_Unlock();
    return count;
}

s32 ADXF_GetStat(ADXF adxf);
static s32 ADXF_GetStatWork(ADXF adxf)
{
    AdxfHandle *h = adxf;
    if (!h) {
        Err("E0010: ADXF_GetStat: NULL handle");
        return -3;
    }
    return h->state;
}

s32 ADXF_GetFsizeSct(ADXF adxf);
static s32 ADXF_GetFsizeSctWork(ADXF adxf)
{
    AdxfHandle *h = adxf;
    if (!h) {
        Err("E0011: ADXF: NULL handle");
        return 0;
    }
    h->fsize = (s32)AdxStm_SizeBytes(h->stm);
    h->fsct = (s32)(((u32)h->fsize + 0x7FFu) >> 11);
    return h->fsct;
}

/* The server's step for one reading handle (func_0015FAC0): the stream's
 * state becomes the handle's; ended or failed, the position moves on. */
void Adxf_Server(void)
{
    Adx_Lock();
    for (int i = 0; i < ADXF_HANDLES; i++) {
        AdxfHandle *h = &Handles()[i];
        if (!h->used || h->state != ADXF_STAT_READING)
            continue;
        s32 st = AdxStm_State(h->stm);
        if (st == ADX_STM_READ)
            continue;
        h->state = (s8)(st == ADX_STM_END ? ADXF_STAT_READEND : st == ADX_STM_ERR ? ADXF_STAT_ERROR : st);
        h->progress = (s32)AdxStm_Done(h->stm);
        if (h->state == ADXF_STAT_READEND || h->state == ADXF_STAT_ERROR) {
            h->pos += h->progress;
            EndRead(h);
        }
    }
    Adx_Unlock();
}

/* ------------------------------------------------------------ the partition loader */

static void LoaderEnd(void)
{
    ADXF_Close(g_ld.h);
    g_ld.done = 0;
    g_ld.h = 0;
    g_ld.scrsct = 0;
}

/* func_0015E778: a load's start. */
static s32 LoadStart(s32 ptid, s32 afsPtid, s32 flid, char *name, void *atr, u8 *rec, u8 *scr, s32 size, s32 kind)
{
    if (g_ld.state == ADXF_STAT_READING) {
        Err("E0012: ADXF: a partition is being loaded");
        return -1;
    }
    if (g_ld.h) {
        ADXF_Close(g_ld.h);
        g_ld.h = 0;
    }
    g_ld.ptid = -1;
    g_ld.state = 1;
    if (!scr || size <= 0) {
        Err("E0013: ADXF: no buffer");
        g_ld.state = ADXF_STAT_ERROR;
        return -3;
    }
    if ((u32)ptid >= ADXF_PARTITIONS || !rec) {
        Err("E0014: ADXF: partition id or record");
        g_ld.state = ADXF_STAT_ERROR;
        return -3;
    }
    g_ld.done = 0;
    g_ld.ptid = ptid;
    g_ld.state = ADXF_STAT_READING;
    __builtin_memset(rec, 0, 0x11C);
    g_ptab[ptid] = rec;
    AdxfHandle *h = name ? ADXF_Open(name, atr) : ADXF_OpenAfs(afsPtid, flid);
    if (!h) {
        Err("E0015: ADXF: the archive can't be opened");
        g_ld.state = ADXF_STAT_ERROR;
        return -1;
    }
    g_ld.h = h;
    g_psrc[ptid][0] = 0;
    g_ppack[ptid][0] = 0;
    if (name) {
        u32 i = 0;
        for (; i < 0x100 && name[i]; i++)
            rec[0x10 + i] = (u8)name[i];
        for (; i < 0x100; i++)
            rec[0x10 + i] = 0;
        Wr32(rec + 0x110, (u32)atr);
        ChainName(g_psrc[ptid], name);
    } else {
        u32 w;
        s32 ofs, sct;
        char key[ADXF_CHAIN];
        u32 plba, psize;
        AfsFile(afsPtid, flid, (char *)rec + 0x10, &w, &ofs, &sct);
        Wr32(rec + 0x110, w);
        Wr32(rec + 0x114, (u32)ofs);
        PackKey(key, afsPtid, flid);
        if (key[0]) {
            ChainJoin(g_psrc[ptid], key, -1);
            if (Io_DiscPackFind(key, &plba, &psize) == 0)   /* the archive is the pack's: its entries are read from there */
                ChainJoin(g_ppack[ptid], key, -1);
        }
    }
    rec[0xF] = (u8)kind;
    g_ld.scr = scr;
    g_ld.scrsct = size >> 11;
    s32 r = ADXF_ReadNw(h, size >> 11, scr);
    if (r < 0) {
        ADXF_Close(h);
        g_ld.state = ADXF_STAT_ERROR;
        return r;
    }
    return 0;
}

s32 ADXF_LoadPartitionNw(s32 ptid, char *fname, void *dir, void *ptinfo);
static s32 ADXF_LoadPartitionNwWork(s32 ptid, char *fname, void *dir, void *ptinfo)
{
    return LoadStart(ptid, 0, 0, fname, dir, ptinfo, g_scratch, 0x800, 0);
}

s32 func_0015E628(s32 ptid, char *fname, void *dir, void *ptinfo);
static s32 func_0015E628Work(s32 ptid, char *fname, void *dir, void *ptinfo)
{
    return LoadStart(ptid, 0, 0, fname, dir, ptinfo, g_scratch, 0x800, 1);
}

s32 ADXF_LoadPartitionFromAfsNw(s32 ptid, s32 rd_ptid, s32 rd_flid, void *ptinfo);
static s32 ADXF_LoadPartitionFromAfsNwWork(s32 ptid, s32 rd_ptid, s32 rd_flid, void *ptinfo)
{
    return LoadStart(ptid, rd_ptid, rd_flid, 0, 0, ptinfo, g_scratch, 0x800, 0);
}

/* ADX's bytes-to-sectors (func_0015E3E8): toward zero, plus one for a rest. */
static s32 Sectors(s32 size)
{
    s32 q = size / 0x800;
    return q + (size - q * 0x800 > 0 ? 1 : 0);
}

static s32 LoadFail(void)
{
    Err("E0016: ADXF: the table of contents");
    LoaderEnd();
    g_ld.state = ADXF_STAT_ERROR;
    return ADXF_STAT_ERROR;
}

/* func_0015EAC8: the poll; each ended sector's entries go into the record. */
s32 ADXF_GetPtStat(s32 ptid);
static s32 ADXF_GetPtStatWork(s32 ptid)
{
    if (ptid != g_ld.ptid) {
        Err("E0017: ADXF_GetPtStat: another partition");
        return -3;
    }
    if (!g_ld.h)
        return g_ld.state;
    s32 st = ADXF_GetStat(g_ld.h);
    g_ld.state = st;
    if (st != ADXF_STAT_READEND)
        return st;
    u8 *rec = g_ptab[ptid];
    const u8 *buf = g_ld.scr;
    u32 slot = 0;
    if (Rd32(rec + 8) == 0) {
        s32 count = (s32)Rd32(buf + 4);
        if (__builtin_memcmp(buf, "AFS", 3) != 0 || count > 0x10000)
            return LoadFail();
        Wr32(rec + 8, (u32)count);
        rec[0xC] = (u8)count;
        rec[0xD] = (u8)(count >> 8);
        Wr32(rec + 4, rec[0xF] == 1 ? 0x11Cu + 4u * (u32)(count + 1) : 0x118u + ((2u * (u32)(count + 1) + 3u) & ~3u));
        s32 first = (s32)Rd32(buf + 8) / 0x800;
        if (rec[0xF] == 1)
            Wr32(rec + 0x118, (u32)first);
        else {
            rec[0x118] = (u8)first;
            rec[0x119] = (u8)(first >> 8);
        }
        slot = 1;
    }
    s32 count = (s32)Rd32(rec + 8);
    for (; slot < (u32)g_ld.scrsct * 256u; slot++) {
        s32 n = Sectors((s32)Rd32(buf + 8 * slot + 4));
        s32 e = g_ld.done + 1;
        if (rec[0xF] == 1) {
            Wr32(rec + 0x118 + 4 * e, (u32)n);
        } else {
            if ((u32)n > 0xFFFFu)
                return LoadFail();
            rec[0x118 + 2 * e] = (u8)n;
            rec[0x119 + 2 * e] = (u8)(n >> 8);
        }
        g_ld.done++;
        if (g_ld.done >= count) {
            LoaderEnd();
            g_ld.state = ADXF_STAT_READEND;
            return ADXF_STAT_READEND;
        }
    }
    if (ADXF_ReadNw(g_ld.h, g_ld.scrsct, g_ld.scr) < 0)
        return LoadFail();
    g_ld.state = ((AdxfHandle *)g_ld.h)->state;
    return g_ld.state;
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls. */

ADXF ADXF_Open(char *fname, void *atr)
{
    return (ADXF)((void *)ADXF_OpenWork(fname, atr));
}

ADXF ADXF_OpenAfs(s32 ptid, s32 flid)
{
    return (ADXF)((void *)ADXF_OpenAfsWork(ptid, flid));
}

s32 ADXF_Stop(ADXF adxf)
{
    return ADXF_StopWork(adxf);
}

void ADXF_Close(ADXF adxf)
{
    ADXF_CloseWork(adxf);
}

s32 ADXF_ReadNw(ADXF adxf, s32 nsct, void *buf)
{
    return ADXF_ReadNwWork(adxf, nsct, buf);
}

s32 ADXF_GetStat(ADXF adxf)
{
    return ADXF_GetStatWork(adxf);
}

s32 ADXF_GetFsizeSct(ADXF adxf)
{
    return ADXF_GetFsizeSctWork(adxf);
}

s32 ADXF_LoadPartitionNw(s32 ptid, char *fname, void *dir, void *ptinfo)
{
    s32 r = (ADXF_LoadPartitionNwWork(ptid, fname, dir, ptinfo));
    if (r < 0)
        Pal_Poll();   /* a "not ready" (a polled wait, ADXF_LoadPartitionNw) */
    return r;
}

s32 func_0015E628(s32 ptid, char *fname, void *dir, void *ptinfo)
{
    s32 r = (func_0015E628Work(ptid, fname, dir, ptinfo));
    if (r < 0)
        Pal_Poll();   /* a "not ready" (a polled wait, func_0015E628) */
    return r;
}

s32 ADXF_LoadPartitionFromAfsNw(s32 ptid, s32 rd_ptid, s32 rd_flid, void *ptinfo)
{
    s32 r = (ADXF_LoadPartitionFromAfsNwWork(ptid, rd_ptid, rd_flid, ptinfo));
    if (r < 0)
        Pal_Poll();   /* a "not ready" (a polled wait, ADXF_LoadPartitionFromAfsNw) */
    return r;
}

s32 ADXF_GetPtStat(s32 ptid)
{
    s32 r = (ADXF_GetPtStatWork(ptid));
    if (r != 3)
        Pal_Poll();   /* a "not ready" (a polled wait, ADXF_GetPtStat) */
    return r;
}
