/*
 * The EE kernel's threads, semaphores, interrupt handlers, alarms and the
 * interrupt-enable state. Strict priorities (0 is the highest), first in first
 * out within a priority; a thread released by a wait joins the tail of its
 * priority, a preempted one stays first. Each thread runs on a fiber of its
 * own (fiber.c); the idle loop has one too. The original game's
 * kernel.c is the same semantics written against guest registers.
 */
#include "kernel/kern_int.h"
#include "pal_core.h"
#include "kernel/klog.h"

#define MAX_THREADS 256
#define MAX_SEMAS 256
#define MAX_HANDLERS 128
#define MAX_ALARMS 64

typedef struct KThread {
    int used;
    s32 id;
    int status;
    void (*entry)(void *);
    void *arg;
    u32 stack, stackSize, gp;
    s32 initPrio, prio;
    u32 attr, option;
    int waitType;
    s32 waitId;
    s64 waitSeq;
    s32 wakeupCount;
    s64 seq;             /* the order in the ready queue */
    s32 waitResult;
    int eie;             /* the thread's interrupt enable (Status.EIE) */
    KFiber *fiber;
} KThread;

typedef struct KSema {
    int used;
    s32 count, max, init;
    u32 attr, option;
} KSema;

typedef struct KHandler {
    int used;
    int kind;            /* 0 INTC, 1 DMAC */
    s32 cause;
    s32 (*fn)(s32);      /* the game's */
    KHandlerFn libFn;    /* a library's own (the SIF's, the alarm module's) */
    void *arg;
    int enabled;
    s64 order;
} KHandler;

typedef struct KAlarm {
    int used;
    int fired;
    void (*fn)(s32, u16, void *);
    void *arg;
    u16 time;
    u32 event;
} KAlarm;

static KThread sThreads[MAX_THREADS];
static KSema sSemas[MAX_SEMAS];
static KHandler sHandlers[MAX_HANDLERS];
static KAlarm sAlarms[MAX_ALARMS];
static KThread *sCur;
static KFiber *sIdleFiber;
static KFiber *sZombie;
static s64 sSeqTail, sSeqHead, sWaitSeq, sHandlerOrder;
static int sInInterrupt, sInEvent, sNeedResched, sDispatchDisabled;
static int sIdleEie, sIntEie;
static int sAlarmsFired;
static u32 sIntcStat, sIntcMask, sDmacStat;
static const u8 *sHandlerSp;    /* the fiber's stack pointer when the handlers started (Kern_StackOwner) */


void KThread_Reset(void)
{
    __builtin_memset(sThreads, 0, sizeof(sThreads));
    __builtin_memset(sSemas, 0, sizeof(sSemas));
    __builtin_memset(sHandlers, 0, sizeof(sHandlers));
    __builtin_memset(sAlarms, 0, sizeof(sAlarms));
    sCur = 0;
    sZombie = 0;
    sSeqTail = 1;
    sSeqHead = -1;
    sWaitSeq = 1;
    sHandlerOrder = 1;
    sInInterrupt = sInEvent = sNeedResched = sDispatchDisabled = 0;
    sIdleEie = 1;
    sIntEie = 1;
    sAlarmsFired = 0;
    sIntcStat = sIntcMask = sDmacStat = 0;
}

/* ---------------------------------------------------------------- helpers */

static KThread *ThreadGet(s32 id)
{
    if (id == 0 && sCur) {
        return sCur;
    }
    if (id <= 0 || id >= MAX_THREADS || !sThreads[id].used) {
        return 0;
    }
    return &sThreads[id];
}

static KSema *SemaGet(s32 id)
{
    if (id <= 0 || id >= MAX_SEMAS || !sSemas[id].used) {
        return 0;
    }
    return &sSemas[id];
}

static void ThreadMain(void *p);

static void EnsureFiber(KThread *t)
{
    if (!t->fiber) {
        t->fiber = KFiber_Create(ThreadMain, t);
    }
}

static KFiber *CurFiber(void)
{
    return sCur ? sCur->fiber : sIdleFiber;
}

