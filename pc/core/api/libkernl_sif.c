/*
 * libkernl's SIF part as Capcom's code calls it: the RPC layer (sceSifInitRpc,
 * sceSifBindRpc, sceSifCallRpc, sceSifCheckStatRpc), the IOP's reboot and
 * sync, the IOP heap's client, the loadfile client and SIF DMA. Their
 * behaviour at the interface is the PS2 library's; their timing is the
 * frame clock's (the
 * services answer at the request, the reply's RPC_END packet moves at a
 * delivery point). What the original game showed of the counts (one RPC, so one
 * blocking wait, per call; the loadfile client's bind and version check at
 * its first call after a reset) was seen in the original game.
 *
 * The packets and the RPC numbers are the library's own memory on the PS2;
 * here they are the kernel's (sif.c), and the client records the game holds
 * get the words the PS2's library writes. Each function the game calls is
 * logged; a `ret` record's "not
 * ready" comes after the tap's end (RetPoll).
 */
#include "common.h"
#include "sif.h"
#include "kernel/kern_int.h"
#include "kernel/ksif.h"
#include "kernel/klog.h"
#include "snd/snd.h"
#include "pal_core.h"

#define SIF_GP 0x001D05F0u              /* the boot executable's gp, which CallRpc stores in the client */
#define IOPHEAP_SERVICE 0x80000003u
#define LOADFILE_SERVICE 0x80000006u

static int sCmdUp;                       /* InitCmd ran (the SIF's DMAC handler is in) */
static s32 sSifHandler;                  /* its id */
static s32 sIopHeapUp = -1;              /* the IOP heap client's set-up flag */
static s32 sLoadFileUp = -1;             /* the loadfile client's: -1 until bound and checked */

/* ---------------------------------------------------------- the RPC layer */

/* The EE library's software register 0, which the IOP's SET_SREG(0, 1) sets (the RPC layer's init). */
static u32 sLibSreg0;

static void SetSreg0(const KSifPacket *p)
{
    (void)p;
    sLibSreg0 = 1;
}

/* sceSifInitRpc: once per reboot. InitCmd (the SIF's DMAC handler added and enabled, the EE command buffer
 * made known to the IOP), then the RPC layer's INIT_CMD, which the IOP answers with SET_SREG(0, 1); the call
 * waits for it in a library wait (polls.txt: `wait` sceSifInitRpc), whose arrival moves the packet. */
static void InitRpcWork(void)
{
    if (KSif_RpcUp()) {
        return;
    }
    if (!sCmdUp) {
        (void)KSif_DmaId();      /* InitCmd's INIT_CMD: the EE's command buffer */
        sCmdUp = 1;
        sSifHandler = KThread_AddHandler(1, 5, 0, 0, 0, 1, KSif_DmacHandler);
        KThread_EnableCause(1, 5, 1);
        KSif_SetBufferKnown(1);
    }
    KSif_RpcReset();
    if (!KSif_Sreg(2)) {          /* SIF register 0x80000002: the IOP's RPC layer is not set up yet */
        KSifPacket p;
        __builtin_memset(&p, 0, sizeof(p));
        p.fn = SetSreg0;
        sLibSreg0 = 0;
        (void)KSif_DmaId();      /* the RPC layer's INIT_CMD */
        KSif_Queue(&p);
        Kern_LibWaitArrive();
        while (!sLibSreg0) {
            Kern_LibWaitStep();
        }
        KSif_SetSreg(2, 1);
    }
    KSif_SetRpcUp(1);
}

void sceSifInitRpc(u32 mode)
{
    InitRpcWork();
}

void sceSifExitCmd(void)
{
    if (sCmdUp) {
        KThread_RemoveHandler(1, 5, sSifHandler);
        KThread_EnableCause(1, 5, 0);
        sCmdUp = 0;
    }
    KSif_SetRpcUp(0);
}

/* The client record (SifClientData) by words. */
static u32 *W(SifClientData *cd)
{
    return (u32 *)cd;
}

/* What the SIF's handler does with a bind's or a call's RPC_END (port-libraries.md, "The SIF layer
 * underneath"): the bind's server and buffer, or the call's end function; the client's semaphore signalled
 * if not negative; the packet freed and the client's word 0 cleared. */
