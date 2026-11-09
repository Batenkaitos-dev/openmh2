/*
 * The SIF and the IOP's scheduling.
 *
 * The IOP is snd's (snd/snd.h): the kernel runs its catch-up at each delivery
 * point's step 2a while it is busy, and unparks it whenever an event fires.
 *
 * The SIF0 queue: what the IOP sends the EE as packets (RPC_END and the
 * like). One packet moves per delivery round (step 2b) when the EE's command
 * buffer is free and its address known; the SIF's DMAC handler (channel 5,
 * the RPC layer's, registered by sceSifInitRpc) then acts on it at step 2c.
 * Here a packet is a native action: what the PS2's library does with the
 * command (signal the client's semaphore, call its end function, fill in
 * the bound server).
 *
 * Also here: the registry of services, the RPC layer's packets and numbers,
 * the wait of an RPC answered at once, the SIF registers the reboot and the
 * sync use, and the list of loaded modules.
 */
#include "kernel/kern_int.h"
#include "kernel/ksif.h"
#include "snd/snd.h"
#include "kernel/klog.h"
#include "pal_core.h"

#define MAX_SIF0 127
#define MAX_SERVICES 64

static KSifPacket sQueue[MAX_SIF0 + 1];
static int sQueueHead, sQueueCount;
static KSifPacket sInBuffer;       /* the packet in the EE's command buffer, until the handler takes it */
static int sBufferBusy;
static int sBufferKnown;           /* the EE command buffer's address is known (INIT_CMD / CHANGE_SADDR) */
static int sRpcUp;                 /* sceSifInitRpc ran since the last reboot */

static void ResetAll(void);
static void CmdReset(void);

/* The kernel's SIF DMA transfer id (SifSetDma's result): one per SifSetDma syscall, the SIF library's own
 * commands included; from 1, never 0, kept across the IOP's reboots (the original's sif.c g_dma_id). */
static u32 sDmaId = 1;

u32 KSif_DmaId(void)
{
    u32 id = sDmaId++;
    if (!sDmaId) {
        sDmaId = 1;
    }
    return id;
}

void KSif_Reset(void)
{
    sDmaId = 1;
    sQueueHead = sQueueCount = 0;
    sBufferBusy = 0;
    sBufferKnown = 0;
    sRpcUp = 0;
    CmdReset();
    ResetAll();
}

int KSif_RpcUp(void)
{
    return sRpcUp;
}

void KSif_SetRpcUp(int up)
{
    sRpcUp = up;
}

/* ------------------------------------------------------------------ IOP */

void KSif_UnparkOnEvent(void)
{
    IopKern_Unpark();
}

int KSif_IopBusy(void)
{
    return IopKern_Busy();
}

/* 4, step 2a: 0 nothing ran, 1 it ran and is idle again, 2 its budget was used (snd parks it). */
int KSif_IopCatchUp(void)
{
    if (!IopKern_Busy()) {
        return 0;
    }
    return IopKern_CatchUp();
}

void KSif_IopVblank(int start)
{
    IopKern_Vblank(start);
}

/* ----------------------------------------------------------- SIF0 queue */

void KSif_SetBufferKnown(int known)
{
    sBufferKnown = known;
}

int KSif_BufferKnown(void)
{
    return sBufferKnown;
}

void KSif_Queue(const KSifPacket *p)
{
    if (sQueueCount >= MAX_SIF0) {
        Pal_Fatal("kernel: more than 127 SIF0 packets queued");
    }
    sQueue[(sQueueHead + sQueueCount) % (MAX_SIF0 + 1)] = *p;
    sQueueCount++;
}

void KSif_DropQueue(void)
{
    sQueueHead = sQueueCount = 0;
    sBufferBusy = 0;
}

/* 3, item 3. */
int KSif_HasWork(void)
{
    return sQueueCount > 0 && sBufferKnown && !sBufferBusy;
}