static void FreeZombie(void)
{
    if (sZombie && sZombie != CurFiber()) {
        KFiber_Free(sZombie);
        sZombie = 0;
    }
}

static KThread *PickNext(void)
{
    KThread *best = 0;
    int i;
    for (i = 1; i < MAX_THREADS; i++) {
        KThread *t = &sThreads[i];
        if (!t->used || t->status != THS_READY) {
            continue;
        }
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)) {
            best = t;
        }
    }
    return best;
}

static void MakeReady(KThread *t, int atHead)
{
    t->status = THS_READY;
    t->seq = atHead ? sSeqHead-- : sSeqTail++;
}

/* The running thread leaves the processor (the original's seam_switch, in its reschedule). */
static void SwitchedAway(KThread *t, KThread *next)
{
}

/* Switches away from the running thread if another should run (8, 8.1). In a handler or an event it only asks
 * for the switch, which the delivery point makes at its end (4, step 3). */
static void Reschedule(void)
{
    KThread *cur = sCur, *next;
    if (sInInterrupt || sInEvent) {
        sNeedResched = 1;
        return;
    }
    if (!cur) {
        return;     /* the idle loop picks the next thread itself */
    }
    next = PickNext();
    if (cur->status == THS_RUN) {
        if (!next || next->prio >= cur->prio || sDispatchDisabled) {
            return;
        }
        MakeReady(cur, 1);      /* preempted: it stays first in its priority */
    }
    if (!next) {
        SwitchedAway(cur, 0);
        sCur = 0;
        KFiber_Switch(cur->fiber, sIdleFiber);
    } else {
        if (next != cur) {
            SwitchedAway(cur, next);
        }
        sCur = next;
        next->status = THS_RUN;
        EnsureFiber(next);
        KFiber_Switch(cur->fiber, next->fiber);
    }
    FreeZombie();
}

void KThread_ReschedIfAsked(void)
{
    if (sNeedResched) {
        sNeedResched = 0;
        Reschedule();
    }
}

/* A blocking wait (8): the thread waits, the instant is settled (a delivery point, also when nothing is
 * pending), then the thread choice. */
static s32 ThreadWait(int type, s32 id)
{
    KThread *t = sCur;
    if (!t || sInInterrupt || sInEvent) {
        return KE_ERROR;
    }
    t->status = THS_WAIT;
    t->waitType = type;
    t->waitId = id;
    t->waitSeq = sWaitSeq++;
    t->waitResult = 0;
    KSched_Deliver();
    Reschedule();
    return t->waitResult;
}

static void ThreadRelease(KThread *t, s32 result)
{
    t->waitType = WAIT_NONE;
    t->waitResult = result;
    if (t->status == THS_WAITSUSPEND) {
        t->status = THS_SUSPEND;
    } else {
        MakeReady(t, 0);
    }
}

void KThread_Exit(int deleteAfter)
{
    KThread *t = sCur, *next;
    if (!t || sInInterrupt) {
        Pal_Fatal("kernel: ExitThread outside a thread");
    }
    t->status = THS_DORMANT;
    FreeZombie();
    sZombie = t->fiber;
    t->fiber = 0;
    if (deleteAfter) {
        t->used = 0;
    }
    sCur = 0;
    next = PickNext();
    if (next) {
        sCur = next;
        next->status = THS_RUN;
        EnsureFiber(next);
        KFiber_Switch(sZombie, next->fiber);
    } else {
        KFiber_Switch(sZombie, sIdleFiber);
    }
    Pal_Fatal("kernel: an exited thread resumed");
    for (;;) {
    }
}

static void ThreadMain(void *p)
{
    KThread *t = (KThread *)p;
    FreeZombie();
    t->entry(t->arg);
    KThread_Exit(0);      /* returning from the entry is ExitThread */
}

/* ------------------------------------------------------------- interrupts */

int Kern_InHandler(void)
{
    return sInInterrupt;
}

s32 Kern_ThreadId(void)
{
    return sCur ? sCur->id : 0;
}

