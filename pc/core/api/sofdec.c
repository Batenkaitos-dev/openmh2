/*
 * CRI Sofdec, the movie library, outside the four wrappers of
 * src/sub_main/movie.c. Under PLATFORM_PC
 * the wrappers play movies through Pal_Movie* (pc/core/ipc/ipc_movie.c), and
 * gMoviePlayer stays NULL, so the game reaches no other Sofdec entry point but
 * mwPlyInitSfdFx, which Movie_InitSfd calls once at boot.
 *
 * Written from the original's behaviour at the interface: the calls
 * mwPlyInitSfdFx makes, the calls the functions it leaves behind make,
 * and the RAM images it leaves. Nothing here comes from Sofdec's code.
 *
 * What the original game was seen doing:
 * - mwPlyInitSfdFx: CRI ADX's start-up (snd's ADXT_Init) after three set-ups
 *   that make no kernel call, then 13 sections under SVM's lock (each a lock
 *   and an unlock: SJ's three, ten in the handle pool's and servers' set-up),
 *   no kernel object of its own. It keeps none of the caller's words: no word
 *   of EE RAM or the scratchpad points into the caller's SfdInitPrm afterwards
 *   (VBlanks 3 to 10), and the display rate is copied into Sofdec's memory.
 * - Its servers, which SVM runs from then on, with no movie (the original's
 *   movies are its runtime's, so no Sofdec handle is ever made): one on the
 *   VSYNC thread after ADX's (one lock section each VBlank), one on the
 *   caller's thread from func_00162040 after ADX's (no kernel call), one on
 *   the IDLE thread (one lock section each time it runs, then one step of
 *   a library wait: IdleServer below).
 * - SVM's sleep hooks, around func_00162188's SleepThread (Sys_WaitFlip's
 *   wait): before it, the scratchpad's 16 KB go by DMA to a save area
 *   (fromSPR, normal) and Sofdec's own 16 KB come in (toSPR, normal); after
 *   it, the other way round. So while the game's main thread waits for the
 *   flip, the scratchpad holds Sofdec's (zeros at first), as the original's
 *   RAM images show (VBlank 200 of the boot: the scratchpad all zero, the
 *   game's 34 nonzero bytes in Sofdec's save area).
 * Sofdec's own memory (the copy of the parameters, the save areas) is the
 * library's (plan 4.3): the RAM images leave it out.
 */
#include "common.h"
#include "pal.h"
#include "io/adx.h"
#include "kernel/kern.h"
#include "snd/snd.h"

/* src/main/cri_setup.c's (Capcom's call site). */
typedef struct SfdInitPrm {
    f32 vhz;
    s32 dispCycle;
    s32 dispLatency;
    u8 decSvr;
    s32 rsv[4];
} SfdInitPrm;

void mwPlyInitSfdFx(SfdInitPrm *prm);
void Sfd_SleepBefore(void);
void Sfd_SleepAfter(void);

static s32 sInit;

/* ------------------------------------------------------------ the scratchpad */

#define SPR_QWC 0x400           /* the whole scratchpad: 16 KB */
#define D8 0x1000D000u          /* fromSPR's registers */
#define D9 0x1000D400u          /* toSPR's */
#define CHCR 0x00u
#define MADR 0x10u
#define QWC 0x20u
#define CHCR_STR 0x100u

static u8 sSprSave[SPR_QWC * 16] __attribute__((aligned(64)));  /* the game's, while it waits */
static u8 sSprOwn[SPR_QWC * 16] __attribute__((aligned(64)));   /* Sofdec's */

/* A normal transfer of the whole scratchpad on one of its channels, as libdma
 * starts one (api/libdma.c: MADR, CHCR read, QWC, then CHCR with STR, MOD
 * normal, DIR from memory for toSPR), without libdma's seam line: the
 * original's calls are the library's own, which the trace leaves out. The
 * core's DMAC runs it at once (gfx_dma.c), so the waits around it (sceDmaSync
 * in the original game, never found busy) have nothing to wait for. */
static void SprDma(u32 chan, void *buf)
{
    u32 c;
    Pal_EeRegWrite32(chan + MADR, (u32)buf);
    c = Pal_EeRegRead32(chan + CHCR);
    Pal_EeRegWrite32(chan + QWC, SPR_QWC);
    c = (c & ~0x0Du) | CHCR_STR | (chan == D9 ? 1u : 0u);
    Pal_EeRegWrite32(chan + CHCR, c);
}

/* Before func_00162188's sleep: the game's scratchpad out, Sofdec's in. */
void Sfd_SleepBefore(void)
{
    SprDma(D8, sSprSave);
    SprDma(D9, sSprOwn);
}

/* After it: Sofdec's out, the game's back. */
void Sfd_SleepAfter(void)
{
    SprDma(D8, sSprOwn);
    SprDma(D9, sSprSave);
}

/* ------------------------------------------------------------ the servers */

static void LockedStep(void)
{
    Adx_Lock();
    Adx_Unlock();
}

/* The VSYNC thread's: with no handle, one section under the lock. */
static void VsyncServer(void *arg)
{
    (void)arg;
    LockedStep();
}

/* func_00162040's (the caller's thread): with no handle, no kernel call. */
static void MainServer(void *arg)
{
    (void)arg;
}

/* The IDLE thread's (priority 25): with no handle, one section under the
 * lock, then one step of Sofdec's library wait (the polled-wait table's func_00151730:
 * one arrival and one step each time the server runs, 6,066 each over the
 * village route). The step is the jump to the next
 * event, and it holds back every thread of lower priority until then: the
 * PSP thread (priority 40) that psp-link's route starts gets its first alarm
 * at the SPU2 tick after the VBlank's start, 1.23 ms later, not at the
 * VBlank's start (6 DelayThread calls more in
 * VBlank 6200). */
static void IdleServer(void *arg)
{
    (void)arg;
    LockedStep();
    Kern_LibWaitArrive();
    Kern_LibWaitStep();
}

/* ------------------------------------------------------------ the start-up */

/* mwPlyInitSfdFx(prm): Sofdec's start-up for the display's rate. A second
 * call does nothing (never observed: Movie_InitSfd runs once). */
void mwPlyInitSfdFx(SfdInitPrm *prm)
{
    s32 i;
    if (sInit++ > 0) {
        return;
    }
    /* prm's words go into Sofdec's own memory there: no one here reads them */
    ADXT_Init();
    for (i = 0; i < 3; i++)         /* SJ's ring-buffer, memory and unified set-ups */
        LockedStep();
    for (i = 0; i < 6; i++)         /* the handle pool */
        LockedStep();
    LockedStep();
    /* the servers, each joining SVM under the lock */
    Adx_Lock();
    Adx_SvrAdd(ADX_SVR_VSYNC, VsyncServer, 0);
    Adx_Unlock();
    Adx_Lock();
    Adx_SvrAdd(ADX_SVR_MAIN, MainServer, 0);
    Adx_Unlock();
    Adx_Lock();
    Adx_SvrAdd(ADX_SVR_IDLE, IdleServer, 0);
    Adx_Unlock();
    /* SVM's sleep hooks, last and without a lock (the original's 0x1326F0):
     * func_00162188 (io's) calls them around the flip wait's sleep */
    Adx_SetSleepHooks(Sfd_SleepBefore, Sfd_SleepAfter);
}