/* 4, step 2b: the first packet into the command buffer, and the DMAC's SIF0 channel posted. */
int KSif_Pump(void)
{
    if (!KSif_HasWork()) {
        return 0;
    }
    sInBuffer = sQueue[sQueueHead];
    sQueueHead = (sQueueHead + 1) % (MAX_SIF0 + 1);
    sQueueCount--;
    sBufferBusy = 1;
    Kern_RaiseDmac(5);
    return 1;
}

/* The SIF's DMAC handler (channel 5): the buffer is free again, and the packet's command acts. */
s32 KSif_DmacHandler(s32 cause, void *arg)
{
    KSifPacket p;
    (void)cause;
    (void)arg;
    if (!sBufferBusy) {
        return 0;
    }
    p = sInBuffer;
    sBufferBusy = 0;
    if (p.fn) {
        p.fn(&p);
    }
    return 0;
}

/* ------------------------------------------------- IOP-to-EE commands */

#define MAX_CMD_HANDLERS 64
#define MAX_DEFERRED 16

static struct {
    u32 cid;
    KernSifCmdFn fn;
    void *data;
} sCmd[MAX_CMD_HANDLERS];
static int sNumCmd;
static int sInServiceCall;
static KSifPacket sDeferred[MAX_DEFERRED];
static int sNumDeferred;

static void CmdReset(void)
{
    sNumCmd = 0;
    sInServiceCall = 0;
    sNumDeferred = 0;
}

/* A handler set for a command id replaces the one before it; a null one removes it. */
void KSif_SetCmdHandler(u32 cid, KernSifCmdFn fn, void *data)
{
    int i;
    for (i = 0; i < sNumCmd; i++) {
        if (sCmd[i].cid == cid) {
            break;
        }
    }
    if (i == sNumCmd) {
        if (!fn) {
            return;
        }
        if (sNumCmd >= MAX_CMD_HANDLERS) {
            Pal_Fatal("kernel: too many SIF command handlers");
        }
        sNumCmd++;
    }
    sCmd[i].cid = cid;
    sCmd[i].fn = fn;
    sCmd[i].data = data;
}

/* The EE's command buffer: the packet as the SIF0 DMA leaves it there, the SDK's 16-byte header (byte 0 the
 * packet's size, word 2 the command id, word 3 the option, 0) and then the payload; the handler gets its
 * address. Byte 0 is cleared once the handler has run (the buffer free again). */
static u32 sCmdBuffer[32] __attribute__((aligned(16)));

static void CmdAction(const KSifPacket *p)
{
    u8 *q = (u8 *)sCmdBuffer;
    u32 n = (16 + p->a + 15) & ~15u;
    int i;
    __builtin_memset(q, 0, n);
    q[0] = (u8)(16 + p->a);
    sCmdBuffer[2] = p->b;
    __builtin_memcpy(q + 16, p->data, p->a);
    for (i = 0; i < sNumCmd; i++) {
        if (sCmd[i].cid == p->b) {
            if (sCmd[i].fn) {
                sCmd[i].fn(q, sCmd[i].data);
            }
            break;
        }
    }
    q[0] = 0;
}

void Kern_SifSendCmd(u32 cid, const void *payload, u32 size)
{
    KSifPacket p;
    if (size > sizeof(p.data)) {
        Pal_Fatal("kernel: a SIF command's payload of more than 112 bytes");
    }
    __builtin_memset(&p, 0, sizeof(p));
    p.fn = CmdAction;
    p.a = size;
    p.b = cid;
    __builtin_memcpy(p.data, payload, size);
    if (sInServiceCall) {
        if (sNumDeferred >= MAX_DEFERRED) {
            Pal_Fatal("kernel: more than 16 SIF commands from one service call");
        }
        sDeferred[sNumDeferred++] = p;
        return;
    }
    KSif_Queue(&p);
}

void KSif_BeginServiceCall(void)
{
    sInServiceCall = 1;
}

void KSif_EndServiceCall(void)
{
    int i;
    sInServiceCall = 0;
    for (i = 0; i < sNumDeferred; i++) {
        KSif_Queue(&sDeferred[i]);
    }
    sNumDeferred = 0;
}

/* ------------------------------------------------------------- services */

