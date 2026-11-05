/* The IOP's file service, fileio (SIF service 0x80000001), as the original's
 * HLE service answers (iop_misc.c, "file I/O"): at the request, with
 * the disc archive's files ("cdrom0:" and "cdrom:") and rom0:ROMVER; any other
 * device's file does not exist, and every other request fails with EIO. */
#include "io/io.h"
#include "io/io_fio.h"
#include "io/io_host.h"
#include "io/io_kernel.h"
#include "pal_arena.h"

typedef struct IopFile {
    int used;
    int rom;            /* rom0:ROMVER, from RomVer */
    u32 lba, size, pos;
    u8 *data;           /* a disc file's sectors, read whole at the open */
} IopFile;

static IopFile g_files[FIO_IOP_FILES];

/* The BIOS version of a Japanese retail console (v2.20), as the original game
 * answers it. */
static const char RomVer[] = "0220JC20060210\n";

void IoFio_Reset(void)
{
    for (int i = 0; i < FIO_IOP_FILES; i++)
        Pal_ArenaFree(g_files[i].data);
    __builtin_memset(g_files, 0, sizeof(g_files));
}

static void Answer(const FioRequest *q, u32 fno, s32 value)
{
    FioAnswer a;
    a.sema = q->sema;
    a.result = q->result;
    a.value = value;
    a.fno = fno;
    Iop_SendCmd(FIO_CMD_ANSWER, &a, sizeof(a));
}

static IopFile *FileOf(s32 fd)
{
    s32 i = fd - FIO_FIRST_FD;
    if (i < 0 || i >= FIO_IOP_FILES || !g_files[i].used)
        return 0;
    return &g_files[i];
}

static s32 Open(const char *name)
{
    IopFile f;
    __builtin_memset(&f, 0, sizeof(f));
    if (!Io_StrNCmp(name, "cdrom0:", 7) || !Io_StrNCmp(name, "cdrom:", 6)) {
        /* The whole file, at the open, as the original's (one disc read:
         * its sectors rounded up). */
        if (Io_DiscFind(name, &f.lba, &f.size) != 0)
            return FIO_ENOENT;
        u32 sectors = (f.size + IO_SECTOR - 1) / IO_SECTOR;
        f.data = Pal_ArenaAlloc(sectors * IO_SECTOR + 1);   /* the arena: a state holds it */
        if (!f.data || Io_DiscReadSectors(f.lba, sectors, f.data) != 0) {
            Pal_ArenaFree(f.data);
            return FIO_ENOENT;
        }
    } else if (!Io_StrNCmp(name, "rom0:ROMVER", 12)) {
        f.rom = 1;
        f.size = sizeof(RomVer) - 1;
    } else {
        return FIO_ENOENT;
    }
    for (int i = 0; i < FIO_IOP_FILES; i++) {
        if (!g_files[i].used) {
            f.used = 1;
            g_files[i] = f;
            return i + FIO_FIRST_FD;
        }
    }
    Pal_ArenaFree(f.data);
    return FIO_EMFILE;
}

static s32 Read(s32 fd, void *buf, s32 size)
{
    IopFile *f = FileOf(fd);
    if (!f)
        return FIO_EBADF;
    u32 left = f->size - f->pos;
    u32 n = (u32)size < left ? (u32)size : left;
    if (f->rom)
        __builtin_memcpy(buf, RomVer + f->pos, n);
    else
        __builtin_memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    return (s32)n;
}

static s32 Lseek(s32 fd, s32 offset, s32 whence)
{
    IopFile *f = FileOf(fd);
    if (!f)
        return FIO_EBADF;
    s64 base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : (s64)f->size;
    s64 np = base + offset;
    if (np < 0 || np > (s64)f->size)
        return FIO_EINVAL;
    f->pos = (u32)np;
    return (s32)np;
}

static void Service(IopRpcCall *c)
{
    FioRequest *q = c->data;
    if (c->fno == FIO_SETUP) {   /* the set-up: the IOP's version and one result record */
        FioSetupReply *r = c->reply;
        if (c->recv_size >= sizeof(*r)) {
            __builtin_memcpy(r->version, "3000", 4);
            r->records = 1;
        }
        return;
    }
    if (c->recv_size >= 4)
        *(u32 *)c->reply = 1;   /* accepted */
    s32 v;
    switch (c->fno) {
    case FIO_OPEN:
        q->name[sizeof(q->name) - 1] = 0;
        v = Open(q->name);
        Io_Log("fileio: open(%s) -> %d", (u32)q->name, (u32)v);
        break;
    case FIO_CLOSE: {
        IopFile *f = FileOf(q->fd);
        v = f ? 0 : FIO_EBADF;
        if (f) {
            Pal_ArenaFree(f->data);
            __builtin_memset(f, 0, sizeof(*f));
        }
        break;
    }
    case FIO_READ:
        v = Read(q->fd, q->buf, q->size);
        break;
    case FIO_LSEEK:
        v = Lseek(q->fd, q->offset, q->whence);
        break;
    default:   /* write, devctl and the rest: EIO, as the original game */
        Io_Log("fileio: request 0x%X answered EIO", c->fno, 0);
        v = FIO_EIO;
        break;
    }
    Answer(q, c->fno, v);
}

void IoFio_Register(void)
{
    Iop_RegisterService(FIO_SID, "fileio", Service);
}
