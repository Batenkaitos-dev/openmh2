/* The IOP's memory card service (mcserv 0x80000400), as the original's HLE
 * answers it (iop_mc.c, whose comment has the protocol's rules):
 * at the request, over PCSX2-format card images (host_mcfs.c). Port 0 holds
 * the image given by --memcard1, port 1 the one given by
 * --memcard2 (Io_Option); a missing file becomes a new formatted card.
 * Every change is written to the image when the call that made it returns. */
#include "io/io.h"
#include "io/io_host.h"
#include "io/io_kernel.h"
#include "io/io_mc.h"
#include "io/io_mcfs.h"

typedef struct Card {
    McFs *fs;
    int seen;   /* GetInfo has reported this card */
} Card;

static Card g_card[2];
static McDirInfo *g_snap;   /* the GetDir listing in progress */
static int g_snapN, g_snapPos, g_snapCap;

static Card *CardOf(s32 port, s32 slot)
{
    return (u32)port < 2 && slot == 0 && g_card[port].fs ? &g_card[port] : 0;
}

/* Handles are numbered per card: port * 16 + the card's own handle. */
static Card *CardFd(s32 fd, int *local)
{
    if (fd < 0 || fd >= 2 * MC_FDS_PER_CARD || !g_card[fd / MC_FDS_PER_CARD].fs)
        return 0;
    *local = fd % MC_FDS_PER_CARD;
    return &g_card[fd / MC_FDS_PER_CARD];
}

static void Name(McRequest *q)
{
    q->name[sizeof(q->name) - 1] = 0;
}

/* The changes' time stamp: the kernel's calendar, in JST. */
static void SetClock(void)
{
    u32 t[6];
    u8 jst[8];
    Io_CalendarJst(t);
    jst[0] = 0;
    jst[1] = (u8)t[0];
    jst[2] = (u8)t[1];
    jst[3] = (u8)t[2];
    jst[4] = (u8)t[3];
    jst[5] = (u8)t[4];
    jst[6] = (u8)t[5];
    jst[7] = (u8)(t[5] >> 8);
    mcfs_set_clock(jst);
}

static s32 GetInfo(McRequest *q)
{
    McBlock info;
    __builtin_memset(&info, 0, sizeof(info));
    s32 res;
    Card *cd = CardOf(q->port, q->slot);
    if (!cd) {
        res = MC_RES_NO_CARD;
    } else if (mcfs_formatted(cd->fs)) {
        info.type = 2;   /* a PS2 card */
        info.free = (s32)mcfs_free_clusters(cd->fs);
        info.formatted = 1;
        res = cd->seen ? 0 : -1;
        cd->seen = 1;
    } else {
        info.type = 2;
        res = MC_ERR_NOFORMAT;
        cd->seen = 1;
    }
    if (q->block)
        *q->block = info;
    return res;
}

static s32 GetDir(Card *cd, McRequest *q)
{
    if (q->flags == 0) {   /* a new listing: a snapshot, since the game deletes entries between calls */
        g_snapN = g_snapPos = 0;
        int n = mcfs_getdir(cd->fs, q->name, 0, 1 << 16, 0);
        if (n < 0)
            return n;
        if (n > g_snapCap) {
            Host_IoFree(g_snap);
            g_snap = Host_Alloc((u32)n * sizeof(McDirInfo));
            g_snapCap = g_snap ? n : 0;
        }
        g_snapN = g_snap ? mcfs_getdir(cd->fs, q->name, 0, n, g_snap) : 0;
    }
    s32 n = 0;
    while (n < q->maxent && g_snapPos < g_snapN) {
        const McDirInfo *d = &g_snap[g_snapPos++];
        if (q->table) {   /* sceMcTblGetDir, 64 bytes */
            u8 *o = (u8 *)q->table + (u32)n * 64;
            __builtin_memset(o, 0, 64);
            __builtin_memcpy(o + 0x00, d->created, 8);
            __builtin_memcpy(o + 0x08, d->modified, 8);
            __builtin_memcpy(o + 0x10, &d->size, 4);
            __builtin_memcpy(o + 0x14, &d->mode, 2);
            __builtin_memcpy(o + 0x16, &d->reserve1, 2);
            __builtin_memcpy(o + 0x18, &d->attr, 4);
            __builtin_memcpy(o + 0x1C, &d->pda_apl_no, 4);
            __builtin_memcpy(o + 0x20, d->name, 32);
        }
        n++;
    }
    return n;
}