static void RpcEnd(const KSifPacket *p)
{
    SifClientData *cd = (SifClientData *)p->ptr;
    u32 *w = W(cd);
    if (p->a == 0) {             /* a bind */
        w[9] = p->b;
        w[5] = p->c;
    } else if (w[7]) {           /* a call with an end function, in the handler */
        ((void (*)(void *))w[7])((void *)w[8]);
    }
    if ((s32)w[2] >= 0) {
        Kern_SemaSignal((s32)w[2]);
    }
    KSif_PacketFree(w[0]);
    w[0] = 0;
}

static void QueueEnd(SifClientData *cd, int call, u32 server, u32 buff)
{
    KSifPacket p;
    __builtin_memset(&p, 0, sizeof(p));
    p.fn = RpcEnd;
    p.ptr = cd;
    p.a = (u32)call;
    p.b = server;
    p.c = buff;
    KSif_Queue(&p);
}

/* The wait of a client's call: a semaphore for the call, the request (the service answers now or later), the
 * blocking wait, the semaphore deleted. */
static s32 WaitedRequest(SifClientData *cd, void (*send)(SifClientData *, void *), void *ctx)
{
    u32 *w = W(cd);
    s32 sema = Kern_SemaCreate(0, 1, 0, 0);
    w[2] = (u32)sema;
    if (sema < 0) {
        KSif_PacketFree(w[0]);
        return -3;
    }
    send(cd, ctx);
    Kern_SemaWait(sema);
    Kern_SemaDelete(sema);
    return 0;
}

static void SendBind(SifClientData *cd, void *ctx)
{
    u32 server, buff;
    (void)KSif_DmaId();          /* the BIND command */
    KSif_ServiceAddresses((u32)ctx, &server, &buff);
    QueueEnd(cd, 0, server, buff);
}

static s32 BindRpcWork(SifClientData *cd, u32 id, s32 mode)
{
    u32 *w = W(cd);
    u32 addr, num;
    w[4] = 0;
    w[9] = 0;
    if (!KSif_PacketAlloc(&addr, &num)) {
        return -1;
    }
    w[0] = addr;
    w[1] = num;
    if (mode & 1) {
        w[2] = 0xFFFFFFFFu;
        SendBind(cd, (void *)id);
        return 0;
    }
    return WaitedRequest(cd, SendBind, (void *)id);
}

s32 sceSifBindRpc(SifClientData *cd, u32 id, s32 mode)
{
    return BindRpcWork(cd, id, mode);
}

typedef struct CallArgs {
    s32 fno;
    u32 mode;
    void *send;
    s32 ssize;
    void *recv;
    s32 rsize;
} CallArgs;

static void SendCall(SifClientData *cd, void *ctx)
{
    const CallArgs *a = (const CallArgs *)ctx;
    u32 *w = W(cd);
    const KernSifService *s = KSif_ServiceByServer(w[9]);
    KernSifCall c;
    (void)KSif_DmaId();          /* the CALL command (its data and the packet in one SifSetDma) */
    if (!s || !s->call) {
        /* The original's runtime: a bound service with no server of its own (one bound without being
         * known) answers at once with a reply of zeros, recv_size bytes (at most 0x20000) to the receive
         * buffer, then RPC_END; a server record no bind gave answers RPC_END only. */
        if (KSif_ServerKnown(w[9]) && a->recv && a->rsize) {
            u32 n = (u32)a->rsize;
            __builtin_memset(a->recv, 0, n > 0x20000u ? 0x20000u : n);
        }
        QueueEnd(cd, 1, 0, 0);
        return;
    }
    __builtin_memset(&c, 0, sizeof(c));
    c.service = s->id;
    c.fno = a->fno;
    c.mode = a->mode;
    c.send = a->send;
    c.ssize = a->ssize;
    c.recv = a->recv;
    c.rsize = a->rsize;
    c.token = (u32)cd;
    KSif_BeginServiceCall();
    if (s->call(&c) == KERN_SIF_ANSWERED) {
        QueueEnd(cd, 1, 0, 0);
    }
    KSif_EndServiceCall();
}

/* A service that answered later (an IOP module's thread): the call's RPC_END now. */
void Kern_SifAnswer(u32 token)
{
    QueueEnd((SifClientData *)token, 1, 0, 0);
}

