#ifndef MH2_CORE_IO_KERNEL_H
#define MH2_CORE_IO_KERNEL_H

/*
 * The io module on the kernel's interface (pc/core/kernel/kern.h;
 * io adapts to the kernel's names).
 *
 * The EE side of the libraries calls the kernel through the API the game calls
 * too (include/sif.h, libkernl.h, and the kernel's api/libkernl.c and
 * api/libkernl_sif.c), so that the waits, the semaphores' ids and the threads'
 * order are the original's: a library's RPC blocks its thread in
 * sceSifCallRpc until the RPC's end is delivered, and its own semaphores take ids as the SDK's do. DIntr and EIntr as
 * a library calls them are kern.h's Kern_LibDIntr and Kern_LibEIntr (never a
 * delivery point, 6.3).
 *
 * The IOP services of io sit on the kernel's registry through io_kernel.c
 * (Iop_RegisterService below, onto Kern_SifRegister and Kern_SifAnswer).
 */
#include "common.h"
#include "sif.h"
#include "libkernl.h"
#include "kernel/kern.h"

/* ---- the EE kernel calls the libraries make and the game doesn't declare
 * (the kernel's api/libkernl.c defines them) ---- */
s32 DeleteSema(s32 sema);
s32 PollSema(s32 sema);
s32 iPollSema(s32 sema);
s32 StartThread(s32 tid, void *arg);
/* The OSD's configuration word (the language and the rest; syscall 0x4B). */
void GetOsdConfigParam(void *param);
s32 SleepThread(void);

/* ---- the SIF's commands, EE side (libkernl's; the game never calls them) ----
 * A handler of an IOP-to-EE command, run where the SDK's SIF0 handler runs (the
 * SIF's DMAC handler at a delivery point, the frame clock's 7.1), so it may only
 * use the i- calls. As in the SDK, packet is the whole command packet: a 16-byte
 * header (the SDK's sceSifCmdHdr), then the payload Kern_SifSendCmd was given;
 * valid during the call. */
#define IO_SIF_CMD_HDR 16u
#define IO_SIF_CMD_MAX 112u      /* the payload's limit: a 128-byte packet */
typedef void (*SifCmdHandler)(void *packet, void *data);
s32 sceSifAddCmdHandler(u32 cid, SifCmdHandler handler, void *data);
s32 sceSifRemoveCmdHandler(u32 cid);
/* Asked of the kernel: an IOP-to-EE command (cid, payload of size bytes, at most
 * IO_SIF_CMD_MAX) queued on the SIF0 queue behind the packets already there;
 * when the SIF moves it, the SIF's DMAC handler calls the handler that
 * sceSifAddCmdHandler gave for cid (none: dropped). */
void Kern_SifSendCmd(u32 cid, const void *payload, u32 size);

/* ---- the IOP services (io_kernel.c, on the kernel's registry) ----
 * An RPC to a registered service (sceSifCallRpc on a client bound to sid) runs
 * the handler at the request, as the original's HLE services do (frame clock
 * 7.1): data is the request (size bytes), reply a zero-filled buffer of
 * recv_size bytes (the client's receive size, or 0 without a receive buffer),
 * copied to the client's receive buffer before the RPC's end is queued. */
typedef struct IopRpcCall {
    u32 sid, fno;
    void *data;
    u32 size;
    void *reply;
    u32 recv_size;
} IopRpcCall;
typedef void (*IopRpcHandler)(IopRpcCall *call);
void Iop_RegisterService(u32 sid, const char *name, IopRpcHandler handler);
/* An IOP-to-EE SIF command (cid, the payload: at most IO_SIF_CMD_MAX bytes),
 * queued after the packets queued before it; sent from a service's handler, it
 * goes after its RPC's end (the original's deferred commands). */
void Iop_SendCmd(u32 cid, const void *payload, u32 size);

/* ---- the calendar (frame clock 1.2, kern.h's Kern_CalendarSeconds) ----
 * The guest's date and time at UTC + utcOffsetSeconds: out = sec, min, hour,
 * day of the month (1-31), month (1-12), year (2006). The CD's ReadClock
 * answers it in the kernel's zone (Kern_CalendarOffset); the card service
 * stamps its changes in JST (+9 h) whatever the zone, as the original's. */
void Io_CalendarZone(s32 utcOffsetSeconds, u32 out[6]);
void Io_CalendarJst(u32 out[6]);

#endif
