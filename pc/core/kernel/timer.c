/*
 * The EE timers and the INTC and D_STAT registers. A timer's count is read
 * from guest time; an event is queued only for an interrupt it can raise.
 */
#include "kernel/kern_int.h"
#include "kernel/ksif.h"
#include "kernel/klog.h"

typedef struct KTimer {
    u32 mode, comp, hold;
    u32 countAtBase;
    u64 baseNs;
    u32 event;
} KTimer;

static KTimer sTimers[4];

void KTimer_Reset(void)
{
    __builtin_memset(sTimers, 0, sizeof(sTimers));
    sTimers[3].mode = 0x100;
}

static u64 TimerRate(const KTimer *t)
{
    switch (t->mode & 3) {
    case 0:
        return KERN_BUS_HZ;
    case 1:
        return KERN_BUS_HZ / 16;
    case 2:
        return KERN_BUS_HZ / 256;
    default:
        return 15734;     /* the H-blank */
    }
}

static u32 TimerCount(const KTimer *t, u64 now)
{
    u64 ticks;
    if (!(t->mode & 0x80)) {
        return t->countAtBase;
    }
    ticks = (now - t->baseNs) * TimerRate(t) / 1000000000ull;
    return (u32)((t->countAtBase + ticks) & 0xFFFF);
}

static void TimerSchedule(int n);

/* A boundary: the compare match or the overflow. A flag raises the interrupt only on its 0 to 1 transition. */
static void TimerEvent(u64 now, u32 n)
{
    KTimer *t = &sTimers[n];
    u64 rate = TimerRate(t), c;
    int irq = 0;
    t->event = 0;
    c = t->countAtBase + (now - t->baseNs) * rate / 1000000000ull;
    if ((t->mode & 0x100) && t->countAtBase < t->comp && c >= t->comp) {
        if (!(t->mode & 0x400)) {
            t->mode |= 0x400;
            irq = 1;
        }
        if (t->mode & 0x40) {     /* ZRET */
            c -= t->comp;
        }
    }
    if (c > 0xFFFF) {
        if ((t->mode & 0x200) && !(t->mode & 0x800)) {
            t->mode |= 0x800;
            irq = 1;
        }
        c &= 0xFFFF;
    }
    t->countAtBase = (u32)c;
    t->baseNs = now;
    if (irq) {
        Kern_RaiseIntc(KERN_INTC_TIM0 + (int)n);
    }
    TimerSchedule((int)n);
}

static void TimerSchedule(int n)
{
    KTimer *t = &sTimers[n];
    u64 rate, target, when;
    u32 c;
    if (t->event) {
        Kern_EventCancel(t->event);
        t->event = 0;
    }
    if (!(t->mode & 0x80) || !(t->mode & 0x300)) {
        return;
    }
    rate = TimerRate(t);
    c = t->countAtBase;
    target = ((t->mode & 0x100) && c < t->comp) ? t->comp : 0x10000;
    when = t->baseNs + ((target - c) * 1000000000ull + rate - 1) / rate;
    t->event = Kern_EventAdd(when, TimerEvent, (u32)n);
}

u32 Kern_TimerRead(u32 addr)
{
    KTimer *t = &sTimers[(addr >> 11) & 3];
    switch (addr & 0xF0) {
    case 0x00:
        return TimerCount(t, Kern_Now());
    case 0x10:
        return t->mode;
    case 0x20:
        return t->comp;
    case 0x30:
        return t->hold;
    }
    return 0;
}

void Kern_TimerWrite(u32 addr, u32 v)
{
    int n = (int)((addr >> 11) & 3);
    KTimer *t = &sTimers[n];
    u64 now = Kern_Now();
    t->countAtBase = TimerCount(t, now);
    t->baseNs = now;
    switch (addr & 0xF0) {
    case 0x00:
        t->countAtBase = v & 0xFFFF;
        break;
    case 0x10:
        t->mode = (t->mode & 0xC00u & ~(v & 0xC00u)) | (v & 0x3FFu);
        break;
    case 0x20:
        t->comp = v & 0xFFFF;
        t->mode &= ~0x400u;
        break;
    case 0x30:
        t->hold = v & 0xFFFF;
        break;
    }
    TimerSchedule(n);
}

u32 Kern_IntcRead(u32 addr)
{
    return addr == 0x1000F010u ? KThread_IntcMask() : KThread_IntcStat();
}

void Kern_IntcWrite(u32 addr, u32 value)
{
    if (addr == 0x1000F010u) {
        KThread_SetIntcMask(KThread_IntcMask() ^ value);     /* a 1 toggles */
    } else {
        KThread_SetIntcStat(KThread_IntcStat() & ~value);    /* a 1 clears */
    }
}

u32 Kern_DmacStatRead(void)
{
    return KThread_DmacStat();
}

void Kern_DmacStatWrite(u32 value)
{
    u32 s = KThread_DmacStat();
    s &= ~(value & 0xFFFFu);          /* CIS: a 1 clears */
    s ^= value & 0xFFFF0000u;         /* CIM: a 1 toggles */
    KThread_SetDmacStat(s);
}

/* The SIO's TXFIFO: the console, a log line per newline (the original's hw.c). */
static char sSioLine[256];
static u32 sSioLen;

static void SioPut(u32 v)
{
    char c = (char)v;
    if (c == '\n' || sSioLen == sizeof(sSioLine) - 1) {
        sSioLine[sSioLen] = 0;
        KLog("sio: %s", sSioLine);
        sSioLen = 0;
    } else if (c != '\r') {
        sSioLine[sSioLen++] = c;
    }
}

u32 Kern_EeRegRead32(u32 addr)
{
    if (addr >= 0x10000000u && addr < 0x10002000u) {
        return Kern_TimerRead(addr);
    }
    if (addr == 0x1000F000u || addr == 0x1000F010u) {
        return Kern_IntcRead(addr);
    }
    if (addr >= 0x1000F200u && addr < 0x1000F300u) {
        return KSif_RegRead(addr);
    }
    return 0;
}

void Kern_EeRegWrite32(u32 addr, u32 v)
{
    if (addr >= 0x10000000u && addr < 0x10002000u) {
        Kern_TimerWrite(addr, v);
    } else if (addr == 0x1000F000u || addr == 0x1000F010u) {
        Kern_IntcWrite(addr, v);
    } else if (addr == 0x1000F180u) {
        SioPut(v);
    } else if (addr >= 0x1000F200u && addr < 0x1000F300u) {
        KSif_RegWrite(addr, v);
    }
}
