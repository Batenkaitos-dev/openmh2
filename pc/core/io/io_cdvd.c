/* The IOP's CD/DVD services (CDVDFSV), as the original's HLE answers them
 *: at the request, over the disc archive. */
#include "io/io.h"
#include "io/io_cdvd.h"
#include "io/io_kernel.h"

static s32 g_error;   /* the last read's (S-command 4) */

static void Put(IopRpcCall *c, u32 word, u32 v)
{
    if (c->recv_size >= 4 * (word + 1))
        ((u32 *)c->reply)[word] = v;
}

static u8 Bcd(u32 v)
{
    return (u8)(((v / 10u) << 4) | (v % 10u));
}

/* 0x80000592: sceCdInit. */
static void Init(IopRpcCall *c)
{
    Put(c, 0, 1);
}

/* 0x80000593: the S-commands. */
static void SCmd(IopRpcCall *c)
{
    switch (c->fno) {
    case 0x03:   /* the disc's type */
        Put(c, 0, CD_DISK_PS2DVD);
        return;
    case 0x04:   /* the last error */
        Put(c, 0, (u32)g_error);
        return;
    case 0x0C:   /* the drive's status */
        Put(c, 0, CD_STAT_PAUSE);
        return;
    case 0x01:   /* ReadClock: 1, then sceCdCLOCK in BCD (the kernel's calendar in its zone) */
        Put(c, 0, 1);
        if (c->recv_size >= 12) {
            u32 t[6];
            u8 *r = c->reply;
            Io_CalendarZone(Kern_CalendarOffset(), t);
            r[4] = 0;              /* stat */
            r[5] = Bcd(t[0]);
            r[6] = Bcd(t[1]);
            r[7] = Bcd(t[2]);
            r[8] = 0;
            r[9] = Bcd(t[3]);
            r[10] = Bcd(t[4]);
            r[11] = Bcd(t[5] % 100u);
        }
        return;
    default:     /* success (power-off 0x21: its status word 0) */
        Put(c, 0, 1);
        return;
    }
}

static void Read(IopRpcCall *c)
{
    const CdReadReq *q = c->data;
    if (q->mode[2] == 1 || q->mode[2] == 2)
        Io_Log("cdvd: a read with the sector pattern %u: read as 2048-byte sectors", q->mode[2], 0);
    g_error = 0;
    if (Io_DiscReadSectors(q->lbn, q->sectors, q->buf) != 0) {
        Io_Log("cdvd: read error at sector %u (%u sectors)", q->lbn, q->sectors);
        g_error = CD_ERR_READ;
        __builtin_memset(q->buf, 0, q->sectors * IO_SECTOR);
    }
    if (q->endRecord) {   /* no unaligned ends */
        q->endRecord[0] = 0;
        q->endRecord[1] = 0;
    }
    if (q->pos)
        *q->pos = q->sectors * IO_SECTOR;
}

/* 0x80000595: the N-commands. */
static void NCmd(IopRpcCall *c)
{
    switch (c->fno) {
    case 0x01:   /* read */
    case 0x03:   /* DVD read */
        Read(c);
        return;
    case 0x0E:   /* the drive's state */
        Put(c, 0, CD_COMPLETE);
        return;
    default:     /* seek, standby, stop, pause and the rest */
        Put(c, 0, 1);
        return;
    }
}

/* 0x80000597: sceCdSearchFile. The date is the original's: fixed. */
static void Search(IopRpcCall *c)
{
    CdSearchReq *q = c->data;
    CdFile f;
    u32 lba = 0, size = 0;
    __builtin_memset(&f, 0, sizeof(f));
    q->name[sizeof(q->name) - 1] = 0;
    int found = Io_DiscFind(q->name, &lba, &size) == 0;
    if (found) {
        static const u8 date[8] = {0x00, 0x00, 0x00, 0x12, 0x01, 0x01, 0xD6, 0x07};
        f.lsn = lba;
        f.size = size;
        const char *base = q->name;
        for (const char *p = q->name; *p; p++)
            if (*p == '\\')
                base = p + 1;
        for (u32 i = 0; i < 15 && base[i]; i++)
            f.name[i] = base[i];
        __builtin_memcpy(f.date, date, 8);
    }
    Io_Log("cdvd: search %s: sector %u", (u32)q->name, found ? lba : 0);
    if (q->dest)
        __builtin_memcpy(q->dest, &f, sizeof(f));
    Put(c, 0, found ? 1 : 0);
}

/* 0x8000059A: the drive's state; 0x8000059C: 1 (the original's). */
static void DiskReady(IopRpcCall *c)
{
    Put(c, 0, CD_COMPLETE);
}

static void Other(IopRpcCall *c)
{
    Put(c, 0, 1);
}

void IoCdvd_Register(void)
{
    g_error = 0;
    Iop_RegisterService(CD_SID_INIT, "cdvd init", Init);
    Iop_RegisterService(CD_SID_SCMD, "cdvd scmd", SCmd);
    Iop_RegisterService(CD_SID_NCMD, "cdvd ncmd", NCmd);
    Iop_RegisterService(CD_SID_SEARCH, "cdvd search", Search);
    Iop_RegisterService(CD_SID_DISKREADY, "cdvd diskready", DiskReady);
    Iop_RegisterService(CD_SID_59C, "cdvd 59C", Other);
}
