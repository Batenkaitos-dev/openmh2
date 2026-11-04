/*
 * libkernl's timer and alarm module, on Timer 2: its set-up at
 * the C library's start-up, the system time, the alarms (DelayThread's) and
 * its Timer 2 interrupt handler. Written from the module's behaviour as
 * port-libraries.md records it (the project's own model, 
 * libkernl_alarm.c, written from observation) and the register writes the
 * reference shows (gdb on the original game, the frame clock, the boot
 * route; the notes, session 1):
 *  - the set-up: AddIntcHandler(11, handler, 0) (the first handler: id 1),
 *    Timer 2's COUNT 0, COMP 0xFFFF, MODE 0xF82 (bus clock / 256, counting,
 *    the compare and overflow interrupts, both flags cleared), then
 *    EnableIntc(11);
 *  - an alarm's set: COMP 0, MODE 0x382 (the compare with no alarm active
 *    yet), then at its start MODE 0x382, COMP target (the target in Timer 2's
 *    clocks: the count + usec x 576 / 1000);
 *  - the handler on an overflow: MODE 0xB82, COMP 0, MODE 0x382; on a
 *    compare match with the alarm due: its callback, then COMP 0, MODE 0x782.
 *
 * The state is the kernel's own (as the original's runtime keeps it, not in
 * game memory); ids are the numbers the PS2's module gives (an alarm's
 * address in the module's array << 4 | its sequence bits, a callback
 * record's << 4 | the alarm id's low 8 bits), so a caller sees the same
 * values. The module's DIntr and EIntr are a library's (Kern_LibDIntr,
 * Kern_LibEIntr: no delivery point).
 */
#include "kernel/kern_int.h"

#define T2_COUNT 0x10001000u
#define T2_MODE 0x10001010u
#define T2_COMP 0x10001020u
#define OVFF 0x800u
#define EQUF 0x400u

#define AL_BASE 0x001CE680u       /* the module's 128 alarms of 0x40 bytes (their ids' addresses) */
#define AL_N 128
#define REC_BASE 0x001D0680u      /* its 64 callback records of 0x10 bytes */
#define REC_N 64
#define MIN_CLOCKS 0x7333u        /* 200 us of bus clocks: the compare's least distance */
#define MIN_RELINK 0x3999u        /* 100 us: the least a callback's answer moves its alarm */
#define ERR_NOMEM 0x80008005u
#define ERR_ID 0x80008002u

typedef struct KAlarmRec {
    int next;                     /* the free list (an index, -1 none) */
    u32 id;                       /* its alarm's id, 0 when free */
    KAlarmFn fn;
    void *arg;
} KAlarmRec;

typedef struct KAlarmEntry {
    int next, prev;               /* the free or active list (an index, -1 none) */
    u32 bits;                     /* the id's sequence bits */
    u32 state;                    /* 1 started (its time taken), 2 set (its clocks and record) */
    u64 time, clocks;             /* its start, its clocks (the module's word 0x18, which only its allocator
                                   * writes (0), is left out of the target) */
    int rec;                      /* its callback record (every alarm the game reaches is set with one) */
} KAlarmEntry;

static struct {
    u64 ovf;                      /* Timer 2's overflows the handler has counted */
    u32 seq, inuse;
    int free, active, recFree;
    KAlarmEntry al[AL_N];
    KAlarmRec rec[REC_N];
} sM;

static const int SH_TIME[4] = {0, 4, 8, 16};   /* the time: Timer 2's clock as a shift of bus clocks */
static const int SH_CMP[4] = {0, 4, 8, 12};    /* the compare's (12 for the H-blank) */

static u32 AlarmId(int i)
{
    return (AL_BASE + 0x40u * (u32)i) << 4 | sM.al[i].bits;
}

static u64 Key(int i)
{
    return sM.al[i].time + sM.al[i].clocks;
}

/* The time in bus clocks: Timer 2's count and mode; with the overflow flag set, the count read again and
 * one overflow more (not kept). */
static u64 TimeNow(void)
{
    u32 c = Kern_TimerRead(T2_COUNT), m = Kern_TimerRead(T2_MODE);
    u64 ovf = sM.ovf;
    if (m & OVFF) {
        c = Kern_TimerRead(T2_COUNT);
        ovf++;
    }
    return ((ovf << 16) | c) << SH_TIME[m & 3];
}

/* Timer 2's compare for the earliest alarm: from the head, the last of those each within 200 us after the
 * one before; a target less than 200 us ahead (or behind) gets the count + 200 us. */
