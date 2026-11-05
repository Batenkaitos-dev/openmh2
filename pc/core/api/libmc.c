/* The memory card client (libmc): sceMcInit, sceMcSync and the request
 * functions.
 *
 * Written from the library's behaviour at its interface, as the original's
 * client shows it: the kernel
 * calls the game can observe are the SDK's, in its order. sceMcInit makes the
 * two semaphores, binds the server, and makes and starts the thread of the EE's
 * own RPC server (priority 0x68), which waits for ever (the core's IOP never
 * calls back, as the original's doesn't). Each request is sent without a wait
 * (sceSifCallRpc mode 1); its end function, run by the SIF's end handler,
 * signals the done semaphore; sceMcSync collects the answer. The library's
 * words are the core's own memory. */
#include "common.h"
#include "libmc.h"
#include "io/io_kernel.h"
#include "io/io_mc.h"

#define MC_BUSY (-200)
#define MC_FAIL (-91)
#define MC_NOSRV (-100)
#define MC_NONAME (-210)
#define MCO_MKDIR_FLAG 0x40   /* sceMcMkdir is an open with this mode */

s32 sceMcGetDir(s32 port, s32 slot, const char *name, u32 mode, s32 maxent, void *table);
s32 sceMcSetFileInfo(s32 port, s32 slot, const char *name, const void *info, u32 flags);

static struct {
    s32 cmd;               /* the request in progress (0: none) */
    int ended;             /* its end function has run (Mc_RequestPending) */
    s32 lock, done;        /* the semaphores (made when the lock's word is negative) */
    s32 tid;
    int ready;
    SifClientData client;
    McRequest req;
    McAnswer recv;
    McBlock block;
    s32 *type, *free, *format;   /* sceMcGetInfo's pointers, for its end function */
    u8 info[0x40];               /* sceMcSetFileInfo's copy of the record */
    u8 stack[0x800];             /* the server thread's */
} g_mc;

static void Defaults(void)
{
    if (!g_mc.ready) {
        g_mc.ready = 1;
        g_mc.lock = g_mc.done = -1;
    }
}

static s32 NewSema(s32 init)
{
    SemaParam p;
    __builtin_memset(&p, 0, sizeof(p));
    p.maxCount = 1;
    p.initCount = init;
    return CreateSema(&p);
}

s32 sceMcSync(s32 mode, s32 *cmd, s32 *result);
static s32 sceMcSyncWork(s32 mode, s32 *cmd, s32 *result)
{
    if (!g_mc.cmd)
        return -1;
    if (cmd)
        *cmd = g_mc.cmd;
    if (PollSema(g_mc.done) < 0) {
        if (mode)
            return 0;
        WaitSema(g_mc.done);
    }
    if (result)
        *result = g_mc.recv.result;
    g_mc.cmd = 0;
    SignalSema(g_mc.lock);
    return 1;
}

static void Unmake(void)
{
    DeleteSema(g_mc.lock);
    g_mc.lock = -1;
    DeleteSema(g_mc.done);
    g_mc.done = -1;
}

/* The EE's RPC server thread: its queue and server, then the loop, which sleeps
 * while no request comes (the core's IOP sends none). */
static void ServerThread(void *arg)
{
    (void)arg;
    GetThreadId();
    for (;;)
        SleepThread();
}

static s32 MakeThread(void)
{
    struct ThreadParam t;
    __builtin_memset(&t, 0, sizeof(t));
    t.entry = (void *)ServerThread;
    t.stack = g_mc.stack;
    t.stackSize = sizeof(g_mc.stack);
    t.initPriority = 0x68;
    s32 tid = CreateThread(&t);
    if (tid < 0)
        return -1;
    StartThread(tid, 0);
    return tid;
}

s32 sceMcInit(void);
static s32 sceMcInitWork(void)
{
    Defaults();
    if (g_mc.lock < 0) {
        g_mc.lock = NewSema(1);
        g_mc.done = NewSema(0);
    }
    sceMcSync(0, 0, 0);
    WaitSema(g_mc.lock);
    sceSifInitRpc(0);
    for (;;) {
        if (sceSifBindRpc(&g_mc.client, MC_SID, 0) < 0)
            for (;;)
                ;   /* the SDK's: a failing bind spins for ever */
        if (g_mc.client.serve)
            break;
        DelayThread(10000);
    }
    g_mc.tid = MakeThread();
    if (g_mc.tid < 0) {
        Unmake();
        return -101;
    }
    if (sceSifCallRpc(&g_mc.client, MC_FNO_INIT, 0, &g_mc.req, sizeof(g_mc.req), &g_mc.recv, sizeof(g_mc.recv), 0, 0)) {
        g_mc.client.serve = 0;
        Unmake();
        return MC_FAIL;
    }
    if (g_mc.recv.mcserv < 0x20A) {
        g_mc.client.serve = 0;
        Unmake();
        return -120;
    }
    if (g_mc.recv.mcman < 0x20E) {
        g_mc.client.serve = 0;
        Unmake();
        return -121;
    }
    SignalSema(g_mc.lock);
    return g_mc.recv.result;
}

