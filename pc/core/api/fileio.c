/* The fileio client (libkernl's sceOpen, sceClose, sceLseek, sceRead, sceWrite,
 * sceDevctl and sceFsReset).
 *
 * Written from the library's behaviour at its interface, as the original's
 * client shows it: the kernel calls the game can observe
 * (the semaphores it creates, the waits, the order of the signals) are the
 * SDK's, in its order. The client's private state (the table of 32 files, the
 * set-up word) is the core's own memory, as plan 4.3 allows.
 *
 * Each request: the lock; the client's set-up the first time; a semaphore of
 * its own; the RPC (sceSifCallRpc, blocking: its wait is the original's); the
 * unlock; the wait for the answer's command (FIO_CMD_ANSWER, whose handler
 * stores the result and signals the semaphore); the semaphore deleted. */
#include "common.h"
#include "io/io_fio.h"
#include "io/io_kernel.h"

#define FIO_FILES 32
#define SLOT_USED 0x10000000u
#define E_VERSION ((s32)0xFFFEFFFCu)   /* the IOP's version is not one the client accepts */
#define E_NODEV (-19)
#define E_BADF (-9)
#define E_INVAL (-22)
#define E_REFUSED (-11)

typedef struct FioSlot {
    s32 fd;      /* the IOP's */
    u32 flags;   /* SLOT_USED | the open's flags; 0 when free */
} FioSlot;

static struct {
    int haveLock, haveTable; /* their semaphores created */
    s32 lock;
    s32 tableSema[2];
    int setup;               /* the client is set up (sceFsReset clears it) */
    int bound;
    SifClientData client;
    FioSlot table[FIO_FILES];
    FioRequest req;
    u32 recv[4];             /* each call's receive area */
    char iopVersion[4];
    u32 twoRecords;
    void (*powerOff)(s32);
    s32 powerOffArg;
} g_fio;

static s32 NewSema(s32 init)
{
    SemaParam p;
    __builtin_memset(&p, 0, sizeof(p));
    p.maxCount = 1;
    p.initCount = init;
    return CreateSema(&p);
}

/* The lock (its semaphore created the first time). */
static void Lock(void)
{
    if (!g_fio.haveLock) {
        g_fio.lock = NewSema(1);
        g_fio.haveLock = 1;
    }
    WaitSema(g_fio.lock);
}

static void Unlock(void)
{
    SignalSema(g_fio.lock);
}

/* The table's two semaphores, created the first time; the first one back. */
static s32 TableSema(void)
{
    if (!g_fio.haveTable) {
        g_fio.tableSema[0] = NewSema(1);
        g_fio.tableSema[1] = NewSema(1);
        g_fio.haveTable = 1;
    }
    return g_fio.tableSema[0];
}

/* The handler of the IOP's answer: the result stored, the request's semaphore
 * signalled (in the SIF0 interrupt's handler). */
static void AnswerHandler(void *packet, void *data)
{
    const FioAnswer *a = (const FioAnswer *)((const u8 *)packet + IO_SIF_CMD_HDR);
    (void)data;
    if (a->result)
        *a->result = a->value;
    iSignalSema(a->sema);
}

/* The IOP's power-off command: the registered callback with its argument. */
static void PowerOffHandler(void *packet, void *data)
{
    (void)packet;
    (void)data;
    if (g_fio.powerOff)
        g_fio.powerOff(g_fio.powerOffArg);
}

/* The RPC, retried after a delay while it fails (1 ms doubling to 128 ms). */
static s32 Rpc(u32 fno, void *send, s32 ssize, s32 rsize)
{
    s32 d = 1000;
    for (;;) {
        s32 r = sceSifCallRpc(&g_fio.client, (s32)fno, 0, send, ssize, g_fio.recv, rsize, 0, 0);
        if (r >= 0)
            return r;
        DelayThread(d);
        if (d < 128000)
            d *= 2;
    }
}

/* The set-up: the RPC layer, the two command handlers, the bind (again while
 * the IOP has no server), the table cleared, the IOP's version. */
