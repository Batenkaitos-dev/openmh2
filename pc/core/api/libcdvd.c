/* The CD/DVD client (libcdvd): sceCdInit, sceCdMmode, sceCdSync,
 * sceCdDiskReady, sceCdStandby, sceCdPowerOff, and the calls CRI's file system
 * makes (io_cdvd.h).
 *
 * Written from the library's behaviour at its interface, as the original's
 * client shows it: the kernel calls the game can observe (the four
 * semaphores sceCdInit makes, the binds and RPCs with their waits, PollSema and
 * SignalSema on the commands' semaphores, DelayThread in sceCdSync's wait) are
 * the SDK's, in its order. The library's words are the core's own memory. */
#include "common.h"
#include "libcdvd.h"
#include "io/io_cdvd.h"
#include "io/io_kernel.h"

static struct {
    s32 semN, sem1, semS, sem3;   /* the N-command's, the first (N-commands), the S-commands', the fourth */
    int ncb;                      /* a callback is set (sceCdCallback: never by the game) */
    u32 scmd, ncmdNo;             /* the commands in progress */
    int ncmd;                     /* an N-command in progress (sceCdSync) */
    u32 nrec;                     /* an N-command's end record: 1 for a read, 5 for standby */
    s32 nbind, sbind, srbind, drbind, initBind, w48;   /* negative: the server is to be bound */
    u32 rflags;                   /* bit 0: no N-command 0xE before a read */
    u32 inits;                    /* sceCdInit's count */
    int busy;                     /* sceCdInit is running */
    int handler;                  /* the callback command's handler is registered */
    SifClientData clInit, clS, clN, clSearch, clDr;
    u32 sendInit, sendS, sendDr;
    u32 recv[4];                  /* the answers */
    u32 recvN, recvDr, recvSearch;
    u32 endRecord[2], pos;
    CdReadReq read;
    CdSearchReq search;
    int ready;                    /* the words' first values set */
} g_cd;

/* The library's words before any call: no semaphores, every server to bind. */
static void Defaults(void)
{
    if (g_cd.ready)
        return;
    g_cd.ready = 1;
    g_cd.semN = g_cd.sem1 = g_cd.semS = g_cd.sem3 = -1;
    g_cd.nbind = g_cd.sbind = g_cd.srbind = g_cd.drbind = g_cd.initBind = g_cd.w48 = -1;
}

/* Binds a server, again while the IOP has none. */
static void BindWait(SifClientData *cl, u32 sid)
{
    for (;;) {
        sceSifBindRpc(cl, sid, 0);
        if (cl->serve)
            return;
    }
}

static s32 Call(SifClientData *cl, u32 fno, void *send, s32 ssize, void *recv, s32 rsize)
{
    return sceSifCallRpc(cl, (s32)fno, 0, send, ssize, recv, rsize, 0, 0);
}

static s32 NewSema(s32 init)
{
    SemaParam p;
    __builtin_memset(&p, 0, sizeof(p));
    p.maxCount = 1;
    p.initCount = init;
    return CreateSema(&p);
}

/* The four semaphores, made when any of the last three is missing. */
static void SemSetup(void)
{
    Defaults();
    if (g_cd.sem1 != -1 && g_cd.semS != -1 && g_cd.sem3 != -1)
        return;
    g_cd.sem1 = NewSema(1);
    g_cd.semS = NewSema(1);
    g_cd.sem3 = NewSema(1);
    g_cd.semN = NewSema(0);
    g_cd.ncmd = 0;
}

static void CallbackHandler(void *packet, void *data)
{
    (void)packet;
    (void)data;
}

/* The semaphores and the callback command's handler removed. */
static void SemRemove(void)
{
    if (g_cd.ncb)
        SignalSema(g_cd.semN);
    DeleteSema(g_cd.sem1);
    DeleteSema(g_cd.semS);
    DeleteSema(g_cd.sem3);
    DeleteSema(g_cd.semN);
    s32 on = Kern_LibDIntr();
    sceSifRemoveCmdHandler(CD_CMD_CALLBACK);
    g_cd.handler = 0;
    if (on)
        (void)Kern_LibEIntr();
}