/* The end functions (the SIF's end handler calls them). */
static void EndSignal(void *arg)
{
    (void)arg;
    g_mc.ended = 1;
    iSignalSema(g_mc.done);
}

static void EndCopy(void *arg)
{
    McBlock *b = arg;
    if (b->size1 > 0)
        __builtin_memcpy(b->dest1, (u8 *)b + 0x10, (u32)b->size1);   /* never: the core's IOP sends no ends */
    g_mc.ended = 1;
    iSignalSema(g_mc.done);
}

static void EndInfo(void *arg)
{
    McBlock *b = arg;
    if (g_mc.type)
        *g_mc.type = b->type;
    if (g_mc.free)
        *g_mc.free = b->free;
    if (g_mc.format)
        *g_mc.format = b->formatted;
    g_mc.ended = 1;
    iSignalSema(g_mc.done);
}

/* No server: -100; the lock not free: -200; else 0, the lock taken. */
static s32 Lock(void)
{
    Defaults();
    if (!g_mc.client.serve)
        return MC_NOSRV;
    if (PollSema(g_mc.lock) < 0)
        return MC_BUSY;
    return 0;
}

static s32 Send(u32 fno, void (*end)(void *), void *arg, s32 cmd)
{
    g_mc.ended = 0;
    if (sceSifCallRpc(&g_mc.client, (s32)fno, 1, &g_mc.req, sizeof(g_mc.req), &g_mc.recv, 4, end, arg)) {
        SignalSema(g_mc.lock);
        return MC_FAIL;
    }
    g_mc.cmd = cmd;
    return 0;
}

/* An empty name: the lock back, -210. Else the name into the request. */
static int NoName(const char *name)
{
    if (name[0])
        return 0;
    SignalSema(g_mc.lock);
    return 1;
}

static void CopyName(const char *name)
{
    u32 n = 0;
    while (n < 0x3FF && name[n]) {
        g_mc.req.name[n] = name[n];
        n++;
    }
    while (n < 0x400)
        g_mc.req.name[n++] = 0;
}

s32 sceMcGetInfo(s32 port, s32 slot, s32 *type, s32 *free, s32 *format);
static s32 sceMcGetInfoWork(s32 port, s32 slot, s32 *type, s32 *free, s32 *format)
{
    s32 r = Lock();
    if (r)
        return r;
    g_mc.type = type;
    g_mc.free = free;
    g_mc.format = format;
    g_mc.req.port = port;
    g_mc.req.slot = slot;
    g_mc.req.block = &g_mc.block;
    return Send(MC_FNO_GETINFO, EndInfo, &g_mc.block, 1);
}

s32 sceMcOpen(s32 port, s32 slot, char *name, s32 mode);
static s32 sceMcOpenWork(s32 port, s32 slot, char *name, s32 mode)
{
    s32 r = Lock();
    if (r)
        return r;
    if (NoName(name))
        return MC_NONAME;
    g_mc.req.port = port;
    g_mc.req.slot = slot;
    g_mc.req.flags = mode;
    CopyName(name);
    return Send(MC_FNO_OPEN, EndSignal, 0, 2);
}

s32 sceMcMkdir(s32 port, s32 slot, char *name);
static s32 sceMcMkdirWork(s32 port, s32 slot, char *name)
{
    s32 r = sceMcOpen(port, slot, name, MCO_MKDIR_FLAG);
    if (!r)
        g_mc.cmd = 0xB;
    return r;
}

s32 sceMcClose(s32 fd);
static s32 sceMcCloseWork(s32 fd)
{
    s32 r = Lock();
    if (r)
        return r;
    g_mc.req.fd = fd;
    return Send(MC_FNO_CLOSE, EndSignal, 0, 3);
}

s32 sceMcRead(s32 fd, u8 *buf, s32 size);
static s32 sceMcReadWork(s32 fd, u8 *buf, s32 size)
{
    s32 r = Lock();
    if (r)
        return r;
    g_mc.req.fd = fd;
    g_mc.req.size = size;
    g_mc.req.buffer = buf;
    g_mc.req.block = &g_mc.block;
    return Send(MC_FNO_READ, EndCopy, &g_mc.block, 5);
}

/* A size up to 16 goes whole in the request's head; else the bytes up to the
 * buffer's 16-byte boundary do, and the IOP takes the rest from the buffer. */