static s32 Setup(void)
{
    sceSifInitRpc(0);
    s32 on = Kern_LibDIntr();
    sceSifAddCmdHandler(FIO_CMD_ANSWER, AnswerHandler, 0);
    sceSifAddCmdHandler(FIO_CMD_POWEROFF, PowerOffHandler, 0);
    g_fio.powerOff = 0;
    g_fio.powerOffArg = 0;
    if (on)
        (void)Kern_LibEIntr();
    for (;;) {
        if (sceSifBindRpc(&g_fio.client, FIO_SID, 0) < 0)
            return E_INVAL;
        if (g_fio.client.serve)
            break;
    }
    s32 t = TableSema();
    WaitSema(t);
    for (int i = 0; i < FIO_FILES; i++)
        g_fio.table[i].flags = 0;
    SignalSema(t);
    static u32 send[2];
    Rpc(FIO_SETUP, send, sizeof(send), sizeof(FioSetupReply));
    g_fio.setup = 1;
    const FioSetupReply *r = (const FioSetupReply *)g_fio.recv;
    __builtin_memcpy(g_fio.iopVersion, r->version, 4);
    g_fio.twoRecords = r->records == 2;
    return 0;
}

/* 0 when the IOP's version is one the client accepts. */
static int VersionCheck(void)
{
    return __builtin_memcmp(g_fio.iopVersion, "3000", 4) != 0;
}

/* The first free slot, marked used; NULL when all are taken. */
static FioSlot *NewSlot(void)
{
    s32 t = TableSema();
    WaitSema(t);
    for (int i = 0; i < FIO_FILES; i++) {
        if (!g_fio.table[i].flags) {
            g_fio.table[i].flags = SLOT_USED;
            SignalSema(t);
            return &g_fio.table[i];
        }
    }
    SignalSema(t);
    return 0;
}

/* The fd's slot (fd below 32), read under the table's semaphore. */
static FioSlot *SlotOf(s32 fd)
{
    s32 t = TableSema();
    WaitSema(t);
    FioSlot *s = (u32)fd < FIO_FILES ? &g_fio.table[fd] : 0;
    SignalSema(t);
    return s;
}

/* The fd's slot under the lock; NULL (the lock released) when the fd is not an
 * open file or the client is not set up. */
static FioSlot *OpenSlot(s32 fd)
{
    FioSlot *s = SlotOf(fd);
    Lock();
    if (!s || !s->flags || !g_fio.setup) {
        Unlock();
        return 0;
    }
    return s;
}

/* A request: its semaphore and result word in the request. */
static void Begin(s32 *result)
{
    g_fio.req.sema = NewSema(0);
    g_fio.req.result = result;
}

static int g_refused;   /* the last request was refused (its first reply word 0) */

/* The end of every request: unlock, wait for the answer, delete the semaphore. */
static s32 Finish(s32 *result)
{
    s32 sema = g_fio.req.sema;
    Unlock();
    g_refused = g_fio.recv[0] == 0;
    if (g_refused) {
        DeleteSema(sema);
        return E_REFUSED;
    }
    WaitSema(sema);
    DeleteSema(sema);
    return *result;
}