static void AddHandler(void)
{
    g_cd.busy = 1;
    s32 on = Kern_LibDIntr();
    sceSifAddCmdHandler(CD_CMD_CALLBACK, CallbackHandler, 0);
    if (on)
        (void)Kern_LibEIntr();
    g_cd.busy = 0;
    g_cd.handler = 1;
    g_cd.w48 = 1;
}

/* Whether an S-command RPC is in flight; mode 0 waits until none is. */
static s32 SBusy(int mode)
{
    if (mode)
        return sceSifCheckStatRpc(&g_cd.clS);
    while (sceSifCheckStatRpc(&g_cd.clS))
        DelayThread(4000);
    return 0;
}

/* Before an S-command: 1, the S-command semaphore taken; 0 when it isn't free
 * or another S-command RPC is in flight. */
static int SBegin(u32 cmd)
{
    SemSetup();
    if (PollSema(g_cd.semS) != g_cd.semS)
        return 0;
    g_cd.scmd = cmd;
    if (SBusy(1)) {
        SignalSema(g_cd.semS);
        return 0;
    }
    sceSifInitRpc(0);
    if (g_cd.sbind < 0) {
        BindWait(&g_cd.clS, CD_SID_SCMD);
        g_cd.sbind = 0;
    }
    return 1;
}

/* Before an N-command, the same with the first semaphore and sceCdSync(1). */
static int NBegin(u32 cmd)
{
    SemSetup();
    if (PollSema(g_cd.sem1) != g_cd.sem1)
        return 0;
    g_cd.ncmdNo = cmd;
    if (sceCdSync(1, 0)) {
        SignalSema(g_cd.sem1);
        return 0;
    }
    sceSifInitRpc(0);
    if (g_cd.nbind < 0) {
        BindWait(&g_cd.clN, CD_SID_NCMD);
        g_cd.nbind = 0;
    }
    return 1;
}

/* An N-command's end (the RPC's end function, in the SIF's handler). */
static void NEnd(void *rec)
{
    u32 v = *(u32 *)rec;
    g_cd.nrec = v;
    if (v == 0xB) {
        g_cd.ncmd = 0;
        g_cd.nrec = 0;
        return;
    }
    iSignalSema(g_cd.sem1);
    g_cd.ncmd = 0;
    g_cd.nrec = 0;
}

/* A read's end: the unaligned ends (none here), then the N-command's end. */
static void ReadEnd(void *rec)
{
    (void)rec;
    NEnd(&g_cd.nrec);
}

s32 sceCdSync(s32 mode, s32 unused);
static s32 sceCdSyncWork(s32 mode, s32 unused)
{
    (void)unused;
    Defaults();
    if (mode)
        return g_cd.ncmd ? 1 : sceSifCheckStatRpc(&g_cd.clN);
    while (g_cd.ncmd || sceSifCheckStatRpc(&g_cd.clN))
        DelayThread(4000);
    return 0;
}

s32 sceCdInit(s32 mode);
static s32 sceCdInitWork(s32 mode)
{
    Defaults();
    if (SBusy(1))
        return 0;
    sceSifInitRpc(0);
    g_cd.busy = 1;
    g_cd.rflags = 0;
    g_cd.nbind = -1;
    g_cd.w48 = g_cd.srbind = g_cd.drbind = g_cd.sbind = g_cd.initBind = -1;
    u32 count = g_cd.inits++;
    (void)count;
    BindWait(&g_cd.clInit, CD_SID_INIT);
    g_cd.initBind = 0;
    g_cd.sendInit = (u32)mode;
    if (Call(&g_cd.clInit, 0, &g_cd.sendInit, 4, g_cd.recv, 0x10) < 0) {
        g_cd.busy = 0;
        return 0;
    }
    s32 res = (s32)g_cd.recv[1] < 0x200 || (s32)g_cd.recv[2] < 0x200 ? 2 : 1;
    g_cd.busy = 0;
    if (mode == 5) {
        SemRemove();
        g_cd.semN = g_cd.sem1 = g_cd.semS = -1;
    } else {
        SemSetup();
        AddHandler();
    }
    return res;
}

