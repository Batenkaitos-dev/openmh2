#ifndef MH2_KERNEL_KERN_INT_H
#define MH2_KERNEL_KERN_INT_H

/*
 * The kernel's own declarations, shared by the files of pc/core/kernel/
 * and pc/core/api/libkernl*.c. Other modules use kern.h.
 */
#include "kernel/kern.h"

/* Kernel error codes (Sony's). */
#define KE_OK 0
#define KE_ERROR (-1)
#define KE_ILLEGAL_THID (-406)
#define KE_UNKNOWN_THID (-407)
#define KE_UNKNOWN_SEMID (-408)
#define KE_DORMANT (-413)
#define KE_NOT_DORMANT (-414)
#define KE_NOT_SUSPEND (-415)
#define KE_NOT_WAIT (-416)
#define KE_RELEASE_WAIT (-418)
#define KE_SEMA_ZERO (-419)
#define KE_WAIT_DELETE (-425)

/* Thread states (ReferThreadStatus's values). */
enum { THS_RUN = 0x01, THS_READY = 0x02, THS_WAIT = 0x04, THS_SUSPEND = 0x08, THS_WAITSUSPEND = 0x0C, THS_DORMANT = 0x10 };
enum { WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2 };

#define KERN_NS_PER_VBLANK 16683350ull     /* VBlank n starts at n x this (the specification, 2.1) */
#define KERN_NS_VBLANK_END 1270000ull      /* the end follows the start by this */
#define KERN_NS_PER_HSYNC 63556ull         /* SetAlarm's unit */
#define KERN_BUS_HZ 147456000ull

/* ---- fiber.c: a host stack per game thread, switched by hand ---- */
typedef struct KFiber KFiber;
typedef void (*KFiberFn)(void *arg);
KFiber *KFiber_Create(KFiberFn fn, void *arg);   /* from the low pool; fn never returns */
KFiber *KFiber_Main(void);                       /* the fiber of the caller's own stack (no memory) */
void KFiber_Switch(KFiber *from, KFiber *to);    /* saves the caller in `from`, runs `to` */
void KFiber_Free(KFiber *f);                      /* back to the pool (not the running one) */
KFiber *KFiber_Current(void);
int KFiber_Holds(const KFiber *f, const void *p); /* p lies in f's stack (never for the main fiber) */
/* Save states (fiber.c, state.c): fn(arg) runs on a stack outside the state; RunOutside returns 0 when it returns,
 * or 1 when a load resumed the context (KFiber_ResumeAt(sp), sp from OutsideSp while fn runs). */
int KFiber_RunOutside(void (*fn)(void *), void *arg);
void *KFiber_OutsideSp(void);
void KFiber_ResumeAt(void *sp) __attribute__((noreturn));

/* ---- sched.c: time, events and the delivery points ---- */
void KSched_Reset(void);
char **KStart_PutArgs(void);       /* main's argc, argv and string in crt0's argument block (start.c) */
u64 KSched_RunDue(void);          /* fires the due events; the time to the next one, or ~0 */
int KSched_HasEvents(void);
void KSched_Jump(void);           /* the jump: fire due events, then if settled go to the next event */
void KSched_Deliver(void);        /* a delivery point (4), with the requested reschedule */
void KSched_IdleWait(void);       /* the idle loop's time wait (clears fseen) */
int KSched_Settled(void);         /* the specification, 3 */
extern int g_kern_fseen;
extern int g_kern_log;      /* MH2_KERNEL_LOG=1: a line per kernel object made (start.c), as the original's --log debug */

/* ---- thread.c: threads, semaphores, handlers, alarms, interrupts ---- */
void KThread_Reset(void);
int KThread_DeliverInterrupts(void);       /* 4.2: the handlers run; returns how many steps ran */
int KThread_InterruptPending(void);        /* section 3, item 1 */
void KThread_ReschedIfAsked(void);         /* 4, step 3 */
int KThread_InterruptsEnabled(void);       /* the context allows interrupts */
void KThread_SetEie(int on);               /* ei / di of the current context (no delivery) */
int KThread_GetEie(void);
int KThread_InEvent(void);
void KThread_EnterEvent(void);
void KThread_LeaveEvent(void);
void KThread_RunIdle(void);                /* the run loop: never returns */
s32 KThread_CreateMain(void (*fn)(void *), void *arg, s32 prio);
u32 KThread_IntcStat(void);
u32 KThread_IntcMask(void);
void KThread_SetIntcMask(u32 mask);
void KThread_SetIntcStat(u32 stat);
u32 KThread_DmacStat(void);
void KThread_SetDmacStat(u32 stat);