static void Compare(u64 now)
{
    u32 m = Kern_TimerRead(T2_MODE);
    int a, cs;
    u64 target;
    if (sM.active < 0) {
        Kern_TimerWrite(T2_COMP, 0);
        Kern_TimerWrite(T2_MODE, m & ~OVFF);
        return;
    }
    a = sM.active;
    while (sM.al[a].next >= 0 && Key(sM.al[a].next) - Key(a) < MIN_CLOCKS) {   /* unsigned */
        a = sM.al[a].next;
    }
    target = Key(a);
    cs = SH_CMP[m & 3];
    if ((s64)(target - now) < (s64)MIN_CLOCKS) {
        u32 c = Kern_TimerRead(T2_COUNT);
        Kern_TimerWrite(T2_COMP, c + (MIN_CLOCKS >> cs));
        Kern_TimerWrite(T2_MODE, m & ~OVFF);
    } else {
        Kern_TimerWrite(T2_MODE, m & ~OVFF);
        Kern_TimerWrite(T2_COMP, (u32)(target >> cs));
    }
}

/* Into the active list in order of target (unsigned), after any of the same target. */
static void Link(int i)
{
    int p = sM.active, prev = -1;
    u64 t = Key(i);
    while (p >= 0 && !(t < Key(p))) {
        prev = p;
        p = sM.al[p].next;
    }
    sM.al[i].next = p;
    sM.al[i].prev = prev;
    if (prev >= 0) {
        sM.al[prev].next = i;
    } else {
        sM.active = i;
    }
    if (p >= 0) {
        sM.al[p].prev = i;
    }
}

static void Unlink(int i)
{
    KAlarmEntry *a = &sM.al[i];
    if (a->prev >= 0) {
        sM.al[a->prev].next = a->next;
    } else {
        sM.active = a->next;
    }
    if (a->next >= 0) {
        sM.al[a->next].prev = a->prev;
    }
    a->prev = -1;
}

/* An alarm from the free list (its id), or ERR_NOMEM; with interrupts off. */
static u32 Alloc(void)
{
    int s = Kern_LibDIntr(), i;
    u32 id = ERR_NOMEM;
    if (sM.free >= 0) {
        i = sM.free;
        sM.free = sM.al[i].next;
        sM.seq++;
        sM.inuse++;
        sM.al[i].bits = ((sM.seq << 1) | 1) & 0x3FF;
        sM.al[i].state = 0;
        sM.al[i].rec = -1;
        id = AlarmId(i);
    }
    if (s) {
        Kern_LibEIntr();
    }
    return id;
}

/* The alarm of an id, or -1. */
static int ById(u32 id)
{
    u32 g = (id >> 4) & ~0x3Fu;
    int i;
    if (g - AL_BASE >= AL_N * 0x40u) {
        return -1;
    }
    i = (int)((g - AL_BASE) / 0x40u);
    return sM.al[i].bits == (id & 0x3FF) ? i : -1;
}

/* Its clocks and record set, linked again if started, the compare updated (0x11F058's). */
static u32 Set(u32 id, u64 clocks, int rec)
{
    int s = Kern_LibDIntr(), i = ById(id);
    u32 r = ERR_ID;
    if (i >= 0) {
        if (sM.al[i].state & 2) {
            Unlink(i);
        }
        sM.al[i].clocks = clocks;
        sM.al[i].rec = rec;
        sM.al[i].state |= 2;
        if (sM.al[i].state & 1) {
            Link(i);
        }
        Compare(TimeNow());
        r = 0;
    }
    if (s) {
        Kern_LibEIntr();
    }
    return r;
}

/* Started: its time taken, linked if set, the compare updated (0x11EBD0's). */
static u32 Start(u32 id)
{
    int s = Kern_LibDIntr(), i = ById(id);
    u32 r = ERR_ID;
    if (i >= 0) {
        r = 1;
        if (!(sM.al[i].state & 1)) {
            u64 now = TimeNow();
            sM.al[i].time = now;
            sM.al[i].state |= 1;
            if (sM.al[i].state & 2) {
                Link(i);
                Compare(now);
            }
            r = 0;
        }
    }
    if (s) {
        Kern_LibEIntr();
    }
    return r;
}

/* An alarm `clocks` bus clocks from now whose callback fn(id, clocks, elapsed, arg) runs from Timer 2's
 * interrupt (the module's 0x11F330): the record's id (> 0), or ERR_NOMEM with no free record or alarm. */
