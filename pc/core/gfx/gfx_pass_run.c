/*
 * The second pass on a thread of its own (MH2_PASS_THREAD=1 in a development build; with the ring in a release): this file holds gfx_pass.c again, under the names of
 * gfx_pass_prefix.h, with its own VIF1, VU1 and GIF (gfx_pass_vif.c, gfx_pass_vu1.c, gfx_pass_gif.c, gfx_pass_vu1gen.c), and the queue and the
 * thread that run it. The game's thread (the real gfx_pass.c) copies a window into a slot (gfx_pass_job.h), queues it and goes on; the pass thread
 * takes the slots in order, runs the pass and gives them back. The two threads share the slots, the queue's indices and nothing else: what the
 * game's thread has made of a slot is the pass thread's until it gives the slot back (the queue's indices are the hand-over, with their
 * acquire and release), and the real units are never touched by this side.
 *
 * What the copy's units call outside themselves is below: the GS (the pass's own stream), the anomaly counter (the copy's own), the DMA's resume
 * (nothing), the arena (the host's allocator), the table of the data blocks' pages (the real one, handed over before the thread starts).
 * A development check holds the list of names the copy's objects may reference.
 *
 * This file's data is the process's, not the game's (the build gives it hostsec.h): a save state does not hold it.
 */
#include "common.h"
#include "gfx_pass_prefix.h"
#include "gfx_pass.c"

#include "gfx_pass_cb.h"

/* ---- the queue ---- */

static PassJob *sSlots[PJ_SLOTS];
static int sSlotState[PJ_SLOTS];   /* 0 free, 1 the game's thread holds it (copying), 2 queued or running */
typedef struct FifoEnt {
    int slot;                      /* -1: a step's end */
    u64 vblank;
} FifoEnt;
static FifoEnt sFifo[PJ_FIFO];
static u32 sHead, sTail;           /* the pass thread advances sHead, the game's thread sTail */
static void *sSem, *sSemSleep;
static int sStop, sDone, sStarted, sWaitSlot, sInline;   /* sInline (MH2_PASS_INLINE=1, tests only): the jobs run on the game's thread, at once, on the copy's units */
static u64 sJobMaxNs, sJobMaxAt, sLeakAt;
static int sLeak;   /* MH2_PASS_LEAK=1 (tests only): the control of the isolation checks. Once a second the pass thread copies its VU memory into the real one (which stops the core); 2: 256 bytes after every 512th job */
static u64 sPushed, sDroppedWin, sStepsPushed, sStepsLost, sRan;

/* ---- the helpers (MH2_PASS_HELPERS=n): their threads, the package pool and the queue of packages, the ring's lock ---- */

int pp1_HelpInit(void);
void pp1_HelpRun(MomPkg *k);
void pp1_HelpFinish(void);
int pp2_HelpInit(void);
void pp2_HelpRun(MomPkg *k);
void pp2_HelpFinish(void);
int pp3_HelpInit(void);
void pp3_HelpRun(MomPkg *k);
void pp3_HelpFinish(void);
int pp4_HelpInit(void);
void pp4_HelpRun(MomPkg *k);
void pp4_HelpFinish(void);
int pp5_HelpInit(void);
void pp5_HelpRun(MomPkg *k);
void pp5_HelpFinish(void);
static int (*const sHInit[PJ_HELPERS])(void) = {pp1_HelpInit, pp2_HelpInit, pp3_HelpInit, pp4_HelpInit, pp5_HelpInit};
static void (*const sHRun[PJ_HELPERS])(MomPkg *) = {pp1_HelpRun, pp2_HelpRun, pp3_HelpRun, pp4_HelpRun, pp5_HelpRun};
static void (*const sHFinish[PJ_HELPERS])(void) = {pp1_HelpFinish, pp2_HelpFinish, pp3_HelpFinish, pp4_HelpFinish, pp5_HelpFinish};
static int sNHelp;                 /* helpers running (0: the pass is as it was) */
static MomPkg *sPFree[PJ_PKGS];    /* the packages not in use, a stack */
static u32 sPFreeN;
static MomPkg *sPQ[PJ_PKGS];       /* the packages queued for a helper, a ring */
static u32 sPQHead, sPQTail;
static void *sPLock, *sPSem, *sPFreeSem, *sRingLock;   /* the lock of the two lists, the count of queued packages, of free packages, the second ring's lock */
static int sHStop;
static u64 sRingPuts, sRingWaitNs, sRingHeldNs;   /* MH2_PASS_CENSUS: the second ring's lock (written with the lock held) */
static u32 sHBusy, sHExited;       /* the helpers' tasks in hand (counted with the queue's lock held when taken), the threads that have left */
static u64 sHSubmitted;
static int sModLeft;               /* MH2_PASS_HMODELS: the later moments' Models the helpers have yet to make */
static void *sModSem;
static u64 sModWaits, sModWaitNs;  /* the step's ends that found some left, and the time they waited */

