/*
 * libkernl, the EE kernel's library, as Capcom's code calls it: threads,
 * semaphores, interrupt handlers, the interrupt enable, the caches. Each
 * function's behaviour at its interface is the PS2 library's, on the kernel of
 * pc/core/kernel/ and the frame clock. The SIF's part of the library is
 * libkernl_sif.c.
 */
#include "common.h"
#include "libkernl.h"
#include "sif.h"
#include "kernel/kern_int.h"
#include "kernel/klog.h"
#include "pal_core.h"

/* ------------------------------------------------------------- handlers */

s32 AddIntcHandler(s32 cause, s32 (*handler)(s32), s32 next)
{
    return KThread_AddHandler(0, cause, handler, next, 0, 0, 0);
}

s32 AddDmacHandler(s32 ch, s32 (*handler)(s32), s32 next)
{
    return KThread_AddHandler(1, ch, handler, next, 0, 0, 0);
}

s32 RemoveIntcHandler(s32 cause, s32 id)
{
    return KThread_RemoveHandler(0, cause, id);
}

s32 RemoveDmacHandler(s32 channel, s32 id)
{
    return KThread_RemoveHandler(1, channel, id);
}

/* ------------------------------------------------- the interrupt enable */

/* DIntr: interrupts off (the PS2's returns 1 if they were on; Capcom's code declares it void here). */
void DIntr(void)
{
    KThread_SetEie(0);
}

/* EIntr, the game's own call (polls.txt's `deliver`): interrupts on, and a delivery point when the context's
 * interrupts are then on (the specification, 4.1 d). The library's own uses of it below set the bit without
 * delivering (6.3). The PS2's returns the enable's old bit; sif.h declares it void. */
void EIntr(void)
{
    KThread_SetEie(1);
    if (KThread_InterruptsEnabled()) {
        KSched_Deliver();
    }
}

/* Status.IE is always set: 0. */
s32 func_001145F0(void)
{
    return 0;
}

/* EnableIntc and the like: with the enable on, DIntr's work, the syscall and the library's EIntr (which
 * delivers nothing); the syscall's result (1 if the mask bit changed). */
static s32 MaskCall(int kind, s32 cause, int on)
{
    s32 r;
    if (KThread_GetEie()) {
        KThread_SetEie(0);
        r = KThread_EnableCause(kind, cause, on);
        KThread_SetEie(1);
        return r;
    }
    return KThread_EnableCause(kind, cause, on);
}

s32 EnableIntc(s32 cause)
{
    return MaskCall(0, cause, 1);
}

void DisableIntc(s32 cause)
{
    MaskCall(0, cause, 0);
}

s32 EnableDmac(s32 ch)
{
    return MaskCall(1, ch, 1);
}

void DisableDmac(s32 ch)
{
    MaskCall(1, ch, 0);
}

s32 iEnableIntc(s32 cause)
{
    return KThread_EnableCause(0, cause, 1);
}

s32 iDisableIntc(s32 cause)
{
    return KThread_EnableCause(0, cause, 0);
}

/* ----------------------------------------------------------- the caches */

void FlushCache(s32 mode)
{
    (void)mode;
}

void iFlushCache(s32 mode)
{
    (void)mode;
}

void SyncDCache(void *start, void *end)
{
}

/* --------------------------------------------------------------- threads */

static s32 CreateThreadWork(struct ThreadParam *p)
{
    KThreadParam k;
    k.entry = (void (*)(void *))p->entry;
    k.stack = (u32)p->stack;
    k.stackSize = (u32)p->stackSize;
    k.gp = (u32)p->gpReg;
    k.initPriority = p->initPriority;
    k.attr = p->attr;
    k.option = p->option;
    return KThread_Create(&k);
}

s32 CreateThread(struct ThreadParam *p)
{
    return CreateThreadWork(p);
}

s32 DeleteThread(s32 id)
{
    return KThread_Delete(id);
}

s32 TerminateThread(s32 id)
{
    return KThread_Terminate(id);
}

s32 ChangeThreadPriority(s32 tid, s32 prio)
{
    return KThread_ChangePriority(tid, prio);
}

s32 GetThreadId(void)
{
    return KThread_GetId();
}

/*
 * func_00114C60(thread, arg) starts a dormant thread with arg (Capcom starts the CSE and ML RPC threads with
 * it): -1 with interrupts off; else, with them off meanwhile, ReferThreadStatus's error, or -1 unless the
 * thread is dormant; then arg is stored as a doubleword 0x260 bytes below the thread's stack top and
 * StartThread(thread, arg)'s result returned.
 */
static s32 StartWithArgWork(s32 tid, s32 arg)
{
    u32 info[12];
    s32 r;
    if (!KThread_GetEie()) {
        return -1;
    }
    KThread_SetEie(0);
    r = KThread_Refer(tid, info);
    KThread_SetEie(1);
    if (r < 0) {
        return r;
    }
    if (info[0] != THS_DORMANT) {
        return -1;
    }
    {
        u32 *slot = (u32 *)(info[2] + info[3] - 0x260u);
        slot[0] = (u32)arg;
        slot[1] = (u32)(arg >> 31);
    }
    return KThread_Start(tid, (void *)arg);
}

s32 func_00114C60(s32 tid, s32 arg)
{
    return StartWithArgWork(tid, arg);
}

/* ------------------------------------------------------------ semaphores */

s32 CreateSema(SemaParam *p)
{
    return Kern_SemaCreate(p->initCount, p->maxCount, p->attr, p->option);
}

s32 SignalSema(s32 sema)
{
    return Kern_SemaSignal(sema);
}

s32 iSignalSema(s32 sema)
{
    return Kern_SemaSignal(sema);
}

s32 WaitSema(s32 sema)
{
    return Kern_SemaWait(sema);
}

