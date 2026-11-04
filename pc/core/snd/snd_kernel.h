#ifndef MH2_SND_KERNEL_H
#define MH2_SND_KERNEL_H

/*
 * What the sound module needs from the others, mostly the kernel's frame
 * clock and SIF. The sound
 * module's files call only these, and the kernel's raw fibers (kern.h:
 * Kern_FiberCreate, Self, Switch, Delete) for the IOP's threads.
 * snd/snd_glue.c maps them onto the kernel's interface (pc/core/kernel/kern.h);
 * the module's tests (snd/tests) define their own, with the kernel's fiber.c.
 */

#include "snd/snd_types.h"
#include "kernel/kern.h"

/* Guest time in ns (the frame clock's; never a host clock). */
u64 SndSys_TimeNs(void);

/* An event (spec 2): `fn(now, param)` at guest time `when`; events of the
 * same time fire in the order they were queued. Returns an id (never 0). */
typedef void (*SndEventFn)(u64 now, u32 param);
u32 SndSys_EventAdd(u64 when, SndEventFn fn, u32 param);
void SndSys_EventCancel(u32 id);

/* Something reached the IOP (an interrupt, a ready thread): the original's
 * kernel_post_interrupts, so that the next delivery point runs the IOP. */
void SndSys_Post(void);

/* EE memory, at the instant of the call (spec 7.1, "Direct IOP-to-EE
 * copies"). `ee_addr` is an EE address as the game gave it (in the core, the
 * host address of game memory, below 0x10000000). */
void SndSys_ToEe(u32 ee_addr, const void *src, u32 size);
void SndSys_FromEe(u32 ee_addr, void *dst, u32 size);

/* RPC (spec 7.1): a module registered a server; a server answered the call
 * `req` (its reply is at IOP address `reply`, 0 for none): the kernel copies
 * req->recv_size bytes of it to req->ee_receive at once, and the RPC_END
 * reaches the EE as a SIF0 packet. */
struct IopRpcReq;
void SndSys_RpcRegister(u32 sid, u32 sd, u32 buff);
void SndSys_RpcDone(const struct IopRpcReq *req, u32 reply);

/* The SPU2's output in blocks of up to 512 frames (48 kHz stereo,
 * interleaved left, right), for the host's sound device (the host's
 * pc/core/ipc). */
void SndSys_OutputSamples(const s16 *lr, u32 frames);

#endif