static void LockOn(void *l)
{
    while (!Host_SemWait(l, 1000))
        ;
}

static void LockOff(void *l)
{
    Host_SemPost(l);
}

int PassHelp_On(void)
{
    return sNHelp > 0 && !sInline;
}

MomPkg *PassHelp_Get(void)
{
    MomPkg *k;
    while (!Host_SemWait(sPFreeSem, 100))
        ;
    LockOn(sPLock);
    k = sPFree[--sPFreeN];
    LockOff(sPLock);
    return k;
}

void PassHelp_Done(MomPkg *k)
{
    LockOn(sPLock);
    sPFree[sPFreeN++] = k;
    LockOff(sPLock);
    Host_SemPost(sPFreeSem);
}

void PassHelp_Submit(MomPkg *k)
{
    LockOn(sPLock);
    sPQ[sPQTail++ % PJ_PKGS] = k;
    sHSubmitted++;
    LockOff(sPLock);
    Host_SemPost(sPSem);
}

int PassHelp_RingPut(unsigned int arg0, unsigned int arg1, const void *data, unsigned int qwords)
{
    int r;
    if (sCensus) {   /* the wait for the lock and the time it is held (the record's copy into the ring) */
        u64 t0 = Ipc_TimeNs(), t1;
        LockOn(sRingLock);
        t1 = Ipc_TimeNs();
        r = Ipc_GsPassRing(arg0, arg1, data, qwords);
        sRingPuts++;
        sRingWaitNs += t1 - t0;
        sRingHeldNs += Ipc_TimeNs() - t1;
        LockOff(sRingLock);
        return r;
    }
    LockOn(sRingLock);
    r = Ipc_GsPassRing(arg0, arg1, data, qwords);
    LockOff(sRingLock);
    return r;
}

void PassHelp_ModelsAdd(u32 n)
{
    __atomic_add_fetch(&sModLeft, (int)n, __ATOMIC_ACQ_REL);
}

void PassHelp_ModelsDone(void)
{
    if (__atomic_sub_fetch(&sModLeft, 1, __ATOMIC_ACQ_REL) == 0)
        Host_SemPost(sModSem);
}

void PassHelp_ModelsWait(void)
{
    u64 t0;
    if (!sNHelp || __atomic_load_n(&sModLeft, __ATOMIC_ACQUIRE) <= 0)
        return;
    t0 = Ipc_TimeNs();
    while (__atomic_load_n(&sModLeft, __ATOMIC_ACQUIRE) > 0)
        Host_SemWait(sModSem, 1);
    sModWaits++;
    sModWaitNs += Ipc_TimeNs() - t0;
}

void PassHelp_MomPut(u32 row, u64 ns)
{
    u32 i;
    if (!sMV || row >= MV_N)
        return;
    i = __atomic_fetch_add(&sMVn[row], 1u, __ATOMIC_RELAXED);
    if (i < TM_MAX)
        sMV[row][i] = (u32)(ns / 1000ull);
}

void PassHelp_Release(PassJob *j)
{
    u32 i;
    for (i = 0; i < PJ_SLOTS && sSlots[i] != j; i++)
        ;
    __atomic_store_n(&sSlotState[i], 0, __ATOMIC_RELEASE);
}

static HOST void HelperMain(void *arg)
{
    u32 id = (u32)(size_t)arg;
    for (;;) {
        MomPkg *k = 0;
        if (!Host_SemWait(sPSem, 20)) {
            if (__atomic_load_n(&sHStop, __ATOMIC_ACQUIRE))
                break;
            continue;
        }
        LockOn(sPLock);
        if (sPQHead != sPQTail) {
            k = sPQ[sPQHead++ % PJ_PKGS];
            sHBusy++;
        }
        LockOff(sPLock);
        if (!k)
            continue;
        sHRun[id](k);
        LockOn(sPLock);
        sHBusy--;
        LockOff(sPLock);
    }
    __atomic_add_fetch(&sHExited, 1u, __ATOMIC_ACQ_REL);
}

