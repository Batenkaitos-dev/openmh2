/*
 * The sound module on the kernel's interface (pc/core/kernel/kern.h):
 * snd_kernel.h's SndSys_* in terms of Kern_*, the IOP modules' RPC servers
 * in the kernel's registry of services, and the module's start-up hook. The
 * kernel calls the other way through snd.h (IopKern_*, IopMem_*, Spu2_*: kern.h, "The
 * IOP").
 *
 * Not in the module's unit tests (snd/tests), which stand in for the kernel.
 */
#include "kernel/kern.h"
#include "snd/snd_iop.h"
#include "pal.h"
#include "data_c.h"

/* ------------------------------------------------------------ time and events */

u64 SndSys_TimeNs(void)
{
    return Kern_Now();
}

u32 SndSys_EventAdd(u64 when, SndEventFn fn, u32 param)
{
    return Kern_EventAdd(when, fn, param);
}

void SndSys_EventCancel(u32 id)
{
    Kern_EventCancel(id);
}

/* Nothing to do on the frame clock: each delivery point asks IopKern_Busy (the specification, 7). */
void SndSys_Post(void)
{
}

/* ------------------------------------------------------------ EE memory */

/* An EE address that crosses to the IOP is the PS2's: the IOP's DMAs to and from the EE take it to the host address that holds it, the data module's
 * DataC_TryAt (a resident block's at its PS2 offset, else the RAM block's). An address TryAt doesn't map
 * (a block's hole, an overlay's block, 32 MiB or more) stops the core with the address. */
static u8 *EePtr(u32 ee)
{
    u8 *p = (u8 *)DataC_TryAt(ee);
    if (!p) {
        static const char hex[] = "0123456789ABCDEF";
        char msg[64] = "an IOP DMA at EE address 0x00000000 that no memory holds";
        for (int i = 0; i < 8; i++) {
            msg[27 + i] = hex[(ee >> (28 - 4 * i)) & 0xF];
        }
        SndLog_Fatal("snd", msg);
    }
    return p;
}

/* The bytes of [ee, ee + size) that lie in one host run (DataC_TryAt contiguous). Every block's and hole's
 * boundary is 16-aligned, so the run is walked in 16-byte steps. */
static u32 EeRun(u32 ee, u32 size)
{
    u8 *p = EePtr(ee);
    u32 n = 16 - (ee & 15);
    while (n < size && (u8 *)DataC_TryAt(ee + n) == p + n) {
        n += 16;
    }
    return n < size ? n : size;
}

void SndSys_ToEe(u32 ee_addr, const void *src, u32 size)
{
    const u8 *s = src;
    if (!ee_addr) {
        return;
    }
    while (size) {
        u32 n = EeRun(ee_addr, size);
        snd_memcpy(EePtr(ee_addr), s, n);
        ee_addr += n;
        s += n;
        size -= n;
    }
}

void SndSys_FromEe(u32 ee_addr, void *dst, u32 size)
{
    u8 *d = dst;
    while (size) {
        u32 n = EeRun(ee_addr, size);
        snd_memcpy(d, EePtr(ee_addr), n);
        ee_addr += n;
        d += n;
        size -= n;
    }
}

/* ------------------------------------------------------------ the modules' RPC servers */

/*
 * A server a module registered (sceSifRegisterRpc) joins the kernel's registry with its own server and
 * buffer addresses, which a bind answers with. A call (the original's sif.c: the CALL command's SIF DMA,
 * then rpc_call): the send data goes to the server's buffer in whole quadwords, as the SIF's DMA moves it,
 * the request to the server's queue (IopKern_RpcCall), and the server's thread answers at a later IOP
 * catch-up (SndSys_RpcDone).
 */
#define MAX_SERVERS 32
static KernSifService sServers[MAX_SERVERS];
static int sNumServers;

static int ServerCall(KernSifCall *c)
{
    const KernSifService *s = 0;
    IopRpcReq req;
    int i;
    for (i = 0; i < sNumServers; i++) {
        if (sServers[i].id == c->service) {
            s = &sServers[i];
        }
    }
    if (!s) {
        SndLog_Fatal("snd", "a call to an IOP server that is not registered");
    }
    if (c->ssize > 0 && c->send) {
        IopMem_Write(s->buff, c->send, ((u32)c->ssize + 15u) & ~15u);
    }
    req.sd = s->server;
    req.fno = (u32)c->fno;
    req.size = (u32)c->ssize;
    req.ee_receive = (u32)c->recv;
    req.recv_size = (u32)c->rsize;
    req.client = (void *)c->token;
    IopKern_RpcCall(&req);
    return KERN_SIF_LATER;
}

void SndSys_RpcRegister(u32 sid, u32 sd, u32 buff)
{
    KernSifService *s = 0;
    int i;
    for (i = 0; i < sNumServers; i++) {
        if (sServers[i].id == sid) {
            s = &sServers[i];
        }
    }
    if (!s) {
        if (sNumServers == MAX_SERVERS) {
            SndLog_Fatal("snd", "more than 32 IOP RPC servers");
        }
        s = &sServers[sNumServers++];
    }
    s->id = sid;
    s->name = "IOP module";
    s->call = ServerCall;
    s->server = sd;
    s->buff = buff;
    Kern_SifRegister(s);
}

/* The server's thread answered: its reply (recv_size bytes at the IOP address `reply`) to the EE's
 * receive buffer at once, then the RPC_END packet (the original's sif_lle_rpc_done). */
void SndSys_RpcDone(const struct IopRpcReq *req, u32 reply)
{
    if (req->ee_receive && req->recv_size && reply) {
        u32 a = reply & (IOP_RAM_SIZE - 1);
        u32 n = req->recv_size;
        if (n > IOP_RAM_SIZE - a) {
            n = IOP_RAM_SIZE - a;
        }
        snd_memcpy((void *)req->ee_receive, IopMem_Ptr(a), n);
    }
    Kern_SifAnswer((u32)req->client);
}

/* ------------------------------------------------------------ the output */

/* The host's sound device takes the SPU2's blocks through the host's ipc (pc/core/ipc), which
 * defines this; without it the blocks go nowhere. */
__attribute__((weak)) void SndSys_OutputSamples(const s16 *lr, u32 frames)
{
    (void)lr;
    (void)frames;
}

/* ------------------------------------------------------------ start-up */

/* The VBlank start's step 3, the SPU2 rendering up to now, is the kernel's
 * direct call to Spu2_Advance (kernel/vblank.c). The start-up hook only notes the sound's setting:
 * the original's `--audio on|stub`. The core's modules are native
 * (cse) and it has no stub, so any other setting stops here. */
void Kern_InitSnd(void)
{
    if (!Libsd_Setting()) {   /* cse (pc/core/api/libsd.c) */
        SndLog_Fatal("snd", "the audio setting is not cse (the core has no sound stub)");
    }
}