static void CopyName(char *dst, const char *src)
{
    u32 n = 0;
    while (n < sizeof(g_fio.req.name) - 1 && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

s32 sceFsReset(void);
static s32 sceFsResetWork(void)
{
    __builtin_memset(g_fio.iopVersion, 0, 4);
    g_fio.setup = 0;
    return 0;
}

s32 sceOpen(void *name, s32 flags, s32 mode);
static s32 sceOpenWork(void *name, s32 flags, s32 mode)
{
    s32 result = 0;
    Lock();
    if (!g_fio.setup)
        Setup();
    if (VersionCheck()) {
        Unlock();
        return E_VERSION;
    }
    FioSlot *s = NewSlot();
    if (!s) {
        Unlock();
        return E_NODEV;
    }
    CopyName(g_fio.req.name, name);
    g_fio.req.flags = flags;
    g_fio.req.mode = mode;
    Begin(&result);
    Rpc(FIO_OPEN, &g_fio.req, sizeof(g_fio.req), 4);
    s32 r = Finish(&result);
    if (g_refused)   /* the slot stays reserved */
        return r;
    s32 t = g_fio.tableSema[0];
    WaitSema(t);
    if (r < 0) {
        s->flags = 0;
        SignalSema(t);
        return r;
    }
    s->fd = r;
    s->flags = SLOT_USED | (u32)flags;
    SignalSema(t);
    return (s32)(s - g_fio.table);
}

s32 sceClose(s32 fd);
static s32 sceCloseWork(s32 fd)
{
    s32 result = 0;
    FioSlot *s = OpenSlot(fd);
    if (!s)
        return E_BADF;
    Begin(&result);
    g_fio.req.fd = s->fd;
    Rpc(FIO_CLOSE, &g_fio.req, sizeof(g_fio.req), 4);
    s->flags = 0;
    s32 r = Finish(&result);
    return r < 0 ? r : 0;
}

s32 sceLseek(s32 fd, s32 offset, s32 whence);
static s32 sceLseekWork(s32 fd, s32 offset, s32 whence)
{
    s32 result = 0;
    FioSlot *s = OpenSlot(fd);
    if (!s)
        return E_BADF;
    Begin(&result);
    g_fio.req.fd = s->fd;
    g_fio.req.offset = offset;
    g_fio.req.whence = whence;
    Rpc(FIO_LSEEK, &g_fio.req, sizeof(g_fio.req), 4);
    return Finish(&result);
}

s32 sceRead(s32 fd, void *buf, s32 size);
static s32 sceReadWork(s32 fd, void *buf, s32 size)
{
    s32 result = 0;
    FioSlot *s = OpenSlot(fd);
    if (!s)
        return E_BADF;
    Begin(&result);
    g_fio.req.fd = s->fd;
    g_fio.req.buf = buf;
    g_fio.req.size = size;
    Rpc(FIO_READ, &g_fio.req, sizeof(g_fio.req), 4);
    return Finish(&result);
}

s32 sceWrite(s32 fd, void *buf, s32 size);
static s32 sceWriteWork(s32 fd, void *buf, s32 size)
{
    s32 result = 0;
    FioSlot *s = OpenSlot(fd);
    if (!s)
        return E_BADF;
    Begin(&result);
    g_fio.req.fd = s->fd;
    g_fio.req.buf = buf;
    g_fio.req.size = size;
    Rpc(FIO_WRITE, &g_fio.req, sizeof(g_fio.req), 4);
    return Finish(&result);
}

s32 sceDevctl(char *name, s32 cmd, void *arg, u32 arglen, void *buf, u32 buflen);
static s32 sceDevctlWork(char *name, s32 cmd, void *arg, u32 arglen, void *buf, u32 buflen)
{
    s32 result = 0;
    (void)arg;
    Lock();
    if (!g_fio.setup)
        Setup();
    CopyName(g_fio.req.name, name);
    if (arglen > 0x400u || buflen > 0x400u) {
        Unlock();
        return E_INVAL;
    }
    Begin(&result);
    g_fio.req.cmd = cmd;
    g_fio.req.arglen = arglen;
    g_fio.req.devbuf = buf;
    g_fio.req.buflen = buflen;
    Rpc(FIO_DEVCTL, &g_fio.req, sizeof(g_fio.req), 4);
    return Finish(&result);
}

/* The power-off callback's registration (Capcom's PowerOff_Init calls it once
 * at boot; the binary has no name for it): under the client's lock, the client
 * set up first if it isn't, the callback and its argument stored with the
 * interrupts off; the previous callback back). The caller's gp the SDK
 * keeps with it has no use in the core. */
s32 func_00118378(void (*func)(s32), s32 arg);
static s32 func_00118378Work(void (*func)(s32), s32 arg)
{
    Lock();
    if (!g_fio.setup)
        Setup();
    s32 on = Kern_LibDIntr();
    void (*old)(s32) = g_fio.powerOff;
    g_fio.powerOff = func;
    g_fio.powerOffArg = arg;
    if (on)
        (void)Kern_LibEIntr();
    Unlock();
    return (s32)old;
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls. */

s32 sceFsReset(void)
{
    return sceFsResetWork();
}

s32 sceOpen(void *name, s32 flags, s32 mode)
{
    return sceOpenWork(name, flags, mode);
}

s32 sceClose(s32 fd)
{
    return sceCloseWork(fd);
}

s32 sceLseek(s32 fd, s32 offset, s32 whence)
{
    return sceLseekWork(fd, offset, whence);
}

s32 sceRead(s32 fd, void *buf, s32 size)
{
    return sceReadWork(fd, buf, size);
}

s32 sceWrite(s32 fd, void *buf, s32 size)
{
    return sceWriteWork(fd, buf, size);
}

s32 sceDevctl(char *name, s32 cmd, void *arg, u32 arglen, void *buf, u32 buflen)
{
    return sceDevctlWork(name, cmd, arg, arglen, buf, buflen);
}

s32 func_00118378(void (*func)(s32), s32 arg)
{
    return func_00118378Work(func, arg);
}