s32 sceCdMmode(s32 media);
static s32 sceCdMmodeWork(s32 media)
{
    if (!SBegin(0x22))
        return 0;
    g_cd.sendS = (u32)media;
    if (Call(&g_cd.clS, 0x22, &g_cd.sendS, 4, g_cd.recv, 4) < 0) {
        SignalSema(g_cd.semS);
        return 0;
    }
    s32 r = (s32)g_cd.recv[0];
    SignalSema(g_cd.semS);
    return r;
}

s32 sceCdPowerOff(volatile s32 *stat);
static s32 sceCdPowerOffWork(volatile s32 *stat)
{
    if (!SBegin(0x21))
        return 0;
    if (Call(&g_cd.clS, 0x21, 0, 0, g_cd.recv, 8) < 0) {
        SignalSema(g_cd.semS);
        return 0;
    }
    *stat = (s32)g_cd.recv[1];
    s32 r = (s32)g_cd.recv[0];
    SignalSema(g_cd.semS);
    return r;
}

/* sceCdSync(1)'s answer without the call (CRI's checks of its reads). */
s32 IoCd_Busy(void)
{
    Defaults();
    return g_cd.ncmd ? 1 : sceSifCheckStatRpc(&g_cd.clN);
}

/* The S-command 4 (the last error): its answer, or -1. */
s32 IoCd_GetError(void)
{
    if (!SBegin(3))
        return -1;
    if (Call(&g_cd.clS, 4, 0, 0, g_cd.recv, 4) < 0) {
        SignalSema(g_cd.semS);
        return -1;
    }
    s32 r = (s32)g_cd.recv[0];
    SignalSema(g_cd.semS);
    return r;
}

/* The N-command 0xE (the drive's state): its answer, or 0. */
s32 IoCd_DiskReady(void)
{
    if (!NBegin(2))
        return 0;
    if (Call(&g_cd.clN, 0xE, 0, 0, &g_cd.recvN, 4) < 0) {
        SignalSema(g_cd.sem1);
        return 0;
    }
    s32 r = (s32)g_cd.recvN;
    SignalSema(g_cd.sem1);
    return r;
}

/* The drive's helper for sceCdDiskReady (the server 0x8000059A). */
static s32 DiskReadyHelper(s32 mode)
{
    if (Kern_LibDIntr())
        (void)Kern_LibEIntr();
    SemSetup();
    if (PollSema(g_cd.semS) != g_cd.semS)
        return 6;
    if (SBusy(1)) {
        SignalSema(g_cd.semS);
        return 6;
    }
    sceSifInitRpc(0);
    if (g_cd.drbind < 0) {
        BindWait(&g_cd.clDr, CD_SID_DISKREADY);
        g_cd.drbind = 0;
    }
    g_cd.sendDr = (u32)mode;
    if (Call(&g_cd.clDr, 0, &g_cd.sendDr, 4, g_cd.recv, 4) < 0) {
        SignalSema(g_cd.semS);
        return 6;
    }
    s32 r = (s32)g_cd.recv[0];
    SignalSema(g_cd.semS);
    return r;
}

s32 sceCdDiskReady(s32 mode, s32 unused);
static s32 sceCdDiskReadyWork(s32 mode, s32 unused)
{
    (void)unused;
    SemSetup();
    if (PollSema(g_cd.sem3) != g_cd.sem3)
        return 6;
    sceSifInitRpc(0);
    if (g_cd.drbind < 0) {
        for (u32 n = 0;;) {
            if (sceSifBindRpc(&g_cd.clDr, CD_SID_59C, 0) >= 0) {
                if (g_cd.clDr.serve)
                    break;
                if (++n == 18) {
                    SignalSema(g_cd.sem3);
                    return DiskReadyHelper(mode);
                }
            }
        }
        g_cd.drbind = 0;
    }
    if (Kern_LibDIntr())
        (void)Kern_LibEIntr();
    g_cd.sendDr = (u32)mode;
    if (Call(&g_cd.clDr, 0, &g_cd.sendDr, 4, &g_cd.recvDr, 4) < 0) {
        SignalSema(g_cd.sem3);
        return 6;
    }
    s32 r = (s32)g_cd.recvDr;
    SignalSema(g_cd.sem3);
    return r;
}