s32 Kern_StackOwner(const void *p)
{
    int i;
    if (sInInterrupt && (const u8 *)p < sHandlerSp && KFiber_Holds(KFiber_Current(), p)) {
        return KERN_STACK_HANDLER;
    }
    for (i = 1; i < MAX_THREADS; i++) {
        const KThread *t = &sThreads[i];
        if (t->used && t->fiber && KFiber_Holds(t->fiber, p)) {
            return t->id;
        }
    }
    return -1;
}

static int *EiePtr(void)
{
    return sInInterrupt ? &sIntEie : sCur ? &sCur->eie : &sIdleEie;
}

int KThread_InterruptsEnabled(void)
{
    if (sInInterrupt) {
        return 0;
    }
    return *EiePtr();
}

void KThread_SetEie(int on)
{
    *EiePtr() = on ? 1 : 0;
}

int KThread_GetEie(void)
{
    return *EiePtr();
}

int Kern_LibDIntr(void)
{
    int old = *EiePtr();
    *EiePtr() = 0;
    return old;
}

int Kern_LibEIntr(void)
{
    int old = *EiePtr();
    *EiePtr() = 1;
    return old;
}

int KThread_InEvent(void)
{
    return sInEvent;
}

void KThread_EnterEvent(void)
{
    sInEvent++;
}

void KThread_LeaveEvent(void)
{
    sInEvent--;
}

static u32 DmacPending(void)
{
    return (sDmacStat & 0x3FFu) & ((sDmacStat >> 16) & 0x3FFu);
}

int KThread_InterruptPending(void)
{
    if (sInInterrupt || !KThread_InterruptsEnabled()) {
        return 0;
    }
    return (sIntcStat & sIntcMask) != 0 || DmacPending() != 0 || sAlarmsFired;
}

static void CallHandler(KHandler *h, s32 cause)
{
    sIntEie = 1;
    if (h->libFn) {
        h->libFn(cause, h->arg);
    } else {
        h->fn(cause);
    }
}

/* The handlers of one cause (4.2, 4.3): a snapshot of the used and enabled ones, in order; each is checked
 * again before it runs. */
static void RunHandlers(int kind, s32 cause)
{
    int ids[MAX_HANDLERS], n = 0, i, a, b;
    for (i = 0; i < MAX_HANDLERS; i++) {
        if (sHandlers[i].used && sHandlers[i].kind == kind && sHandlers[i].cause == cause && sHandlers[i].enabled) {
            ids[n++] = i;
        }
    }
    for (a = 1; a < n; a++) {
        for (b = a; b > 0 && sHandlers[ids[b]].order < sHandlers[ids[b - 1]].order; b--) {
            int t = ids[b];
            ids[b] = ids[b - 1];
            ids[b - 1] = t;
        }
    }
    for (i = 0; i < n; i++) {
        KHandler *h = &sHandlers[ids[i]];
        if (!h->used || !h->enabled) {
            continue;
        }
        CallHandler(h, cause);
    }
}

static int RunFiredAlarms(void)
{
    int i, ran = 0;
    if (!sAlarmsFired) {
        return 0;
    }
    sAlarmsFired = 0;
    for (i = 0; i < MAX_ALARMS; i++) {
        KAlarm *a = &sAlarms[i];
        if (!a->used || !a->fired) {
            continue;
        }
        a->used = 0;
        a->fired = 0;
        sIntEie = 1;
        a->fn(i + 1, a->time, a->arg);
        ran = 1;
    }
    return ran;
}

