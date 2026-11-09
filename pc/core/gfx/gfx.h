#ifndef MH2_GFX_H
#define MH2_GFX_H

/*
 * The graphics front: the EE's
 * hardware registers (include/pal.h's Pal_EeReg* and Pal_GsReg*), the DMAC,
 * VIF0 and VIF1, VU1, the GIF and what the GS receives. The library functions
 * over them (libgraph, libdma, libvu0) are in pc/core/api/.
 *
 * This is the graphics front's interface for the other modules. The semantics are the
 * original game's, cited where a
 * rule is not obvious. Everything runs at the instant of the request
 *: a DMA started by a store to its CHCR is done
 * by the store's return, except a GIF transfer started while PATH3 is masked,
 * which runs when the mask is lifted.
 */
#include "common.h"

/* ---- for the kernel ---- */

/* The reset state of every unit, before the game runs (the original's hw_init). */
void Gfx_Init(void);
/* VBlank start:
 * GS CSR's FIELD flips, VSINT is set, and the GS cause is raised unless IMR
 * masks VSINT (the original's hw_vblank(1)). */
void Gfx_VBlankStart(u64 vblank);
/* VBlank end: the frame's GS stream is complete; it goes to the host. */
void Gfx_VBlankEnd(u64 vblank);
/* The kernel library's GS calls: SetGsCrt (syscall 2) programs SMODE1 and
 * SMODE2, which go into the GS stream (the original's hw_set_gs_crt); GsPutIMR (0x71)
 * and GsGetIMR (0x70) set and read IMR. */
void Gfx_SetGsCrt(u32 interlace, u32 mode, u32 ffmd);
void Gfx_GsPutImr(u32 imr);
u32 Gfx_GsGetImr(void);
/* What SetVSyncFlag's CSR slot gets at VBlank start: the CSR as a read of it
 * gives it then (the revision, the ID, FIFO empty, FIELD, the stored low 5
 * bits), as a PS2's kernel stores the register.
 * FIELD has toggled at the VBlank's step 8, before. */
u64 Gfx_GsCsrForVSyncFlag(void);
/* SetVSyncFlag (syscall 0x73) for the kernel library's VSync wait under
 * sceGsSyncV (libgraph.c): it hands the wait's slots to the kernel
 * (Kern_SetVSyncFlag), whose VBlank step 10 then stores
 * Gfx_GsCsrForVSyncFlag() in the CSR slot, which Gfx_VSyncSlot reads. */
void Gfx_SetVSyncFlag(void);   /* the flag cleared first (the wait's own word) */
u32 Gfx_VSyncFlag(void);       /* the flag: 1 once a VBlank start has written it */
u64 Gfx_VSyncSlot(void);
/* The kernel calls Gfx_Init at its start, Gfx_VBlankStart at the VBlank's
 * step 8 and Gfx_VBlankEnd at its end (kernel/start.c, vblank.c).
 * Kern_InitGfx (kern.h's start-up hook, defined in gfx_gs.c) sets the VU1
 * check and resets the slots. */
/* The DMAC's interrupt state is D_STAT (0x1000E010), through Pal_EeRegRead32
 * and Pal_EeRegWrite32 with the hardware's rules: CIS in bits 0-9 (a write of
 * 1 clears), CIM in bits 16-25 (a write of 1 toggles). A pending channel is
 * (stat & 0x3FF) & ((stat >> 16) & 0x3FF). */

/* The graphics front calls the kernel's interface (pc/core/kernel/kern.h):
 * Kern_RaiseIntc, Kern_RaiseDmac, Kern_IntcRead/Write, Kern_DmacStatRead/Write,
 * Kern_TimerRead/Write, Kern_LibWaitArrive/Step, Kern_LibDIntr/EIntr and
 * Kern_SetVSyncFlag. */

/* ---- what the GS receives: for the host ---- */

/* The records: GIF data
 * (arg = the path, 1-3; whole GIFtags, in the pieces the original game hands its
 * GS), a privileged register write (arg = the address; the 8-byte value), and
 * VBlank end (arg = the VBlank; data = the field, 4 bytes). Every record goes
 * to the host (Ipc_GsGif, Ipc_GsPrivWrite, Ipc_GsVSync;
 * headless, nowhere). */
enum { GFX_GS_GIF = 1, GFX_GS_PRIV = 2, GFX_GS_VBLANK = 3 };
typedef void (*GfxGsSink)(u32 kind, u32 arg, const void *data, u32 bytes);
/* Tests: every record to SINK instead of the host (NULL: the host again). */
void Gfx_SetGsSink(GfxGsSink sink);
/* GS-to-EE data (VIF1's FIFO in the GS-to-EE direction): the host's answer
 * (Ipc_GsReadback), which the game sees; zeros when headless.
 * In play the game gets the host's pixels. Tests: FN answers instead
 * (NULL: the host again). */
typedef void (*GfxReadback)(void *dst, u32 qwords);
void Gfx_SetReadback(GfxReadback fn);
/* The PS2 address of a pointer into the DMA channels' registers (what
 * sceDmaGetChan returns, which the game keeps in gSysWork.dmaChan), or 0. */
u32 Gfx_DmaChanPs2Addr(u32 addr);

/* ---- checks ---- */

/* The compiled microprograms against the interpreter (the original's
 * --vu1-check; the core: MH2_VU1_CHECK=1, read by Kern_InitGfx): every VU1 run
 * is made on both from the same state and compared (gfx_vu1.c). The counts:
 * runs compared, and (*differ) those that differed, each also an anomaly. */
void Gfx_Vu1Check(int on);
u64 Gfx_Vu1CheckCounts(u64 *differ);

/* The anomalies the original game logs once (a VIFcode, DMA mode or register the
 * game is not known to use): their count, and the first one's tag and key. */
u32 Gfx_Oddities(const char **firstTag, u32 *firstKey);

#endif