static s32 Read(McRequest *q)
{
    int local;
    s32 res;
    Card *cd = CardFd(q->fd, &local);
    if (!cd)
        res = MC_ERR_NOENT;
    else if (q->size <= 0)
        res = 0;
    else
        res = mcfs_read(cd->fs, local, q->buffer, (u32)q->size);
    if (q->block) {   /* no head or tail bytes to copy */
        q->block->size1 = q->block->size2 = 0;
        q->block->dest1 = q->block->dest2 = 0;
    }
    return res;
}

static s32 Write(McRequest *q)
{
    static u8 *buf;
    static u32 cap;
    int local;
    Card *cd = CardFd(q->fd, &local);
    if (!cd)
        return MC_ERR_NOENT;
    u32 head = q->headSize > 16 ? 16 : q->headSize, size = (u32)q->size;
    if (head + size + 1 > cap) {
        Host_IoFree(buf);
        cap = head + size + 1;
        buf = Host_Alloc(cap);
        if (!buf) {
            cap = 0;
            return MC_ERR_CHANGED;
        }
    }
    __builtin_memcpy(buf, q->head, head);
    if (size)
        __builtin_memcpy(buf + head, q->buffer, size);
    s32 res = mcfs_write(cd->fs, local, buf, head + size);
    mcfs_commit(cd->fs);
    return res;
}

