#ifndef MH2_SND_SND_H
#define MH2_SND_SND_H

/*
 * The sound module's interface.
 *
 * mh2-core's sound is the original's (iop_lle.c, iop_hw.c, spu2.c,
 * adapted): the game's IOP sound modules (Sony's libsd, the Capcom Sound
 * Engine and CRI's ADX driver, from the archive's MH2DRIVE.DAT) are native
 * code (pc/core/api/libsd.c, pc/core/cse) against an IOP kernel written
 * here (iop_kern.c, iop_mod.c), and the SPU2 they drive is mixed here. The
 * original's R3000A interpreter, which could run them, is not in the
 * core. Timing is the frame clock's (kernel/kern.h).
 *
 * Two directions:
 * - this file: what the other modules call (the kernel's SIF and its delivery
 *   points, the VBlank);
 * - snd/snd_kernel.h: what the sound module needs from them.
 *
 * Every function here uses the game's calling convention (the platform
 * layer's default) and none of them touches game memory: EE memory is only
 * reached through snd_kernel.h.
 */

#include "snd/snd_types.h"

/* ---- IOP memory (the HLE IOP's address space, as the original's) ----
 * IOP addresses are the IOP's own (0x00000000 - 0x007FFFFF), not host
 * pointers; kseg mirrors fold (& 0x1FFFFFFF). The heap is the original's
 *: first fit in 256-byte units over 0x100000 - 0x600000;
 * module images, thread stacks and the game's sceSifAllocIopHeap blocks come
 * from it, so the addresses the game stores are the original's. */
#define IOP_RAM_SIZE 0x00800000u
#define IOP_HEAP_BASE 0x00100000u
#define IOP_HEAP_END 0x00600000u

void IopKern_Reset(void);                 /* the IOP reboot (SIF_CMD_RESET): the heap is reset */
u32 IopKern_HeapAlloc(u32 size);          /* 0 when exhausted */
void IopKern_HeapFree(u32 addr);
/* Bytes in and out of IOP RAM (the SIF's transfers, the RPC buffers). An
 * access that leaves IOP RAM is cut at its end. */
void IopMem_Write(u32 iop_addr, const void *src, u32 size);
void IopMem_Read(u32 iop_addr, void *dst, u32 size);
u8 *IopMem_Ptr(u32 iop_addr);         /* the byte at iop_addr in IOP RAM (masked to it) */

/* ---- modules (loadfile's "load from a buffer", fno 6) ----
 * IopKern_HasNativeModule: the modules that have native code, by the name in
 * their IRX header: "Sound_Device_Library", "Capcom Sound Engine",
 * "CRI_ADX_Driver". IopKern_LoadModule takes the block the IRX image at
 * `image` (an IOP address; `max` bytes are readable) asks for from the IOP
 * heap, and runs the module's native start function at the call's instant
 * (spec 7): returns the module count (> 0) or -1, and the start result & 3 in
 * *start_result. The first load starts the IOP's hardware and the SPU2 (and
 * the SPU2's tick, spec 10). */
int IopKern_HasNativeModule(const char *name);
int IopKern_LoadModule(u32 image, u32 max, const char *name, const u8 *args, u32 arglen, int *start_result);
int IopKern_ModuleLoaded(void);           /* a module has been loaded since the start (a reboot then is fatal) */
/* The module name in an IRX image's header (its .iopmod section), or "" */
void IopKern_IrxName(u32 image, u32 max, char *out, u32 cap);

/* ---- the IOP's scheduling at the frame clock's delivery points (spec 7) ---- */
int IopKern_Busy(void);                   /* a thread is ready or an interrupt is pending, and the IOP is not parked */
int IopKern_CatchUp(void);                /* step 2a: 0 nothing ran, 1 ran and idle again, 2 budget used (parked) */
void IopKern_Unpark(void);                /* every event that fires; anything that reaches the IOP from outside */
void IopKern_Vblank(int start);           /* VBlank start (1) / end (0): IOP irq 0 / 11 and the vblank waiters */

/* ---- RPC to the modules' servers (spec 7.1) ----
 * A module's sceSifRegisterRpc calls SndSys_RpcRegister(sid, sd, buff).
 * The EE's call reaches the server with IopKern_RpcCall, once the kernel has
 * copied the request to the server's buffer (`buff`); the server thread runs
 * at the next catch-up and answers through SndSys_RpcDone. */
typedef struct IopRpcReq {
    u32 sd;                           /* IOP address of the server's SifRpcServerData */
    u32 fno, size;                    /* function number, request bytes */
    u32 ee_receive, recv_size;        /* the EE's receive buffer and its size */
    void *client;                     /* the kernel's own: given back in SndSys_RpcDone */
} IopRpcReq;
void IopKern_RpcCall(const IopRpcReq *req);

/* ---- the SPU2 ---- */
void Spu2_Advance(u64 now_ns);        /* render up to now: at each VBlank start */
/* The interval that ends now: the samples
 * rendered since the last call and their FNV-1a 64 hash;
 * advances to now first, then starts a new interval. */
void Spu2_TakeInterval(u64 now_ns, u64 *samples, u64 *hash);

/* CRI ADX's start-up for the PS2's output (api/adxt.c): the bind of
 * CRI_ADX_Driver's server 0x90000200 and its set-up calls, under ADX's lock.
 * Sofdec's mwPlyInitSfdFx (the host's) calls it where the original's
 * calls ADX's init: after its first three set-ups, before its SJs. */
void ADXT_Init(void);

/* CRI ADX's decoder (adx_dec.c; the format's public description, observed bit
 * for bit against the original's rings): the prediction coefficients of a
 * header's cutoff and rate, and one 18-byte frame of a channel into 32 samples
 * (h1, h2: the channel's last two samples, 0 at the stream's start). */
void AdxDec_Coefs(u32 cutoff, u32 rate, s32 *c1, s32 *c2);
void AdxDec_Frame(const u8 *frame, s16 *out, s32 c1, s32 c2, s32 *h1, s32 *h2);

#endif