/* A library's own interrupt handler (the SIF's DMAC handler, the alarm module's timer handler):
 * kept in the same table as the game's, so that it takes an id and an order value as the SDK's does. */
typedef s32 (*KHandlerFn)(s32 cause, void *arg);
s32 KThread_AddHandler(int kind, s32 cause, s32 (*fn)(s32), s32 next, void *arg, int lib, KHandlerFn libFn);
s32 KThread_RemoveHandler(int kind, s32 cause, s32 id);
s32 KThread_SetHandlerEnabled(s32 id, int on);
s32 KThread_EnableCause(int kind, s32 cause, int on);   /* the mask bit; returns the change (1) or not (0) */

/* The game's thread and semaphore calls (libkernl's syscalls). */
typedef struct KThreadParam {
    void (*entry)(void *);
    u32 stack, stackSize, gp;
    s32 initPriority;
    u32 attr, option;
} KThreadParam;
s32 KThread_Create(const KThreadParam *p);
s32 KThread_Delete(s32 id);
s32 KThread_Start(s32 id, void *arg);
void KThread_Exit(int deleteAfter) __attribute__((noreturn));
s32 KThread_Terminate(s32 id);
s32 KThread_ChangePriority(s32 id, s32 prio);
s32 KThread_RotateReadyQueue(s32 prio);
s32 KThread_ReleaseWait(s32 id);
s32 KThread_GetId(void);
s32 KThread_Refer(s32 id, u32 *info12);       /* the 12 words of ReferThreadStatus; returns the status */
s32 KThread_Sleep(void);
s32 KThread_Wakeup(s32 id);
s32 KThread_CancelWakeup(s32 id);
s32 KThread_Suspend(s32 id);
s32 KThread_Resume(s32 id);
void KThread_SetDispatch(int enabled);
s32 KThread_SemaRefer(s32 id, s32 *info6);
s32 KThread_AlarmSet(u32 hsyncs, void (*fn)(s32, u16, void *), void *arg);
s32 KThread_AlarmRelease(s32 id);

/* ---- timer.c: the EE timers ---- */
void KTimer_Reset(void);

/* ---- alarm.c: libkernl's timer and alarm module ---- */
void KAlarm_Init(void);
/* An alarm's callback (id, its clocks, the clocks since its start, arg): 0 ends the alarm; else the alarm
 * comes again that many clocks later (at least 100 us). */
typedef s32 (*KAlarmFn)(u32 id, u64 clocks, u64 elapsed, void *arg);
s32 KAlarm_Set(u64 clocks, KAlarmFn fn, void *arg);   /* the record's id, or an error (negative) */
u64 KAlarm_SystemTime(void);                          /* GetTimerSystemTime: bus clocks */

/* ---- vblank.c ---- */
/* Save states (state.c; weak no-ops where it is not linked). */
void KState_VBlankStart(u64 vblank);   /* first in a VBlank's start: a request taken, a save made or a load made */
void KState_BootLoad(void);            /* before the game runs: a state named on the command line, development */
s32 Kern_StateOption(s32 argc, char **argv, s32 i);

void KVblank_Reset(void);           /* queues VBlank 1 */
void KVblank_SetStop(u64 frames);   /* the run ends at VBlank `frames` (0: never) */
#define KERN_EXIT_CRASH 70     /* the exit code after a crash (start.c) */
void KVblank_Finish(const char *reason);           /* the run's end: the SPU2 to now, verify's last lines */

/* ---- sif.c: the SIF, the RPC client's queue and the IOP's scheduling ---- */
void KSif_Reset(void);
int KSif_IopCatchUp(void);          /* 4, step 2a: 0, 1 or 2 */
int KSif_IopBusy(void);             /* section 3, item 2 */
int KSif_HasWork(void);             /* section 3, item 3 */
int KSif_Pump(void);                /* 4, step 2b: moves one packet; 1 if it did */
void KSif_IopVblank(int start);
void KSif_UnparkOnEvent(void);

/* ---- run.c ---- */
/* The kernel's state, then the other modules' start-up: io's Io_Init(argc, argv), gfx's Gfx_Init, the weak hooks. */
void Kern_Start(u64 maxFrames, int argc, char **argv);
/* Kern_Start, then ps2_main on thread 1 and the run loop. */
void Kern_Boot(u64 maxFrames, int argc, char **argv) __attribute__((noreturn));

#endif /* MH2_KERNEL_KERN_INT_H */