s32 KAlarm_Set(u64 clocks, KAlarmFn fn, void *arg)
{
    int s = Kern_LibDIntr(), rg;
    u32 id;
    s32 r;
    if (sM.recFree < 0) {
        r = (s32)ERR_NOMEM;
    } else {
        id = Alloc();
        if ((s32)id < 0) {
            sM.rec[sM.recFree].id = 0;
            r = (s32)id;
        } else {
            rg = sM.recFree;
            sM.recFree = sM.rec[rg].next;
            sM.rec[rg].id = id;
            sM.rec[rg].fn = fn;
            sM.rec[rg].arg = arg;
            Set(id, clocks, rg);
            Start(id);
            r = (s32)((REC_BASE + 0x10u * (u32)rg) << 4 | (id & 0xFF));
        }
    }
    if (s) {
        Kern_LibEIntr();
    }
    return r;
}

/* A record's dispatch: its callback; 0 back frees the record and gives -1 (the alarm freed), else what it gave
 * (the alarm moved on by that many clocks). */
static s64 Dispatch(int i, u64 now)
{
    KAlarmEntry *a = &sM.al[i];
    KAlarmRec *r = &sM.rec[a->rec];
    u32 rid = (REC_BASE + 0x10u * (u32)a->rec) << 4 | (AlarmId(i) & 0xFF) | 1;
    s64 v = r->fn(rid, a->clocks, now - a->time, r->arg);
    if (v == 0) {
        r->id = 0;
        r->next = sM.recFree;
        sM.recFree = a->rec;
        return -1;
    }
    return v;
}

/* The handler's time: an overflow flag is cleared (the mode written without the compare flag) and counted. */
static u64 HTime(void)
{
    u32 c = Kern_TimerRead(T2_COUNT), m = Kern_TimerRead(T2_MODE);
    if (m & OVFF) {
        Kern_TimerWrite(T2_MODE, m & ~EQUF);
        sM.ovf++;
        c = Kern_TimerRead(T2_COUNT);
    }
    return ((sM.ovf << 16) | c) << SH_CMP[m & 3];
}

/* Timer 2's INTC handler (0x11E550): on a compare match, the due alarms from the head, each one's next taken
 * before its dispatch; then the time, the compare, the time again. Its closing `ei` is a no-op here: the
 * handler runs only when the interrupted context had interrupts on. */
static s32 Tim2Handler(s32 cause, void *arg)
{
    (void)cause;
    (void)arg;
    if (Kern_TimerRead(T2_MODE) & EQUF) {
        int i = sM.active;
        while (i >= 0) {
            u64 now = HTime();
            int next;
            s64 r;
            if (Key(i) > now) {
                break;
            }
            next = sM.al[i].next;
            Unlink(i);
            r = Dispatch(i, now);
            if (r == -1) {
                sM.al[i].next = sM.free;
                sM.free = i;
                sM.inuse--;
                sM.al[i].bits = 0;
                sM.al[i].state = 0;
            } else if (r == 0) {
                sM.al[i].state &= ~2u;
            } else {
                sM.al[i].clocks += (u64)r < MIN_RELINK ? MIN_RELINK : (u64)r;
                Link(i);
            }
            i = next;
        }
    }
    Compare(HTime());
    HTime();
    return 0;
}

void KAlarm_Init(void)
{
    int i;
    __builtin_memset(&sM, 0, sizeof(sM));
    for (i = 0; i < AL_N; i++) {
        sM.al[i].next = i + 1 < AL_N ? i + 1 : -1;
        sM.al[i].prev = -1;
        sM.al[i].rec = -1;
    }
    for (i = 0; i < REC_N; i++) {
        sM.rec[i].next = i + 1 < REC_N ? i + 1 : -1;
    }
    sM.free = 0;
    sM.active = -1;
    sM.recFree = 0;
    KThread_AddHandler(0, KERN_INTC_TIM2, 0, 0, 0, 1, Tim2Handler);
    Kern_TimerWrite(T2_COUNT, 0);
    Kern_TimerWrite(T2_COMP, 0xFFFFu);
    Kern_TimerWrite(T2_MODE, 0xF82u);
    KThread_EnableCause(0, KERN_INTC_TIM2, 1);
}

/* The system time in bus clocks (GetTimerSystemTime): the time with interrupts off. */
u64 KAlarm_SystemTime(void)
{
    int s = Kern_LibDIntr();
    u64 t = TimeNow();
    if (s) {
        Kern_LibEIntr();
    }
    return t;
}