/* 4.2: the context is checked once; then up to 64 steps, the pending sets read again before each. */
int KThread_DeliverInterrupts(void)
{
    int taken = 0, guard;
    if (sInInterrupt || !KThread_InterruptsEnabled()) {
        return 0;
    }
    for (guard = 0; guard < 64; guard++) {
        u32 intc = sIntcStat & sIntcMask;
        u32 dmac = DmacPending();
        sHandlerSp = (const u8 *)__builtin_frame_address(0);
        if (!intc && !dmac) {
            int ran;
            sInInterrupt = 1;
            ran = RunFiredAlarms();
            sInInterrupt = 0;
            if (!ran) {
                return taken;
            }
            taken++;
            continue;
        }
        sInInterrupt = 1;
        if (intc) {
            s32 cause = 0;
            while (!(intc & (1u << cause))) {
                cause++;
            }
            sIntcStat &= ~(1u << cause);
            RunHandlers(0, cause);
        } else {
            s32 ch = 0;
            while (!(dmac & (1u << ch))) {
                ch++;
            }
            sDmacStat &= ~(1u << ch);
            RunHandlers(1, ch);
        }
        sInInterrupt = 0;
        taken++;
    }
    return taken;
}

void Kern_RaiseIntc(int cause)
{
    sIntcStat |= 1u << (cause & 31);
}

void Kern_RaiseDmac(int channel)
{
    sDmacStat |= 1u << (channel & 15);
}

u32 KThread_IntcStat(void) { return sIntcStat; }
u32 KThread_IntcMask(void) { return sIntcMask; }
void KThread_SetIntcMask(u32 mask) { sIntcMask = mask; }
void KThread_SetIntcStat(u32 stat) { sIntcStat = stat; }
u32 KThread_DmacStat(void) { return sDmacStat; }
void KThread_SetDmacStat(u32 stat) { sDmacStat = stat; }

s32 KThread_AddHandler(int kind, s32 cause, s32 (*fn)(s32), s32 next, void *arg, int lib, KHandlerFn libFn)
{
    int i;
    for (i = 1; i < MAX_HANDLERS; i++) {
        KHandler *h = &sHandlers[i];
        if (h->used) {
            continue;
        }
        h->used = 1;
        h->kind = kind;
        h->cause = cause;
        h->fn = lib ? 0 : fn;
        h->libFn = lib ? libFn : 0;
        h->arg = arg;
        h->enabled = 1;
        h->order = next == 0 ? -(sHandlerOrder++) : sHandlerOrder++;
        if (g_kern_log) {
            KLog("kernel: Add%sHandler(%d) = %d at %llu", kind ? "Dmac" : "Intc", cause, i, Kern_Now());
        }
        return i;
    }
    return KE_ERROR;
}

s32 KThread_RemoveHandler(int kind, s32 cause, s32 id)
{
    if (id <= 0 || id >= MAX_HANDLERS || !sHandlers[id].used || sHandlers[id].kind != kind ||
        sHandlers[id].cause != cause) {
        return KE_ERROR;
    }
    sHandlers[id].used = 0;
    return 0;
}

s32 KThread_SetHandlerEnabled(s32 id, int on)
{
    if (id > 0 && id < MAX_HANDLERS && sHandlers[id].used) {
        sHandlers[id].enabled = on ? 1 : 0;
    }
    return 0;
}

/* _EnableIntc/_DisableIntc and the DMAC's: the mask bit. Returns 1 if it changed. */
s32 KThread_EnableCause(int kind, s32 cause, int on)
{
    u32 bit = kind == 0 ? 1u << (cause & 31) : 1u << ((cause & 15) + 16);
    u32 *reg = kind == 0 ? &sIntcMask : &sDmacStat;
    int was = (*reg & bit) != 0;
    if (on) {
        *reg |= bit;
        return !was;
    }
    *reg &= ~bit;
    return was;
}

/* ----------------------------------------------------------------- alarms */

static void AlarmEvent(u64 now, u32 param)
{
    KAlarm *a = &sAlarms[param];
    (void)now;
    if (!a->used) {
        return;
    }
    a->fired = 1;
    sAlarmsFired = 1;
}

s32 KThread_AlarmSet(u32 hsyncs, void (*fn)(s32, u16, void *), void *arg)
{
    int i;
    for (i = 0; i < MAX_ALARMS; i++) {
        KAlarm *a = &sAlarms[i];
        if (a->used) {
            continue;
        }
        a->used = 1;
        a->fired = 0;
        a->fn = fn;
        a->arg = arg;
        a->time = (u16)(hsyncs & 0xFFFF);
        a->event = Kern_EventAdd(Kern_Now() + (u64)(hsyncs & 0xFFFF) * KERN_NS_PER_HSYNC, AlarmEvent, (u32)i);
        return i + 1;
    }
    return KE_ERROR;
}