s32 sceMcWrite(s32 fd, u8 *buf, s32 size);
static s32 sceMcWriteWork(s32 fd, u8 *buf, s32 size)
{
    s32 r = Lock();
    if (r)
        return r;
    u32 head, rest;
    u8 *start;
    if (size <= 16) {
        head = (u32)size;
        rest = 0;
        start = 0;
    } else {
        head = (16 - ((u32)buf & 15)) & 15;
        rest = (u32)size - head;
        start = buf + head;
    }
    g_mc.req.fd = fd;
    g_mc.req.size = (s32)rest;
    g_mc.req.headSize = head;
    g_mc.req.buffer = start;
    for (u32 i = 0; (s32)i < (s32)head; i++)
        g_mc.req.head[i] = buf[i];
    return Send(MC_FNO_WRITE, EndSignal, 0, 6);
}

s32 sceMcGetDir(s32 port, s32 slot, const char *name, u32 mode, s32 maxent, void *table);
static s32 sceMcGetDirWork(s32 port, s32 slot, const char *name, u32 mode, s32 maxent, void *table)
{
    s32 r = Lock();
    if (r)
        return r;
    if (NoName(name))
        return MC_NONAME;
    g_mc.req.port = port;
    g_mc.req.slot = slot;
    g_mc.req.flags = (s32)mode;
    g_mc.req.maxent = maxent;
    g_mc.req.table = table;
    CopyName(name);
    return Send(MC_FNO_GETDIR, EndSignal, 0, 0xD);
}

s32 sceMcFormat(s32 port, s32 slot);
static s32 sceMcFormatWork(s32 port, s32 slot)
{
    s32 r = Lock();
    if (r)
        return r;
    g_mc.req.port = port;
    g_mc.req.slot = slot;
    return Send(MC_FNO_FORMAT, EndSignal, 0, 0x10);
}

s32 sceMcDelete(s32 port, s32 slot, char *name);
static s32 sceMcDeleteWork(s32 port, s32 slot, char *name)
{
    s32 r = Lock();
    if (r)
        return r;
    if (NoName(name))
        return MC_NONAME;
    g_mc.req.port = port;
    g_mc.req.slot = slot;
    g_mc.req.flags = 0;
    CopyName(name);
    return Send(MC_FNO_DELETE, EndSignal, 0, 0xF);
}

s32 sceMcSetFileInfo(s32 port, s32 slot, const char *name, const void *info, u32 flags);
static s32 sceMcSetFileInfoWork(s32 port, s32 slot, const char *name, const void *info, u32 flags)
{
    s32 r = Lock();
    if (r)
        return r;
    if (NoName(name))
        return MC_NONAME;
    g_mc.req.port = port;
    g_mc.req.slot = slot;
    g_mc.req.flags = (s32)(flags & 7);
    __builtin_memcpy(g_mc.info, info, sizeof(g_mc.info));
    g_mc.req.table = g_mc.info;
    CopyName(name);
    return Send(MC_FNO_SETINFO, EndSignal, 0, 0xE);
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls (pc/core/verify/verify.h). */

s32 sceMcSync(s32 mode, s32 *cmd, s32 *result)
{
    return sceMcSyncWork(mode, cmd, result);
}

s32 sceMcInit(void)
{
    return sceMcInitWork();
}

s32 sceMcGetInfo(s32 port, s32 slot, s32 *type, s32 *free, s32 *format)
{
    return sceMcGetInfoWork(port, slot, type, free, format);
}

s32 sceMcOpen(s32 port, s32 slot, char *name, s32 mode)
{
    return sceMcOpenWork(port, slot, name, mode);
}

s32 sceMcMkdir(s32 port, s32 slot, char *name)
{
    return sceMcMkdirWork(port, slot, name);
}

s32 sceMcClose(s32 fd)
{
    return sceMcCloseWork(fd);
}

s32 sceMcRead(s32 fd, u8 *buf, s32 size)
{
    return sceMcReadWork(fd, buf, size);
}

s32 sceMcWrite(s32 fd, u8 *buf, s32 size)
{
    return sceMcWriteWork(fd, buf, size);
}

s32 sceMcGetDir(s32 port, s32 slot, const char *name, u32 mode, s32 maxent, void *table)
{
    return sceMcGetDirWork(port, slot, name, mode, maxent, table);
}

s32 sceMcFormat(s32 port, s32 slot)
{
    return sceMcFormatWork(port, slot);
}

s32 sceMcDelete(s32 port, s32 slot, char *name)
{
    return sceMcDeleteWork(port, slot, name);
}

s32 sceMcSetFileInfo(s32 port, s32 slot, const char *name, const void *info, u32 flags)
{
    return sceMcSetFileInfoWork(port, slot, name, info, flags);
}

/* Not a library function: the platform layer's question (pc/core/pal_freecam.c) whether a request is running (sent, its
 * end not yet arrived). The freeze does not take hold while one is. */
s32 Mc_RequestPending(void)
{
    return g_mc.cmd != 0 && !g_mc.ended;
}