/*
 * The services in the original's order of registration (its disc, file, pad and
 * card services, with the sound modules on the interpreter): the n-th gets the server record 0x18000 + n x 0x40
 * and the buffer 0x600000 + n x 0x10000 that a bind answers with. A service an IOP module registers, or one
 * bound without being known, joins the list at its end (an unknown one answers its calls with zeros).
 */
static const u32 sKnownIds[] = {
    0x80000592u, 0x80000593u, 0x80000595u, 0x80000597u, 0x8000059Au, 0x8000059Cu,   /* cdvd */
    0x80000001u, 0x80000003u, 0x80000006u, 0x80000211u,                              /* fileio, iopheap, loadfile, usb */
    0x01270030u, 0x80001101u,                                                        /* AVE-TCP wrapper, netcnfif */
    0x80001300u, 0x8000131Cu, 0x8000131Eu, 0x8000131Fu,                              /* dbcman */
    0x80000400u,                                                                     /* mcserv */
};

typedef struct KService {
    u32 id;
    const KernSifService *svc;
    u32 server, buff;
} KService;

static KService sSvc[MAX_SERVICES];
static int sNumSvc;

static void ServicesReset(void)
{
    unsigned i;
    const KernSifService *keep[sizeof(sKnownIds) / sizeof(sKnownIds[0])];
    for (i = 0; i < sizeof(sKnownIds) / sizeof(sKnownIds[0]); i++) {
        keep[i] = (int)i < sNumSvc ? sSvc[i].svc : 0;
    }
    sNumSvc = 0;
    for (i = 0; i < sizeof(sKnownIds) / sizeof(sKnownIds[0]); i++) {
        sSvc[sNumSvc].id = sKnownIds[i];
        sSvc[sNumSvc].svc = keep[i];
        sSvc[sNumSvc].server = 0x18000u + (u32)sNumSvc * 0x40u;
        sSvc[sNumSvc].buff = 0x600000u + (u32)sNumSvc * 0x10000u;
        sNumSvc++;
    }
}

static KService *SvcById(u32 id)
{
    int i;
    if (!sNumSvc) {
        ServicesReset();
    }
    for (i = 0; i < sNumSvc; i++) {
        if (sSvc[i].id == id) {
            return &sSvc[i];
        }
    }
    return 0;
}

static KService *SvcAppend(u32 id)
{
    KService *k;
    if (sNumSvc >= MAX_SERVICES) {
        Pal_Fatal("kernel: too many SIF services");
    }
    k = &sSvc[sNumSvc];
    k->id = id;
    k->svc = 0;
    k->server = 0x18000u + (u32)sNumSvc * 0x40u;
    k->buff = 0x600000u + (u32)sNumSvc * 0x10000u;
    sNumSvc++;
    return k;
}

void Kern_SifRegister(const KernSifService *service)
{
    KService *k = SvcById(service->id);
    if (!k) {
        k = SvcAppend(service->id);
    }
    k->svc = service;
    if (service->server) {
        k->server = service->server;
        k->buff = service->buff;
    }
}

void KSif_ServiceAddresses(u32 id, u32 *server, u32 *buff)
{
    KService *k = SvcById(id);
    if (!k) {
        k = SvcAppend(id);
        KLog("kernel: bind to unknown service 0x%08X: answering with a stub", id);
    }
    *server = k->server;
    *buff = k->buff;
    if (g_kern_log) {
        KLog("kernel: bind 0x%08X at %llu", id, Kern_Now());
    }
}

const KernSifService *KSif_ServiceByServer(u32 server)
{
    int i;
    for (i = 0; i < sNumSvc; i++) {
        if (sSvc[i].server == server) {
            return sSvc[i].svc;
        }
    }
    return 0;
}

int KSif_ServerKnown(u32 server)
{
    int i;
    for (i = 0; i < sNumSvc; i++) {
        if (sSvc[i].server == server) {
            return 1;
        }
    }
    return 0;
}

const KernSifService *KSif_FindService(u32 id)
{
    KService *k = SvcById(id);
    return k ? k->svc : 0;
}