s32 KThread_AlarmRelease(s32 id)
{
    if (id <= 0 || id > MAX_ALARMS || !sAlarms[id - 1].used) {
        return KE_ERROR;
    }
    Kern_EventCancel(sAlarms[id - 1].event);
    sAlarms[id - 1].used = 0;
    sAlarms[id - 1].fired = 0;
    return id;
}

/* ---------------------------------------------------------------- threads */

static void InitThreadRun(KThread *t, void *arg)
{
    t->arg = arg;
    t->eie = 1;
    t->wakeupCount = 0;
    t->waitType = WAIT_NONE;
}

s32 KThread_Create(const KThreadParam *p)
{
    int i;
    for (i = 2; i < MAX_THREADS; i++) {
        KThread *t = &sThreads[i];
        if (t->used) {
            continue;
        }
        __builtin_memset(t, 0, sizeof(*t));
        t->used = 1;
        t->id = i;
        t->status = THS_DORMANT;
        t->entry = p->entry;
        t->stack = p->stack;
        t->stackSize = p->stackSize;
        if (p->stack && p->stackSize) {
        }
        t->gp = p->gp;
        t->initPrio = t->prio = p->initPriority;
        t->attr = p->attr;
        t->option = p->option;
        if (g_kern_log) {
            KLog("kernel: CreateThread %d prio %d at %llu", i, p->initPriority, Kern_Now());
        }
        return i;
    }
    return KE_ERROR;
}

/* The main thread (id 1), ready to run fn(arg) when the run loop starts. */
s32 KThread_CreateMain(void (*fn)(void *), void *arg, s32 prio)
{
    KThread *t = &sThreads[1];
    __builtin_memset(t, 0, sizeof(*t));
    t->used = 1;
    t->id = 1;
    t->entry = fn;
    t->initPrio = t->prio = prio;
    InitThreadRun(t, arg);
    MakeReady(t, 0);
    return 1;
}

s32 KThread_Delete(s32 id)
{
    KThread *t = ThreadGet(id);
    if (!t || t == sCur) {
        return KE_UNKNOWN_THID;
    }
    if (t->status != THS_DORMANT) {
        return KE_NOT_DORMANT;
    }
    if (t->fiber) {
        KFiber_Free(t->fiber);
        t->fiber = 0;
    }
    t->used = 0;
    return id;
}

s32 KThread_Start(s32 id, void *arg)
{
    KThread *t = ThreadGet(id);
    if (!t || t == sCur) {
        return KE_UNKNOWN_THID;
    }
    if (t->status != THS_DORMANT) {
        return KE_NOT_DORMANT;
    }
    t->prio = t->initPrio;
    InitThreadRun(t, arg);
    MakeReady(t, 0);
    Reschedule();
    return id;
}

s32 KThread_Terminate(s32 id)
{
    KThread *t = ThreadGet(id);
    if (!t || t == sCur) {
        return KE_ILLEGAL_THID;
    }
    if (t->status == THS_DORMANT) {
        return KE_DORMANT;
    }
    t->status = THS_DORMANT;
    t->waitType = WAIT_NONE;
    if (t->fiber) {
        KFiber_Free(t->fiber);
        t->fiber = 0;
    }
    return t->id;
}

void KThread_SetDispatch(int enabled)
{
    sDispatchDisabled = !enabled;
    if (enabled) {
        Reschedule();
    }
}

s32 KThread_ChangePriority(s32 id, s32 prio)
{
    KThread *t = ThreadGet(id);
    s32 old;
    if (!t) {
        return KE_UNKNOWN_THID;
    }
    old = t->prio;
    t->prio = prio;
    if (t->status == THS_READY) {
        t->seq = sSeqTail++;
    }
    Reschedule();
    return old;
}