s32 sceCdStandby(s32 unused1, s32 unused2);
static s32 sceCdStandbyWork(s32 unused1, s32 unused2)
{
    (void)unused1;
    (void)unused2;
    if (IoCd_DiskReady() == 6)
        return 0;
    if (!NBegin(0xA))
        return 0;
    g_cd.ncmd = 1;
    g_cd.nrec = 5;
    if (sceSifCallRpc(&g_cd.clN, 6, 1, 0, 0, 0, 0, NEnd, &g_cd.nrec) < 0) {
        g_cd.ncmd = 0;
        g_cd.nrec = 0;
        SignalSema(g_cd.sem1);
        return 0;
    }
    return 1;
}

/* A read for CRI's file system: started without a wait (its end through
 * sceCdSync); 1, or 0 when the drive is not ready or the command can't begin. */
s32 IoCd_Read(u32 lbn, u32 sectors, void *buf, const u8 *mode)
{
    if (!(g_cd.rflags & 1) && IoCd_DiskReady() == 6)
        return 0;
    if (!NBegin(4))
        return 0;
    g_cd.read.lbn = lbn;
    g_cd.read.sectors = sectors;
    g_cd.read.buf = buf;
    g_cd.read.mode[0] = mode[0];
    g_cd.read.mode[1] = mode[1];
    g_cd.read.mode[2] = mode[2];
    g_cd.read.endRecord = g_cd.endRecord;
    g_cd.read.pos = &g_cd.pos;
    g_cd.pos = 0;
    g_cd.ncmd = 1;
    g_cd.nrec = 1;
    if (sceSifCallRpc(&g_cd.clN, 1, 1, &g_cd.read, sizeof(g_cd.read), 0, 0, ReadEnd, g_cd.endRecord) < 0) {
        g_cd.ncmd = 0;
        g_cd.nrec = 0;
        SignalSema(g_cd.sem1);
        return 0;
    }
    return 1;
}

/* The file search: its answer (1 found), the record in file; 0 when the
 * command can't begin or the call fails. */
s32 IoCd_Search(CdFile *file, const char *name, s32 layer)
{
    SemSetup();
    if (PollSema(g_cd.sem1) != g_cd.sem1)
        return 0;
    g_cd.ncmdNo = 1;
    if (!sceCdSync(1, 0)) {
        sceSifInitRpc(0);
        if (g_cd.srbind < 0) {
            BindWait(&g_cd.clSearch, CD_SID_SEARCH);
            g_cd.srbind = 0;
        }
        u32 n = 0;
        while (n < 255 && name[n]) {
            g_cd.search.name[n] = name[n];
            n++;
        }
        g_cd.search.name[n] = 0;
        static CdFile record;
        g_cd.search.dest = &record;
        g_cd.search.layer = layer;
        if (Call(&g_cd.clSearch, 0, &g_cd.search, sizeof(g_cd.search), &g_cd.recvSearch, 4) >= 0) {
            *file = record;
            s32 r = (s32)g_cd.recvSearch;
            SignalSema(g_cd.sem1);
            return r;
        }
    }
    SignalSema(g_cd.sem1);
    return 0;
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls. */

s32 sceCdSync(s32 mode, s32 unused)
{
    return sceCdSyncWork(mode, unused);
}

s32 sceCdInit(s32 mode)
{
    return sceCdInitWork(mode);
}

s32 sceCdMmode(s32 media)
{
    return sceCdMmodeWork(media);
}

s32 sceCdPowerOff(volatile s32 *stat)
{
    return sceCdPowerOffWork(stat);
}

s32 sceCdDiskReady(s32 mode, s32 unused)
{
    return sceCdDiskReadyWork(mode, unused);
}

s32 sceCdStandby(s32 unused1, s32 unused2)
{
    return sceCdStandbyWork(unused1, unused2);
}