/* ---------------------------------------------------------------- timers */

/* DelayThread (port-libraries.md, "DelayThread and its alarm's callback"): 0x80008008 at once with interrupts
 * off; else a semaphore (initial 0, maximum 1, the option word 0x1BE5D8; 0x80008003 if none), an alarm of
 * usec in bus clocks whose callback signals it (its error, with the semaphore deleted, if the module has no
 * free alarm), the wait, the semaphore deleted, 0. */
static s32 DelayWake(u32 id, u64 clocks, u64 elapsed, void *arg)
{
    (void)id;
    (void)clocks;
    (void)elapsed;
    Kern_SemaSignal((s32)(u32)arg);
    return 0;
}

static s32 DelayThreadWork(s32 usec)
{
    s32 sema, r;
    if (!KThread_GetEie()) {
        return (s32)0x80008008u;
    }
    sema = Kern_SemaCreate(0, 1, 0, 0x1BE5D8u);
    if (sema < 0) {
        return (s32)0x80008003u;
    }
    r = KAlarm_Set((u64)(s64)usec * KERN_BUS_HZ / 1000000ull, DelayWake, (void *)(u32)sema);
    if (r < 0) {
        Kern_SemaDelete(sema);
        return r;
    }
    Kern_SemaWait(sema);
    Kern_SemaDelete(sema);
    return 0;
}

s32 DelayThread(s32 usec)
{
    return DelayThreadWork(usec);
}

/* The alarm module's system time in bus clocks (port-libraries.md, "The timer and alarm module's stateful
 * part"): ((Timer 2's overflows << 16) | its count) << its clock's shift, with interrupts off. */
s64 GetTimerSystemTime(void)
{
    return (s64)(KAlarm_SystemTime());
}

/* The bus clocks as seconds and microseconds (port-libraries.md, "TimerBusClock2USec"): the microseconds
 * from the clocks less the seconds' low word times the rate, in 64 bits wrapping. */
void TimerBusClock2USec(s64 clocks, u32 *sec, u32 *usec)
{
    u64 v = (u64)clocks;
    u64 s = v / KERN_BUS_HZ;
    if (sec) {
        *sec = (u32)s;
    }
    if (usec) {
        u64 r = (v - (u64)(u32)s * KERN_BUS_HZ) * 1000000ull;
        *usec = (u32)(r / KERN_BUS_HZ);
    }
}

/* -------------------------------------------------------------- printing */

s32 vsnprintf(char *buf, s32 size, const char *fmt, __builtin_va_list ap);

/* scePrintf: the IOP's console, answered at once (no wait); the line goes to the log. */
void scePrintf(const char *fmt, ...)
{
    char buf[1024];
    s32 n;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    n = 0;
    while (buf[n]) {
        n++;
    }
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
    if (n) {
        KLog("guest: %s", buf);
    }
}

/* ---------------------------------------------- for the other libraries */

/* libkernl's calls that Capcom's code does not make but the platform's other libraries do, as the PS2's
 * libraries call them. */
s32 DeleteSema(s32 sema);
s32 PollSema(s32 sema);
s32 StartThread(s32 tid, void *arg);
s32 SleepThread(void);
s32 WakeupThread(s32 tid);
s32 iWakeupThread(s32 tid);
s32 ReferThreadStatus(s32 tid, struct ThreadParam *st);
s32 iReferThreadStatus(s32 tid, struct ThreadParam *st);
s32 SuspendThread(s32 tid);
s32 ResumeThread(s32 tid);
void GetOsdConfigParam(void *p);
void SetOsdConfigParam(void *p);

s32 DeleteSema(s32 sema)
{
    return Kern_SemaDelete(sema);
}

s32 PollSema(s32 sema)
{
    return Kern_SemaPoll(sema);
}

s32 StartThread(s32 tid, void *arg)
{
    return KThread_Start(tid, arg);
}

s32 SleepThread(void)
{
    return KThread_Sleep();
}

s32 WakeupThread(s32 tid)
{
    return KThread_Wakeup(tid);
}

s32 iWakeupThread(s32 tid)
{
    return KThread_Wakeup(tid);
}

/* ReferThreadStatus (and its i- form): KE_UNKNOWN_THID, or the thread's status, its 12 words written to st
 * when st is not null (status, entry, stack, size, gp, initial and current priority, attr, option, the wait's
 * type and id, the wakeup count). */
s32 ReferThreadStatus(s32 tid, struct ThreadParam *st)
{
    u32 info[12];
    s32 r = KThread_Refer(tid, info);
    _Static_assert(sizeof(struct ThreadParam) == sizeof(info), "ThreadParam is the PS2's 12 words");
    if (r >= 0 && st) {
        __builtin_memcpy(st, info, sizeof(info));
    }
    return r;
}

s32 iReferThreadStatus(s32 tid, struct ThreadParam *st)
{
    return ReferThreadStatus(tid, st);
}

/* SuspendThread: KE_ILLEGAL_THID for the caller or an unknown thread; a ready thread becomes suspended, a
 * waiting one wait-suspended, any other is left; the id. ResumeThread: the reverse (the thread ready at the
 * tail of its priority, or waiting again), KE_NOT_SUSPEND when it isn't suspended. */
s32 SuspendThread(s32 tid)
{
    return KThread_Suspend(tid);
}

s32 ResumeThread(s32 tid)
{
    return KThread_Resume(tid);
}

/* The OSD's configuration word (the original's kernel: 540 << 21 | 1 << 13 until set). */
static u32 sOsdConfig = (540u << 21) | (1u << 13);

void GetOsdConfigParam(void *p)
{
    *(u32 *)p = sOsdConfig;
}

void SetOsdConfigParam(void *p)
{
    sOsdConfig = *(const u32 *)p;
}