/* ------------------------------------------------- packets and numbers */

/*
 * The RPC layer's 32 packets and its RPC number: the first
 * free packet; it gets the counter plus one, and the counter goes on from there, except that a 1 makes it 2.
 * The counter's value after sceSifInitRpc is provisional (1); the CSE clients' numbers in the original game
 * (0x29-0x2B on the boot route) check it once the other modules' clients make their RPCs. The packets'
 * addresses are provisional too (0x1CB740 + n x 0x40, the allocator's argument): a waited call leaves 0 in
 * the client, so only a call in flight shows one.
 */
#define NUM_PACKETS 32
#define PACKET_BASE 0x001CB740u
static u32 sPacketNum[NUM_PACKETS];
static u8 sPacketBusy[NUM_PACKETS];
static u32 sRpcCounter;

void KSif_RpcReset(void)
{
    __builtin_memset(sPacketBusy, 0, sizeof(sPacketBusy));
    sRpcCounter = 1;
}

int KSif_PacketAlloc(u32 *addr, u32 *num)
{
    int i;
    for (i = 0; i < NUM_PACKETS; i++) {
        if (!sPacketBusy[i]) {
            u32 n = sRpcCounter + 1;
            sRpcCounter = n == 1 ? 2 : n;
            sPacketBusy[i] = 1;
            sPacketNum[i] = n;
            *addr = PACKET_BASE + (u32)i * 0x40u;
            *num = n;
            return 1;
        }
    }
    return 0;
}

static int PacketIndex(u32 addr)
{
    u32 i = (addr - PACKET_BASE) / 0x40u;
    return addr >= PACKET_BASE && i < NUM_PACKETS ? (int)i : -1;
}

void KSif_PacketFree(u32 addr)
{
    int i = PacketIndex(addr);
    if (i >= 0) {
        sPacketBusy[i] = 0;
    }
}

int KSif_PacketBusy(u32 addr, u32 num)
{
    int i = PacketIndex(addr);
    return i >= 0 && sPacketBusy[i] && sPacketNum[i] == num;
}

/* --------------------------------------------------- the answered wait */

static void SignalAction(const KSifPacket *p)
{
    Kern_SemaSignal((s32)p->a);
    KSif_PacketFree(p->b);
}

s32 Kern_RpcWait(void)
{
    KSifPacket p;
    s32 sema;
    u32 addr, num;
    if (!sRpcUp) {
        return -1;
    }
    if (!KSif_PacketAlloc(&addr, &num)) {
        return -1;
    }
    sema = Kern_SemaCreate(0, 1, 0, 0);
    if (sema < 0) {
        KSif_PacketFree(addr);
        return -3;
    }
    __builtin_memset(&p, 0, sizeof(p));
    p.fn = SignalAction;
    p.a = (u32)sema;
    p.b = addr;
    KSif_Queue(&p);
    Kern_SemaWait(sema);
    Kern_SemaDelete(sema);
    return 0;
}

/* -------------------------------------------- registers and the reboot */

#define SMFLAG_BOOT 0x70000u      /* SIFINIT | CMDINIT | BOOTEND */
static u32 sSmflag;
static u32 sMainAddr, sSubAddr, sMsflag;
#define IOP_CMD_BUF 0x0001F000u   /* SUBADDR once the IOP is up: where the EE sends commands (the original's) */
static u32 sSreg[32];

u32 KSif_Sreg(u32 n)
{
    return sSreg[n & 31];
}

void KSif_SetSreg(u32 n, u32 v)
{
    sSreg[n & 31] = v;
}

int KSif_BootEnd(void)
{
    return (sSmflag & 0x40000u) != 0;
}

static void BootDone(u64 now, u32 param)
{
    (void)now;
    (void)param;
    sSmflag |= SMFLAG_BOOT;
    sSubAddr = IOP_CMD_BUF;
}

/* The RESET request (the specification, 7.1): fatal after a module load; else the SIF0 queue dropped, the EE
 * command buffer forgotten, the IOP reset (its heap), the RPC layer's software registers cleared, BOOTEND
 * cleared (the library's SifSetReg(4, 0x40000)), and the IOP up 2 ms later. */