s32 KThread_RotateReadyQueue(s32 prio)
{
    if (sCur && sCur->prio == prio && !sInInterrupt) {
        KThread *next = PickNext();
        if (next && next->prio == prio) {
            MakeReady(sCur, 0);
            Reschedule();
        }
    } else {
        KThread *first = 0;
        int i;
        for (i = 1; i < MAX_THREADS; i++) {
            KThread *t = &sThreads[i];
            if (t->used && t->status == THS_READY && t->prio == prio && (!first || t->seq < first->seq)) {
                first = t;
            }
        }
        if (first) {
            first->seq = sSeqTail++;
        }
        if (sCur && sCur->prio == prio) {
            sNeedResched = 1;
        }
    }
    return prio;
}

s32 KThread_ReleaseWait(s32 id)
{
    KThread *t = ThreadGet(id);
    if (!t) {
        return KE_UNKNOWN_THID;
    }
    if (!(t->status & THS_WAIT)) {
        return KE_NOT_WAIT;
    }
    ThreadRelease(t, KE_RELEASE_WAIT);
    Reschedule();
    return t->id;
}

s32 KThread_GetId(void)
{
    return sCur ? sCur->id : 0;
}

s32 KThread_Refer(s32 id, u32 *info)
{
    KThread *t = ThreadGet(id);
    if (!t) {
        return KE_UNKNOWN_THID;
    }
    if (info) {
        info[0] = (u32)t->status;
        info[1] = (u32)t->entry;
        info[2] = t->stack;
        info[3] = t->stackSize;
        info[4] = t->gp;
        info[5] = (u32)t->initPrio;
        info[6] = (u32)t->prio;
        info[7] = t->attr;
        info[8] = t->option;
        info[9] = (u32)t->waitType;
        info[10] = (u32)t->waitId;
        info[11] = (u32)t->wakeupCount;
    }
    return t->status;
}

s32 KThread_Sleep(void)
{
    s32 id, r;
    if (sCur && sCur->wakeupCount > 0) {
        sCur->wakeupCount--;
        return sCur->id;
    }
    id = sCur ? sCur->id : 0;
    r = ThreadWait(WAIT_SLEEP, 0);
    return r < 0 ? r : id;
}

s32 KThread_Wakeup(s32 id)
{
    KThread *t = ThreadGet(id);
    if (!t || t == sCur) {
        return KE_ILLEGAL_THID;
    }
    if (t->status == THS_DORMANT) {
        return KE_DORMANT;
    }
    if ((t->status & THS_WAIT) && t->waitType == WAIT_SLEEP) {
        ThreadRelease(t, 0);
    } else {
        t->wakeupCount++;
    }
    Reschedule();
    return t->id;
}

s32 KThread_CancelWakeup(s32 id)
{
    KThread *t = ThreadGet(id);
    s32 n;
    if (!t) {
        return KE_UNKNOWN_THID;
    }
    n = t->wakeupCount;
    t->wakeupCount = 0;
    return n;
}

s32 KThread_Suspend(s32 id)
{
    KThread *t = ThreadGet(id);
    if (!t || t == sCur) {
        return KE_ILLEGAL_THID;
    }
    if (t->status == THS_READY) {
        t->status = THS_SUSPEND;
    } else if (t->status == THS_WAIT) {
        t->status = THS_WAITSUSPEND;
    }
    return t->id;
}

s32 KThread_Resume(s32 id)
{
    KThread *t = ThreadGet(id);
    if (!t || t == sCur) {
        return KE_ILLEGAL_THID;
    }
    if (t->status == THS_SUSPEND) {
        MakeReady(t, 0);
    } else if (t->status == THS_WAITSUSPEND) {
        t->status = THS_WAIT;
    } else {
        return KE_NOT_SUSPEND;
    }
    Reschedule();
    return t->id;
}

/* ------------------------------------------------------------- semaphores */

