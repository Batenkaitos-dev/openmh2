/*
 * The VBlank: VBlank n starts at
 * n x 16,683,350 ns and ends 1,270,000 ns later. The start's steps run in the
 * specification's order, the other modules' work called directly (snd's SPU2,
 * io's pads, gfx's GS); the host's are hooks it sets. The end
 * of the run (--frames, the player's quit, a fatal error) is here too, with
 * the SPU2 brought up to now.
 */
#include "kernel/kern_int.h"
#include "kernel/khost.h"
#include "kernel/klog.h"
#include "pal_core.h"
#include "snd/snd.h"
#include "io/io_pad.h"
#include "gfx/gfx.h"
#include "ipc/core_ipc.h"

static KernVblankFn sHooks[KERN_VB_STEPS];
static u64 sVblankCount;
static u64 sMaxFrames KERN_HOSTBSS;     /* the run's end: the process's, not the state's */
static int sFinished;
static u64 sCrashAt;     /* MH2_KERNEL_CRASH=N: a fault on purpose at VBlank N (the crash path's test) */

/* SetVSyncFlag's words (the kernel's: what the original game writes to the stack slots its caller gave). */
static u32 *sVsyncFlag;
static u64 *sVsyncCsr;

void Kern_SetVblankHook(int step, KernVblankFn fn)
{
    if (step < 0 || step >= KERN_VB_STEPS) {
        Pal_Fatal("kernel: Kern_SetVblankHook: no such step");
    }
    sHooks[step] = fn;
}

u64 Kern_VblankCount(void)
{
    return sVblankCount;
}

void KVblank_SetStop(u64 frames)
{
    sMaxFrames = frames;
}

void Kern_SetVSyncFlag(u32 *flag, u64 *csr)
{
    sVsyncFlag = flag;
    sVsyncCsr = csr;
}

/* The run's end: the SPU2 to now, once (a fatal error inside them ends plainly). */
void KVblank_Finish(const char *reason)
{
    if (sFinished) {
        return;
    }
    sFinished = 1;
    Spu2_Advance(Kern_Now());
}

static void Hook(int step, u64 n)
{
    if (sHooks[step]) {
        sHooks[step](n);
    }
}

static void VblankEnd(u64 now, u32 param)
{
    (void)now;
    (void)param;
    Hook(KERN_VB_END, sVblankCount);
    Gfx_VBlankEnd(sVblankCount);
    Kern_RaiseIntc(KERN_INTC_VBLANK_E);
    KSif_IopVblank(0);
}

__attribute__((weak)) void KState_VBlankStart(u64 vblank)
{
    (void)vblank;
}

static void VblankStart(u64 now, u32 param)
{
    u64 n;
    (void)param;
    KState_VBlankStart(sVblankCount + 1);   /* a state saved or loaded here is the one before VBlank n */
    n = ++sVblankCount;                  /* 1 */
    Hook(KERN_VB_PACE, n);               /* 2 */
    Spu2_Advance(now);                   /* 3 */
    Hook(KERN_VB_INPUT, n);              /* 7 */
    Io_PadVBlank();
    Gfx_VBlankStart(n);                  /* 8 */
    KSif_IopVblank(1);                   /* 9 */
    if (sVsyncFlag) {                    /* 10: once, then the words are dropped */
        *sVsyncFlag = 1;
        *sVsyncCsr = Gfx_GsCsrForVSyncFlag();
        sVsyncFlag = 0;
        sVsyncCsr = 0;
    }
    Kern_RaiseIntc(KERN_INTC_VBLANK_S);  /* 11 */
    Kern_EventAdd((n + 1) * KERN_NS_PER_VBLANK, VblankStart, 0);   /* 12 */
    Kern_EventAdd(now + KERN_NS_VBLANK_END, VblankEnd, 0);
    if (sMaxFrames && n >= sMaxFrames) {   /* 13 */
        KLog("mh2-core: reached %llu frames, stopping", n);
        KVblank_Finish("frames");
        HostK_Exit(0);
    }
    if (Ipc_QuitRequested()) {
        KLog("mh2-core: the player quit at VBlank %llu", n);
        KVblank_Finish("quit");
        HostK_Exit(0);
    }
    if (sCrashAt && n == sCrashAt) {
        *(volatile u32 *)16 = 0;         /* start.c's KStart_OnCrash: "crash" at VBlank N, exit 70 */
    }
}

void KVblank_Reset(void)
{
    sVblankCount = 0;
    sVsyncFlag = 0;
    sVsyncCsr = 0;
    sFinished = 0;
    sCrashAt = 0;
    {
        const char *e = Host_Getenv("MH2_KERNEL_CRASH");
        while (e && *e >= '0' && *e <= '9') {
            sCrashAt = sCrashAt * 10 + (u64)(*e++ - '0');
        }
    }
    Kern_EventAdd(KERN_NS_PER_VBLANK, VblankStart, 0);
}