static s32 CallRpcWork(SifClientData *cd, s32 fno, u32 mode, void *send, s32 ssize, void *recv, s32 rsize,
                       void (*end_func)(void *), void *end_param)
{
    u32 *w = W(cd);
    u32 addr, num;
    CallArgs a;
    if (!KSif_PacketAlloc(&addr, &num)) {
        return -1;
    }
    w[0] = addr;
    w[1] = num;
    w[6] = SIF_GP;
    w[7] = (u32)end_func;
    w[8] = (u32)end_param;
    a.fno = fno;
    a.mode = mode;
    a.send = send;
    a.ssize = ssize;
    a.recv = recv;
    a.rsize = rsize;
    if (mode & 1) {
        w[2] = 0xFFFFFFFFu;
        SendCall(cd, &a);
        return 0;
    }
    return WaitedRequest(cd, SendCall, &a);
}

/* The tap's arguments end with end_func: end_param is the ninth, on the PS2's stack, and not in the table. */
s32 sceSifCallRpc(SifClientData *cd, s32 fno, u32 mode, void *send, s32 ssize, void *recv, s32 rsize,
                  void (*end_func)(void *), void *end_param)
{
    return CallRpcWork(cd, fno, mode, send, ssize, recv, rsize, end_func, end_param);
}

/* 1 while the client's call is in flight: its packet set and still the client's. */
s32 sceSifCheckStatRpc(SifClientData *cd)
{
    u32 *w = W(cd);
    return w[0] != 0 && KSif_PacketBusy(w[0], w[1]);
}

/* ------------------------------------------------------ the SIF's commands */

/* The EE's handler of an IOP-to-EE command (Kern_SifSendCmd: fn(packet, data) in the SIF's DMAC handler);
 * a second one for the same id replaces the first. */
s32 sceSifAddCmdHandler(u32 cid, KernSifCmdFn handler, void *data);
s32 sceSifRemoveCmdHandler(u32 cid);

s32 sceSifAddCmdHandler(u32 cid, KernSifCmdFn handler, void *data)
{
    KSif_SetCmdHandler(cid, handler, data);
    return 0;
}

s32 sceSifRemoveCmdHandler(u32 cid)
{
    KSif_SetCmdHandler(cid, 0, 0);
    return 0;
}

/* ------------------------------------------------------- the IOP's reboot */

/* The end of a call that polls: "not ready" (NOTREADY: 0 for "v0 == 0", -1 for "v0 < 0") is polled. */
static s32 RetPoll(s32 r, int notReady)
{
    if (notReady == 0 ? r == 0 : r < 0) {
        Pal_Poll();
    }
    return r;
}

/* sceSifRebootIop(image): 0 at once past 69 bytes of name; else the RPC layer and the command layer exit (the
 * SIF's DMAC handler removed), BOOTEND cleared, the RESET request: the SIF0 queue dropped, the command buffer
 * forgotten, the IOP heap reset, and 2 ms later the IOP is up again (sif.c). 1. */
static s32 RebootIopWork(const char *img)
{
    s32 n = 0;
    while (img[n]) {
        n++;
    }
    if (n > 69) {
        return 0;
    }
    sceSifExitCmd();
    (void)KSif_DmaId();          /* the RESET command */
    KSif_Reboot(img);
    sIopHeapUp = -1;
    sLoadFileUp = -1;
    return 1;
}

s32 sceSifRebootIop(const char *img)
{
    return RetPoll(RebootIopWork(img), 0);
}

/* sceSifSyncIop: 1 once the IOP has booted (SMFLAG's BOOTEND). A `ret` record: 0 is "not ready". */
static s32 SyncIopWork(void)
{
    if (KSif_BootEnd()) {
        return 1;
    }
    return 0;
}

s32 sceSifSyncIop(void)
{
    return RetPoll(SyncIopWork(), 0);
}

/* ------------------------------------------------------- the IOP heap */

static SifClientData sIopHeapClient;
static SifClientData sLoadFileClient;

static s32 InitIopHeapWork(void)
{
    if (sceSifBindRpc(&sIopHeapClient, IOPHEAP_SERVICE, 0) < 0) {
        return -1;
    }
    sIopHeapUp = 0;
    return 0;
}

s32 sceSifInitIopHeap(void)
{
    return InitIopHeapWork();
}

static void IopHeapCall(void)
{
    if (sceSifCallRpc(&sIopHeapClient, 0, 0, 0, 0, 0, 0, 0, 0) < 0) {
        KFatal("kernel: an IOP heap call failed");
    }
}

/* sceSifAllocIopHeap: 0 before the set-up; one RPC; the IOP heap's block (snd's heap, IopKern_HeapAlloc).
 * A `ret` record: 0 is "not ready". */
static u32 AllocIopHeapWork(s32 size)
{
    u32 a;
    if (sIopHeapUp < 0) {
        return 0;
    }
    IopHeapCall();
    a = IopKern_HeapAlloc((u32)size);
    return a;
}