s32 Kern_SemaCreate(s32 initCount, s32 maxCount, u32 attr, u32 option)
{
    int i;
    for (i = 1; i < MAX_SEMAS; i++) {
        KSema *s = &sSemas[i];
        if (s->used) {
            continue;
        }
        s->used = 1;
        s->max = maxCount;
        s->init = s->count = initCount;
        s->attr = attr;
        s->option = option;
        if (g_kern_log) {
            KLog("kernel: CreateSema %d (init %d, max %d) at %llu", i, initCount, maxCount, Kern_Now());
        }
        return i;
    }
    return KE_ERROR;
}

static int SemaWaiters(s32 sid)
{
    int i, n = 0;
    for (i = 1; i < MAX_THREADS; i++) {
        KThread *t = &sThreads[i];
        if (t->used && (t->status & THS_WAIT) && t->waitType == WAIT_SEMA && t->waitId == sid) {
            n++;
        }
    }
    return n;
}

s32 Kern_SemaDelete(s32 id)
{
    KSema *s = SemaGet(id);
    int i;
    if (!s) {
        return KE_UNKNOWN_SEMID;
    }
    for (i = 1; i < MAX_THREADS; i++) {
        KThread *t = &sThreads[i];
        if (t->used && (t->status & THS_WAIT) && t->waitType == WAIT_SEMA && t->waitId == id) {
            ThreadRelease(t, KE_WAIT_DELETE);
        }
    }
    s->used = 0;
    Reschedule();
    return id;
}

/* A signal readies the waiter that waited first, whatever its priority; with no waiter the count grows (8). */
s32 Kern_SemaSignal(s32 id)
{
    KSema *s = SemaGet(id);
    KThread *best = 0;
    int i;
    if (!s) {
        return KE_UNKNOWN_SEMID;
    }
    for (i = 1; i < MAX_THREADS; i++) {
        KThread *t = &sThreads[i];
        if (t->used && (t->status & THS_WAIT) && t->waitType == WAIT_SEMA && t->waitId == id) {
            if (!best || t->waitSeq < best->waitSeq) {
                best = t;
            }
        }
    }
    if (best) {
        ThreadRelease(best, id);
    } else {
        s->count++;
    }
    Reschedule();
    return id;
}

s32 Kern_SemaWait(s32 id)
{
    KSema *s = SemaGet(id);
    if (!s) {
        return KE_UNKNOWN_SEMID;
    }
    if (s->count > 0) {
        s->count--;
        return id;
    }
    return ThreadWait(WAIT_SEMA, id);
}

s32 Kern_SemaPoll(s32 id)
{
    KSema *s = SemaGet(id);
    if (!s) {
        return KE_UNKNOWN_SEMID;
    }
    if (s->count > 0) {
        s->count--;
        return id;
    }
    return KE_SEMA_ZERO;
}

s32 KThread_SemaRefer(s32 id, s32 *info)
{
    KSema *s = SemaGet(id);
    if (!s) {
        return KE_UNKNOWN_SEMID;
    }
    info[0] = s->count;
    info[1] = s->max;
    info[2] = s->init;
    info[3] = SemaWaiters(id);
    info[4] = (s32)s->attr;
    info[5] = (s32)s->option;
    return id;
}

/* --------------------------------------------------------------- the loop */

static void IdleMain(void *arg)
{
    (void)arg;
    for (;;) {
        KThread *next;
        FreeZombie();
        next = PickNext();
        if (next) {
            sCur = next;
            next->status = THS_RUN;
            EnsureFiber(next);
            KFiber_Switch(sIdleFiber, next->fiber);
            continue;
        }
        sCur = 0;
        if (KSched_Settled()) {
            KSched_IdleWait();
        }
        KSched_Deliver();
    }
}

/* The run loop (8): on a fiber of its own, from the low pool like the threads' (a handler the idle loop runs
 * keeps its locals there). Never returns. */
void KThread_RunIdle(void)
{
    sIdleFiber = KFiber_Create(IdleMain, 0);
    KFiber_Switch(KFiber_Main(), sIdleFiber);
    Pal_Fatal("kernel: the idle loop returned");
}