static int HelpersStart(void)
{
    const char *e = PASS_ENV("MH2_PASS_HELPERS");
    /* a helper for each moment unless the variable says how many (0: none, the pass as it was) */
    u32 want = !e ? PJ_HELPERS : e[0] >= '0' && e[0] <= '9' ? (u32)(e[0] - '0') : 0, i;
    if (!want || sInline || sMode < 3)
        return 1;
    if (!sRec1) {   /* the moments of a window come to the host in any order, and only whole records can: a record a piece (MH2_PASS_REC1=0) cannot be shared */
        return 1;
    }
    if (want > PJ_HELPERS)
        want = PJ_HELPERS;
    sPLock = Host_SemNew();
    sPSem = Host_SemNew();
    sPFreeSem = Host_SemNew();
    sModSem = Host_SemNew();
    if (!sPLock || !sPSem || !sPFreeSem || !sModSem)
        return 0;
    Host_SemPost(sPLock);
    for (i = 0; i < PJ_PKGS; i++) {
        sPFree[sPFreeN++] = PkgNew();
        Host_SemPost(sPFreeSem);
    }
    for (i = 0; i < want; i++) {
        if (!sHInit[i]())
            return 0;
        if (!Host_ThreadStart(HelperMain, (void *)(size_t)i, 1))
            return 0;
        sNHelp++;
    }
    {
        char line[80];
        u32 n = 0;
        Str(line, &n, "mh2-core: pass helpers ");
        Num(line, &n, (u64)sNHelp);
        line[n] = 0;
        Host_Log(line);
    }
    return 1;
}

static void HelpersFinish(void)
{
    u32 i;
    if (!sNHelp)
        return;
    for (;;) {   /* drained: nothing queued, nothing in hand */
        int idle;
        LockOn(sPLock);
        idle = sPQHead == sPQTail && sHBusy == 0;
        LockOff(sPLock);
        if (idle)
            break;
        Host_SemWait(sSemSleep, 2);
    }
    __atomic_store_n(&sHStop, 1, __ATOMIC_RELEASE);
    for (i = 0; i < (u32)sNHelp; i++)
        Host_SemPost(sPSem);
    while (__atomic_load_n(&sHExited, __ATOMIC_ACQUIRE) < (u32)sNHelp)
        Host_SemWait(sSemSleep, 2);
    for (i = 0; i < MV_N; i++)   /* the helpers counted the moments' rows past the table's end, if they did: no more than the rows hold */
        if (sMVn[i] > TM_MAX)
            sMVn[i] = TM_MAX;
    for (i = 0; i < (u32)sNHelp; i++)
        sHFinish[i]();
    {
        char line[160];
        u32 n = 0;
        Str(line, &n, "mh2-core: pass helpers: moments queued ");
        Num(line, &n, sHSubmitted);
        if (sHMod) {   /* MH2_PASS_HMODELS: the step's ends that waited for a helper's Models */
            Str(line, &n, " step-end-waits ");
            Num(line, &n, sModWaits);
            Str(line, &n, " waited-us ");
            Num(line, &n, sModWaitNs / 1000ull);
        }
        line[n] = 0;
        Host_Log(line);
    }
}