void KSif_Reboot(const char *img)
{
    if (IopKern_ModuleLoaded()) {
        KFatal("kernel: IOP reboot (%s) after an IOP module load", img);
    }
    sSmflag &= ~0x40000u;
    KSif_DropQueue();
    sBufferKnown = 0;
    IopKern_Reset();
    sSreg[0] = sSreg[1] = sSreg[2] = sSreg[3] = 0;
    Kern_EventAdd(Kern_Now() + 2000000ull, BootDone, 0);
}

/* ------------------------------------------------------------ modules */

#define MAX_MODULES 64
static char sModules[MAX_MODULES][32];
static int sNumModules;

static int StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void Copy(char *dst, const char *src, int cap)
{
    int i;
    for (i = 0; i < cap - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static int ModuleAdd(const char *name)
{
    int i;
    for (i = 0; i < sNumModules; i++) {
        if (StrEq(sModules[i], name)) {
            return i + 1;
        }
    }
    if (sNumModules == MAX_MODULES) {
        return -1;
    }
    Copy(sModules[sNumModules], name, 32);
    return ++sNumModules;
}

/* loadfile's loads (the original's loadfile service): a module from a path is only recorded (its start
 * result 0, RESIDENT_END); one from IOP memory is named by its IRX header, and the sound modules run on
 * snd's IOP (their start-up inside the call). The id is the module's place in the list of names. */
s32 KSif_LoadModule(const char *path, u32 iopAddr, s32 argsize, const char *args, s32 *result)
{
    char name[64];
    int res = 0;
    if (path) {
        Copy(name, path, sizeof(name));
    } else {
        u32 a = iopAddr & (IOP_RAM_SIZE - 1);
        u32 max = IOP_RAM_SIZE - a;
        IopKern_IrxName(iopAddr, max, name, sizeof(name));
        if (IopKern_HasNativeModule(name)) {
            u32 len = argsize < 0 ? 0 : (u32)argsize;
            if (len > 252) {
                len = 252;
            }
            if (IopKern_LoadModule(a, max, name, (const u8 *)args, len, &res) < 0) {
                KLog("kernel: %s: loading on the IOP interpreter failed", name);
            }
        }
    }
    *result = res;
    return ModuleAdd(name);
}

s32 KSif_SearchModule(const char *name)
{
    int i, found = -200;
    for (i = 0; i < sNumModules; i++) {
        if (StrEq(sModules[i], name)) {
            found = i + 1;
        }
    }
    return found;
}

void KSif_Sif1Write(u32 dest, const void *src, u32 size)
{
    IopMem_Write(dest, src, size);
}

static void ResetAll(void)
{
    ServicesReset();
    KSif_RpcReset();
    sSmflag = SMFLAG_BOOT;
    sSubAddr = IOP_CMD_BUF;
    sMainAddr = sMsflag = 0;
    __builtin_memset(sSreg, 0, sizeof(sSreg));
    sNumModules = 0;
}

/* The SIF's registers as the EE sees them (as the original has them): MAINADDR 0x1000F200, SUBADDR
 * 0x1000F210, MSFLAG 0x1000F220 (a write sets bits), SMFLAG 0x1000F230 (a write clears bits). */
u32 KSif_RegRead(u32 addr)
{
    switch (addr) {
    case 0x1000F200u:
        return sMainAddr;
    case 0x1000F210u:
        return sSubAddr;
    case 0x1000F220u:
        return sMsflag;
    case 0x1000F230u:
        return sSmflag;
    }
    return 0;
}

void KSif_RegWrite(u32 addr, u32 v)
{
    switch (addr) {
    case 0x1000F200u:
        sMainAddr = v;
        break;
    case 0x1000F210u:
        sSubAddr = v;
        break;
    case 0x1000F220u:
        sMsflag |= v;
        break;
    case 0x1000F230u:
        sSmflag &= ~v;
        break;
    }
}
