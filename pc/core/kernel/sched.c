/*
 * The frame clock: guest
 * time, the event queue, the jump, the delivery point, poll events and
 * library waits. Guest time is a variable that only the jump moves; nothing
 * reads a host clock (R6). The original game's sched.c and
 * kernel.c (service_frame, recomp_service_events) are the same rules.
 */
#include "kernel/kern_int.h"
#include "kernel/khost.h"
#include "pal_core.h"

#define MAX_EVENTS 256
#define SERVICE_ROUNDS 100000

typedef struct KEvent {
    u64 when;
    KernEventFn fn;
    u32 param;
    u32 id;
} KEvent;

static KEvent sEvents[MAX_EVENTS];
static int sNumEvents;
static u32 sNextId;
static u64 sNow;
int g_kern_fseen;

#define DEFAULT_EPOCH 1140058800ull     /* 2006-02-16 03:00:00 UTC (the specification, 1.2) */
#define JST_OFFSET (9 * 3600)

/* The calendar's start and zone: the fixed epoch in JST
 * (checks and deterministic runs), the host's local date and zone (play:
 * main.c's --local-date). Set before the
 * kernel starts; KSched_Reset keeps them. */
static u64 sCalendarEpoch = DEFAULT_EPOCH;
static s32 sCalendarOffset = JST_OFFSET;

void KSched_Reset(void)
{
    sNumEvents = 0;
    sNextId = 1;
    sNow = 0;
    g_kern_fseen = 0;
}

u64 Kern_Now(void)
{
    return sNow;
}

u64 Kern_CalendarSeconds(void)
{
    return sCalendarEpoch + sNow / 1000000000ull;
}

void Kern_SetCalendarEpoch(u64 seconds)
{
    sCalendarEpoch = seconds;
}

s32 Kern_CalendarOffset(void)
{
    return sCalendarOffset;
}

void Kern_SetCalendar(u64 epochSeconds, s32 utcOffsetSeconds)
{
    sCalendarEpoch = epochSeconds;
    sCalendarOffset = utcOffsetSeconds;
}

void Kern_CalendarStart(u64 *epochSeconds, s32 *utcOffsetSeconds)
{
    *epochSeconds = sCalendarEpoch;
    *utcOffsetSeconds = sCalendarOffset;
}

/* Play: the host's date and zone, read once, here: the only host time the
 * game sees, and verify records it. */
void Kern_SetCalendarLocal(void)
{
    s64 epoch, offset;
    HostK_LocalDate(&epoch, &offset);
    Kern_SetCalendar((u64)epoch, (s32)offset);
}


/* 2: inserted after every event whose time is not later. */
u32 Kern_EventAdd(u64 when, KernEventFn fn, u32 param)
{
    int i;
    u32 id;
    if (sNumEvents == MAX_EVENTS) {
        Pal_Fatal("kernel: the event queue is full (256 events)");
    }
    i = sNumEvents++;
    while (i > 0 && sEvents[i - 1].when > when) {
        sEvents[i] = sEvents[i - 1];
        i--;
    }
    id = sNextId++;
    if (!sNextId) {
        sNextId = 1;
    }
    sEvents[i].when = when;
    sEvents[i].fn = fn;
    sEvents[i].param = param;
    sEvents[i].id = id;
    return id;
}

void Kern_EventCancel(u32 id)
{
    int i, j;
    for (i = 0; i < sNumEvents; i++) {
        if (sEvents[i].id == id) {
            for (j = i; j < sNumEvents - 1; j++) {
                sEvents[j] = sEvents[j + 1];
            }
            sNumEvents--;
            return;
        }
    }
}

int KSched_HasEvents(void)
{
    return sNumEvents > 0;
}

/* While the first event is due, take it off the queue and fire it with the current time. Every event that
 * fires unparks the IOP (7). Returns the time to the next event, or ~0 with none. */
u64 KSched_RunDue(void)
{
    for (;;) {
        KEvent e;
        int j;
        if (!sNumEvents) {
            return ~0ull;
        }
        if (sEvents[0].when > sNow) {
            return sEvents[0].when - sNow;
        }
        e = sEvents[0];
        for (j = 0; j < sNumEvents - 1; j++) {
            sEvents[j] = sEvents[j + 1];
        }
        sNumEvents--;
        KSif_UnparkOnEvent();
        KThread_EnterEvent();
        e.fn(sNow, e.param);
        KThread_LeaveEvent();
    }
}

/* 3: nothing can still happen at this instant. Inside a handler the instant is always settled. */
int KSched_Settled(void)
{
    if (Kern_InHandler()) {
        return 1;
    }
    return !KThread_InterruptPending() && !KSif_IopBusy() && !KSif_HasWork();
}

/* The jump (1): the due events fire; if the instant is then settled and an event is queued, time goes to the
 * first event. Its events fire at the delivery point that follows. */
void KSched_Jump(void)
{
    u64 wait = KSched_RunDue();
    if (wait != ~0ull && wait && KSched_Settled()) {
        sNow += wait;
    }
}

/* A delivery point (4): the due events, then rounds of the IOP's catch-up, one SIF packet and the
 * interrupts until a round finds nothing to do, then the requested thread switch. Nothing in a handler. */
void KSched_Deliver(void)
{
    int round;
    if (Kern_InHandler()) {
        return;
    }
    KSched_RunDue();
    for (round = 0;; round++) {
        int iop, sent, took;
        if (round >= SERVICE_ROUNDS) {
            Pal_Fatal("kernel: the IOP and the interrupts did not settle in 100,000 rounds at a delivery point");
        }
        iop = KSif_IopCatchUp();
        sent = KSif_Pump();
        took = KThread_DeliverInterrupts();
        if (iop != 1 && !sent && !took) {
            break;
        }
    }
    KThread_ReschedIfAsked();
}

/* The idle loop's time wait (8): clears fseen; fires the due events; if the instant is settled, time goes to
 * the next event and its events fire. With no event queued the run hangs: fatal. */
void KSched_IdleWait(void)
{
    u64 wait;
    g_kern_fseen = 0;
    wait = KSched_RunDue();
    if (wait == 0 || !KSched_Settled()) {
        return;
    }
    if (wait == ~0ull) {
        Pal_Fatal("kernel: every thread waits and no event is queued (a hang)");
    }
    sNow += wait;
    KSched_RunDue();
}

/* 5: a "not ready" of a listed spin. The first after a wait only notes it; the next is a poll event: the jump,
 * then a delivery point. */
void Pal_Poll(void)
{
    if (!g_kern_fseen) {
        g_kern_fseen = 1;
        return;
    }
    KSched_Jump();
    KSched_Deliver();
}

/* PAL_SYNC_EI (pal.h): the game's `sync.l; ei`, which ends its interrupt handlers. As the original's ei of
 * the game's code: the context's enable set (in a handler only what the handler reads back), then a delivery
 * point when the interrupts are on (never in a handler: the interrupts raised meanwhile wait for its end). */
void Pal_SyncEi(void)
{
    KThread_SetEie(1);
    if (KThread_InterruptsEnabled()) {
        KSched_Deliver();
    }
}

/* 6.2: the arrival at a library wait's loop: one delivery point, nothing else. */
void Kern_LibWaitArrive(void)
{
    KSched_Deliver();
}

/* 6.2: one step of a library wait: a w count, fseen cleared, the jump, a delivery point. */
void Kern_LibWaitStep(void)
{
    g_kern_fseen = 0;
    KSched_Jump();
    KSched_Deliver();
}