static HOST void ThreadMain(void *arg)
{
    (void)arg;
    for (;;) {
        u32 h = sHead, t = __atomic_load_n(&sTail, __ATOMIC_ACQUIRE);
        if (h == t) {
            if (__atomic_load_n(&sStop, __ATOMIC_ACQUIRE))
                break;
            Host_SemWait(sSem, 20);
            continue;
        }
        FifoEnt e = sFifo[h % PJ_FIFO];
        if (e.slot >= 0) {
            u64 t0 = Ipc_TimeNs(), dt;
            int handed = RunJob(sSlots[e.slot]);
            sRan++;
            if (handed) {   /* the window's moments are the helpers': the slot comes back from them */
                dt = Ipc_TimeNs() - t0;
                if (dt > sJobMaxNs)
                    sJobMaxNs = dt, sJobMaxAt = sRan;
                __atomic_store_n(&sHead, h + 1, __ATOMIC_RELEASE);
                continue;
            }
            if (sLeak == 1 && t0 - sLeakAt > 1000000000ull) {
                sLeakAt = t0;
                memcpy(sSlots[e.slot]->realVu, g_vu_mem, sizeof(g_vu_mem));
            } else if (sLeak == 2 && (sRan & 511u) == 0) {   /* the gentler form: 256 bytes of the copy's VU memory after every 512th job, at a place that moves through the VU1 data memory; the real unit goes on */
                u32 off = 0xC000u + ((u32)(sRan * 256u) & 0x3FFFu);   /* VU1 data memory */
                memcpy((u8 *)sSlots[e.slot]->realVu + off, (u8 *)g_vu_mem + off, 256);
            }
            dt = Ipc_TimeNs() - t0;
            if (dt > sJobMaxNs)
                sJobMaxNs = dt, sJobMaxAt = sRan;
            __atomic_store_n(&sSlotState[e.slot], 0, __ATOMIC_RELEASE);
        } else
            RunStep(e.vblank);
        __atomic_store_n(&sHead, h + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&sDone, 1, __ATOMIC_RELEASE);
}

int PassJob_Start(void)
{
    u32 i;
    if (sStarted)
        return 1;
    Init();   /* this copy's: the same switches, its own buffers, sets and counters */
    if (sMode <= 0)
        return 0;
    vif_init();                            /* the copy's units, from their reset state */
    for (i = 0; i < PJ_SLOTS; i++) {
        sSlots[i] = Host_Alloc(sizeof(PassJob));
        if (!sSlots[i])
            return 0;
    }
    sLeak = PASS_ENV("MH2_PASS_LEAK") ? PASS_ENV("MH2_PASS_LEAK")[0] - '0' : 0;
    sInline = PASS_ENV("MH2_PASS_INLINE") ? PASS_ENV("MH2_PASS_INLINE")[0] - '0' : 0;   /* 2: the jobs are thrown away (the game's thread's side alone) */
    sWaitSlot = PASS_ENV("MH2_PASS_WAITSLOT") && PASS_ENV("MH2_PASS_WAITSLOT")[0] == '1';
    CbInit();
    sSem = Host_SemNew();
    sSemSleep = Host_SemNew();
    sRingLock = Host_SemNew();
    if (sCensus)
        Ipc_GsPassRingCensus(1, 0);
    if (!sSem || !sSemSleep || !sRingLock)
        return 0;
    Host_SemPost(sRingLock);
    if (!sInline && !HelpersStart())
        return 0;
    if (!sInline && !Host_ThreadStart(ThreadMain, 0, 1))
        return 0;
    sStarted = 1;
    return 1;
}

PassJob *PassJob_Acquire(void)
{
    u32 i;
    for (;;) {
        for (i = 0; i < PJ_SLOTS; i++)
            if (__atomic_load_n(&sSlotState[i], __ATOMIC_ACQUIRE) == 0) {
                sSlotState[i] = 1;
                return sSlots[i];
            }
        if (!sWaitSlot)
            return 0;
        Host_SemWait(sSemSleep, 1);   /* MH2_PASS_WAITSLOT=1 (tests only): the game's thread waits for a slot, so that no window goes without its pass */
    }
}

void PassJob_Dropped(void)
{
    sDroppedWin++;
}

static void Put(int slot, u64 vblank)
{
    u32 t = sTail;
    if (t - __atomic_load_n(&sHead, __ATOMIC_ACQUIRE) >= PJ_FIFO) {   /* the queue is full (it cannot be: it holds more than the slots and the steps in flight) */
        if (slot >= 0) {
            sDroppedWin++;
            __atomic_store_n(&sSlotState[slot], 0, __ATOMIC_RELEASE);
        } else
            sStepsLost++;
        return;
    }
    sFifo[t % PJ_FIFO].slot = slot;
    sFifo[t % PJ_FIFO].vblank = vblank;
    __atomic_store_n(&sTail, t + 1, __ATOMIC_RELEASE);
    Host_SemPost(sSem);
}

void PassJob_Push(PassJob *j)
{
    u32 i;
    for (i = 0; i < PJ_SLOTS && sSlots[i] != j; i++)
        ;
    sPushed++;
    if (sInline) {
        if (sInline == 1)
            RunJob(j);
        sRan++;
        sSlotState[i] = 0;
        return;
    }
    sSlotState[i] = 2;
    Put((int)i, 0);
}

void PassJob_PushStep(u64 vblank)
{
    if (!sStarted)
        return;
    sStepsPushed++;
    if (sInline) {
        if (sInline == 1)
            RunStep(vblank);
        return;
    }
    Put(-1, vblank);
}

void PassJob_Counts(u64 *pushed, u64 *dropped)
{
    *pushed = sPushed;
    *dropped = sDroppedWin;
}

void PassJob_Finish(void)
{
    char line[200];
    u32 n = 0;
    if (!sStarted)
        return;
    if (sInline) {
        sStarted = 0;
        Gfx_PassFinish();
        return;
    }
    while (__atomic_load_n(&sHead, __ATOMIC_ACQUIRE) != sTail) {   /* drained */
        Host_SemPost(sSem);
        Host_SemWait(sSemSleep, 2);
    }
    __atomic_store_n(&sStop, 1, __ATOMIC_RELEASE);
    Host_SemPost(sSem);
    while (!__atomic_load_n(&sDone, __ATOMIC_ACQUIRE))
        Host_SemWait(sSemSleep, 2);
    HelpersFinish();
    sStarted = 0;
    if (sCensus) {
        Str(line, &n, "mh2-core: pass census: ring puts ");
        Num(line, &n, sRingPuts);
        Str(line, &n, " waited-ns ");
        Num(line, &n, sRingWaitNs);
        Str(line, &n, " held-ns ");
        Num(line, &n, sRingHeldNs);
        line[n] = 0;
        Host_Log(line);
        n = 0;
        {   /* what the time under the lock is made of: the copy, the waits for room, the reader's wakes; and how full the ring was */
            static const char *const nm[10] = {" pieces ", " bytes ", " copy-ns ", " room-waits ", " room-sleeps ", " room-ns ", " wakes ", " wake-ns ", " fill-max ", " fill-sum "};
            unsigned long long c[10];
            u32 i;
            Ipc_GsPassRingCensus(0, c);
            Str(line, &n, "mh2-core: pass census: put");
            for (i = 0; i < 10; i++) {
                Str(line, &n, nm[i]);
                Num(line, &n, c[i]);
            }
            line[n] = 0;
            Host_Log(line);
            n = 0;
        }
        {   /* where the waits for room were: each stretch's first and last VBlank, its sleeps and its time */
            static unsigned long long e[1 + 4 * 24];
            u32 i;
            Ipc_GsPassRingCensus(2, e);
            for (i = 0; i < (u32)e[0]; i++) {
                Str(line, &n, "mh2-core: pass census: room ");
                Num(line, &n, i);
                Str(line, &n, " vblank-first ");
                Num(line, &n, e[1 + i * 4]);
                Str(line, &n, " vblank-last ");
                Num(line, &n, e[2 + i * 4]);
                Str(line, &n, " sleeps ");
                Num(line, &n, e[3 + i * 4]);
                Str(line, &n, " ns ");
                Num(line, &n, e[4 + i * 4]);
                line[n] = 0;
                Host_Log(line);
                n = 0;
            }
        }
    }
    Str(line, &n, "mh2-core: pass thread: windows queued ");
    Num(line, &n, sPushed);
    Str(line, &n, " run ");
    Num(line, &n, sRan);
    Str(line, &n, " no-slot ");
    Num(line, &n, sDroppedWin);
    Str(line, &n, " steps ");
    Num(line, &n, sStepsPushed);
    Str(line, &n, " steps-lost ");
    Num(line, &n, sStepsLost);
    Str(line, &n, " moment-records ");
    Num(line, &n, sMBrecs);
    Str(line, &n, " ring-dropped ");
    Num(line, &n, sRingDropped);
    Str(line, &n, " longest-job-us ");
    Num(line, &n, sJobMaxNs / 1000);
    Str(line, &n, " at ");
    Num(line, &n, sJobMaxAt);
    Str(line, &n, " oddities ");
    Num(line, &n, sCopyOdd);
    line[n] = 0;
    Host_Log(line);
    Gfx_PassFinish();   /* the copy's: its counts and timers, as the pass on the game's thread logs them */
}