static void Service(IopRpcCall *c)
{
    McRequest *q = c->data;
    McAnswer *a = c->reply;
    s32 res = 0;
    int local;
    Card *cd;
    if (c->fno == MC_FNO_INIT) {
        if (c->recv_size >= sizeof(McAnswer)) {
            a->result = 0;
            a->mcserv = MC_MCSERV_VERSION;
            a->mcman = MC_MCMAN_VERSION;
        }
        return;
    }
    SetClock();
    switch (c->fno) {
    case MC_FNO_GETINFO:
        res = GetInfo(q);
        break;
    case MC_FNO_OPEN:   /* and Mkdir: flags 0x40 */
        cd = CardOf(q->port, q->slot);
        Name(q);
        if (!cd) {
            res = MC_RES_NO_CARD;
        } else {
            res = mcfs_open(cd->fs, q->name, q->flags);
            if (res >= 0 && !(q->flags & MCO_MKDIR))
                res += q->port * MC_FDS_PER_CARD;
            mcfs_commit(cd->fs);
        }
        Io_Log("mc: open(%s) -> %d", (u32)q->name, (u32)res);
        break;
    case MC_FNO_CLOSE:
        cd = CardFd(q->fd, &local);
        res = cd ? mcfs_close(cd->fs, local) : MC_ERR_NOENT;
        if (cd)
            mcfs_commit(cd->fs);
        break;
    case MC_FNO_SEEK:
        cd = CardFd(q->fd, &local);
        res = cd ? mcfs_seek(cd->fs, local, q->offset, q->origin) : MC_ERR_NOENT;
        break;
    case MC_FNO_READ:
        res = Read(q);
        break;
    case MC_FNO_WRITE:
        res = Write(q);
        break;
    case MC_FNO_FLUSH:
        cd = CardFd(q->fd, &local);
        res = cd ? mcfs_flush(cd->fs, local) : MC_ERR_NOENT;
        break;
    case MC_FNO_CHDIR: {   /* the previous directory to table */
        static char old[1024], cur[1024];
        cd = CardOf(q->port, q->slot);
        Name(q);
        if (!cd) {
            res = MC_RES_NO_CARD;
            break;
        }
        mcfs_chdir(cd->fs, ".", old, sizeof(old));
        res = mcfs_chdir(cd->fs, q->name, cur, sizeof(cur));
        if (q->table)
            __builtin_memcpy(q->table, old, Io_StrLen(old) + 1);
        break;
    }
    case MC_FNO_GETDIR:
        cd = CardOf(q->port, q->slot);
        Name(q);
        res = cd ? GetDir(cd, q) : MC_RES_NO_CARD;
        break;
    case MC_FNO_SETINFO: {   /* and Rename: flags 0x10 */
        cd = CardOf(q->port, q->slot);
        Name(q);
        if (!cd) {
            res = MC_RES_NO_CARD;
            break;
        }
        McDirInfo info;
        const u8 *o = q->table;
        __builtin_memset(&info, 0, sizeof(info));
        __builtin_memcpy(info.created, o + 0x00, 8);
        __builtin_memcpy(info.modified, o + 0x08, 8);
        __builtin_memcpy(&info.size, o + 0x10, 4);
        __builtin_memcpy(&info.mode, o + 0x14, 2);
        __builtin_memcpy(&info.reserve1, o + 0x16, 2);
        __builtin_memcpy(&info.attr, o + 0x18, 4);
        __builtin_memcpy(&info.pda_apl_no, o + 0x1C, 4);
        __builtin_memcpy(info.name, o + 0x20, 32);
        info.name[31] = 0;
        res = mcfs_setinfo(cd->fs, q->name, &info, (u32)q->flags);
        mcfs_commit(cd->fs);
        break;
    }
    case MC_FNO_DELETE:
        cd = CardOf(q->port, q->slot);
        Name(q);
        res = cd ? mcfs_delete(cd->fs, q->name) : MC_RES_NO_CARD;
        if (cd)
            mcfs_commit(cd->fs);
        break;
    case MC_FNO_FORMAT:
    case MC_FNO_UNFORMAT:
        cd = CardOf(q->port, q->slot);
        if (cd) {
            res = c->fno == MC_FNO_FORMAT ? mcfs_format(cd->fs) : mcfs_unformat(cd->fs);
            mcfs_commit(cd->fs);
        } else {
            res = MC_RES_NO_CARD;
        }
        break;
    case MC_FNO_GETENTSPACE:
        cd = CardOf(q->port, q->slot);
        Name(q);
        res = cd ? mcfs_getentspace(cd->fs, q->name) : MC_RES_NO_CARD;
        break;
    case 0x14:   /* ChangeThreadPriority */
        res = 0;
        break;
    case 0x15:   /* GetSlotMax */
        res = 1;
        break;
    default:
        Io_Log("mc: mcserv function 0x%X is not written", c->fno, 0);
        res = MC_ERR_CHANGED;
        break;
    }
    if (c->recv_size >= 4)
        a->result = res;
}

int IoMc_Load(int port, const char *path)
{
    if ((u32)port >= 2)
        return -1;
    if (g_card[port].fs)
        mcfs_free(g_card[port].fs);
    g_card[port].fs = 0;
    g_card[port].seen = 0;
    if (!path) {
        return 0;
    }
    g_card[port].fs = mcfs_load(path, 1);
    if (!g_card[port].fs) {
        Io_Log("mc: memory card %u: cannot use %s; the slot stays empty", (u32)port + 1, (u32)path);
        return -1;
    }
    Io_Log("mc: memory card %u: %s", (u32)port + 1, (u32)path);
    return 0;
}

int IoMc_SaveCopy(int port, const char *path)
{
    if ((u32)port >= 2 || !g_card[port].fs)
        return -1;
    return mcfs_save_copy(g_card[port].fs, path);
}

void IoMc_Register(void)
{
    Iop_RegisterService(MC_SID, "mcserv", Service);
}