/* The result is an IOP address: a word, never normalised. */
void *sceSifAllocIopHeap(s32 size)
{
    u32 a = (AllocIopHeapWork(size));
    if (!a) {
        Pal_Poll();
    }
    return (void *)a;
}

static s32 FreeIopHeapWork(void *addr)
{
    if (sIopHeapUp < 0) {
        return -1;
    }
    IopHeapCall();
    IopKern_HeapFree((u32)addr);
    return 0;
}

s32 sceSifFreeIopHeap(void *addr)
{
    return FreeIopHeapWork(addr);
}

/* ------------------------------------------------------- loadfile */

/* The loadfile client's set-up at its first use after a reset: the bind and the version check (two RPCs). */
static s32 LoadFileSetup(void)
{
    if (sLoadFileUp >= 0) {
        return 0;
    }
    if (sceSifBindRpc(&sLoadFileClient, LOADFILE_SERVICE, 0) < 0) {
        return -1;
    }
    sceSifCallRpc(&sLoadFileClient, 0xFF, 0, 0, 0, 0, 0, 0, 0);
    sLoadFileUp = 0;
    return 0;
}

s32 sceSifLoadFileReset(void)
{
    sLoadFileUp = -1;
    return 0;
}

static s32 LoadFileCall(void)
{
    if (LoadFileSetup() < 0) {
        return -1;
    }
    return sceSifCallRpc(&sLoadFileClient, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* A module's load and start, from a path or (path 0) a buffer at addr. */
static s32 LoadStartWork(const char *path, void *addr, s32 argsize, const char *args, s32 *result)
{
    s32 id, res = 0;
    if (LoadFileCall() < 0) {
        return -1;
    }
    id = KSif_LoadModule(path, (u32)addr, argsize, args, &res);
    if (result) {
        *result = res;
    }
    return id;
}


/* addr is an IOP address the game holds as a pointer (its module's place in the IOP heap): no EE pointer, so
 * the tap passes it as a word in both builds. */
s32 sceSifLoadStartModuleBuffer(void *addr, s32 argsize, const char *args, s32 *result)
{
    return RetPoll(LoadStartWork(0, addr, argsize, args, result), -1);
}

s32 sceSifLoadStartModule(const char *path, s32 argsize, const char *args, s32 *result)
{
    return RetPoll(LoadStartWork(path, 0, argsize, args, result), -1);
}

s32 sceSifLoadModule(char *path, s32 argc, char *argv)
{
    return RetPoll(LoadStartWork(path, 0, argc, argv, 0), -1);
}

static s32 SearchModuleWork(char *name)
{
    if (LoadFileCall() < 0) {
        return -1;
    }
    return KSif_SearchModule(name);
}

s32 sceSifSearchModuleByName(char *name)
{
    return SearchModuleWork(name);
}

static s32 StopModuleWork(s32 id, s32 *result)
{
    if (LoadFileCall() < 0) {
        return -1;
    }
    if (result) {
        *result = 0;
    }
    return id;
}

s32 sceSifStopModule(s32 id, s32 argc, char *argv, s32 *result)
{
    (void)argv;
    return RetPoll(StopModuleWork(id, result), -1);
}

static s32 UnloadModuleWork(s32 id)
{
    if (LoadFileCall() < 0) {
        return -1;
    }
    return id;
}

s32 sceSifUnloadModule(s32 id)
{
    return RetPoll(UnloadModuleWork(id), -1);
}

/* ------------------------------------------------------- SIF DMA */

typedef struct SifDmaRecord {
    u32 src, dest, size, attr;
} SifDmaRecord;

/* SifSetDma: each record copied to IOP memory at the call (size rounded up to 16); a transfer id from a
 * counter that never gives 0. */
static u32 SetDmaWork(void *transfers, s32 n)
{
    const SifDmaRecord *r = (const SifDmaRecord *)transfers;
    u32 id;
    s32 i;
    for (i = 0; i < n; i++) {
        KSif_Sif1Write(r[i].dest, (const void *)r[i].src, (r[i].size + 15u) & ~15u);
    }
    id = KSif_DmaId();
    return id;
}

u32 SifSetDma(void *transfers, s32 n)
{
    return SetDmaWork(transfers, n);
}

/* SifDmaStat: every transfer is done (-1). A `ret` record whose "not ready" is v0 >= 0: never. */
s32 SifDmaStat(u32 id)
{
    return -1;
}
