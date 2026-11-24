#ifndef GFX_PASS_JOB_H
#define GFX_PASS_JOB_H

/*
 * The second pass on a thread of its own (MH2_PASS_THREAD=1 in a development build; with the ring in a release): the jobs the game's thread hands it.
 *
 * The game's thread makes the copy of a window as before, but into the buffers of a free slot, and takes the snapshot of the units
 * (what the window starts from, and what the real window left) into the slot's PassBuf; when the window closes it queues the slot.
 * The pass thread (gfx_pass_run.c) takes the slots in order, runs the pass on its own copy of the units, and gives the slot back.
 * A step's end is queued the same way, so that the sets of the steps rotate in the order the game made them. The queue is bounded:
 * when no slot is free the window gets no pass (counted) and the game's thread does not wait.
 */
#include "common.h"
#include "ipc/core_ipc.h"


/* the snapshots of the units: what a window starts from (0), what the real window left (1), and a scratch one (2) */
typedef struct PassBuf {
    u8 vu0[0x10000], vu1[0x10000], vu2[0x10000];
    u8 vif0[1 << 15], vif1[1 << 15], vif2[1 << 15];
    u8 vus0[4096], vus1[4096], vus2[4096], vut[4096];
    u8 vift[1 << 15];
    u32 gif0[2], gif1[2], gif2[2], gs0[4], gs1[4], gs2[4];
} PassBuf;

#define PJ_SLOTS 8u
#define PJ_FIFO 1024u

typedef struct PassJob {
    int kind;                    /* 1 a window, 2 a step's end */
    u32 serial;                  /* the window's number (from 1, in the order the windows were made): its records on both rings carry it */
    u64 vblank;                  /* kind 2 */
    u64 tEnd;                    /* kind 1: the time the window closed on the game's thread (Ipc_TimeNs), for the moments' times from the window's end */
    u8 *ev;                      /* the copy of the window, its length and capacity (Host_Alloc'ed, grown by the game's thread as the window needs) */
    u32 evLen, evCap;
    u8 *real;                    /* the real stream (the identity pass only) */
    u32 realLen, realCap;
    IpcLabel *draws;             /* the labels of the window's model draws */
    u32 nDraws, drawCap;
    u8 *gifReal;                 /* the real path buffers at the window's end (the identity pass only) */
    u32 gifRealLen, gifRealCap;
    u8 ws[256];                  /* the output stage's registers at the window's start (a GsSt) */
    int dirty, overflow;
    u32 csr;                     /* the MXCSR the game's thread had: the pass thread runs its float code under it */
    void *realVu;                /* the real VU memory: only the leak control (MH2_PASS_LEAK=1, tests) writes through it */
    const u16 *pages;            /* the real VIF1's table of the data blocks' pages if it has built it (it is read-only after that), else 0 */
    u32 nMom, momA[6];           /* the live alphas (MH2_PASS_LIVE): the moments this window is made at (0: none), in 1/256; MOM_MAX of gfx_pass.c */
    int left;                    /* helpers: the moments of the window not yet done (counted down by the helpers; the one that reaches 0 sends "all sent" and gives the slot back) */
    void *share;                 /* MH2_PASS_HMODELS: what a helper's Models of a later moment reads of the window (gfx_pass.c's WinShare; the deciding copy makes it, the helpers read it until the countdown ends) */
    PassBuf buf;
} PassJob;

/* The helpers (MH2_PASS_HELPERS=n, 1 to 5: one for each moment of a step at 144 pictures a second): a moment's package (plan 11, 5.2). The deciding thread makes each moment's Models in turn, puts what that moment's Resend reads into a package and
 * queues it; a helper (gfx_pass_help.c, its own units and its own copy of gfx_pass.c) takes any package, loads the window's snapshot 0 from the slot (read only until the window's countdown ends),
 * runs the Resend and puts the moment's record to the second ring. The package has its own copy of the stream and of the arrays the Models made. PJ_MAXDRAWS is gfx_pass.c's MAX_DRAWS. */
#define PJ_MAXDRAWS 4096u
#define PJ_PKGS 16u
#define PJ_HELPERS 5u
typedef struct MomPkg {
    u8 *ev;
    u32 cap, evLen, nDraws, mo, nm;
    int flipQ[PJ_MAXDRAWS];
    u32 flipOff[PJ_MAXDRAWS];
    float flipOld[PJ_MAXDRAWS][2];
    u8 cls[PJ_MAXDRAWS];
    double alpha;
    int skip;
    int models;                  /* MH2_PASS_HMODELS: 1, the package of a moment after the first holds mo, nm, alpha, evLen and nDraws only, and the helper makes the moment's Models from the window's share */
    PassJob *job;                /* the window's slot: the units' snapshot 0, the output stage's start, the labels, the pages table, the MXCSR, the serial (read only) */
    u32 win;                     /* the window's number on the deciding thread (the hash's key) */
    u64 tQueued;                 /* when the package was queued (Ipc_TimeNs) */
} MomPkg;

/* gfx_pass_run.c; the game's thread calls them, and only it */
int PassJob_Start(void);                  /* the pass thread's units and thread, once, before the first window */
PassJob *PassJob_Acquire(void);           /* a free slot, or 0 when none is */
void PassJob_Push(PassJob *j);            /* the window is closed: the slot goes to the pass thread */
void PassJob_PushStep(u64 vblank);        /* a step ends */
void PassJob_Finish(void);                /* the queue is drained and the pass thread stopped; its counts are logged */
void PassJob_Counts(u64 *pushed, u64 *dropped);
void PassJob_Dropped(void);               /* a window found no slot */
/* the helpers (gfx_pass_run.c); the deciding thread calls the first four, a helper the rest */
int PassHelp_On(void);                    /* helpers are running */
MomPkg *PassHelp_Get(void);               /* a free package (waits for one); 0 when the pass is stopping */
void PassHelp_Submit(MomPkg *k);          /* the package is made: a helper takes it */
void PassHelp_Release(PassJob *j);        /* a helper: the window's last moment is done, the slot goes back */
int PassHelp_RingPut(unsigned int arg0, unsigned int arg1, const void *data, unsigned int qwords);   /* the second ring's one record, under the ring's lock */
void PassHelp_MomPut(u32 row, u64 ns);    /* a helper: a moment's time from its window's end, into the deciding copy's rows */
void PassHelp_Done(MomPkg *k);            /* a helper: the package is free again */
void PassHelp_ModelsAdd(u32 n);           /* MH2_PASS_HMODELS, the deciding thread: n Models of this window are the helpers' to make */
void PassHelp_ModelsDone(void);           /* a helper: one of them is made */
void PassHelp_ModelsWait(void);           /* the deciding thread, before a step's end: until none is left */
/* the pass thread's side: gfx_pass.c (the copy of it) runs a job */
#endif
