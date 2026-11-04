/* The IOP's kernel services for the sound modules (the interpreter is not in the core).
 * The original's iop_lle.c, adapted to the core, without its R3000A interpreter: the
 * game's sound modules (Sony's libsd, the Capcom Sound Engine and CRI's ADX driver) are native code (pc/core/api/libsd.c,
 * pc/core/cse), and the kernel functions they import are native handlers here, called by name
 * (IopNative_Call, IopKern_Call). Timing:.
 *
 * - The modules are loaded by iop_mod.c. A module's code is a native function at an IOP address (IopNative_Map).
 * - IOP threads run on the kernel's raw fibers (Kern_FiberCreate, kern.h): a thread's entry is the native function at the
 *   address the thread was created with. A strict-priority scheduler (IopKern_Run) is called from the EE side whenever there
 *   is IOP work: a ready thread or a pending interrupt. It runs until the IOP is idle or a budget of calls runs out.
 * - The time model: no time slices. A thread
 *   runs until it waits, or until a call it makes readies a higher priority (maybe_preempt). Pending interrupts and
 *   hard-timer expiries are delivered when the scheduler runs, and at the return of each kernel call and each libsd call of
 *   a thread whose interrupts are on (native_call_point: the thread gives way, first in its priority, as after an
 *   interrupt). A busy loop is found by counting those calls: a run that makes RUN_CALLS of them with a thread still ready
 *   ends, and the IOP is parked.
 * - Interrupt handlers (intrman) and hard-timer handlers (timrman) run in an interrupt context on the scheduler's fiber,
 *   when the thread about to run has interrupts enabled.
 * - RPC servers (sifcmd) are registered with the kernel's SIF (SndSys_RpcRegister), which hands the EE's calls here
 *   (IopKern_RpcCall). The server thread's sceSifRpcLoop runs the server function and hands the reply back (SndSys_RpcDone).
 *
 * What changed from the original game: the names; the frame clock is the only clock (the original's other clocks and its RPC
 * history for play tests are left out); the RPC queue's entries come from a fixed pool; and the kernel functions only
 * guest code called (the string, printf and sysmem functions, loadcore, the polls and the deletes) are not here, nor are the
 * seven that no native module names (CreateSema, SignalSema, iSignalSema, WaitSema, DeleteThread, TerminateThread and
 * sceSifCallRpc: the core's routes and the owner's sessions never call them; a native module that needs one adds it, and a
 * call by a name not in the table is fatal and says so).
 *
 * Kernel function numbers and structures follow the IOP ABI as documented by ps2sdk's headers (iop/system). */
#include "snd/snd_iop.h"

#define MAX_ITHREADS 64
#define MAX_EVFS 64
#define MAX_MBXS 32
#define MAX_HTIMERS 8
#define RUN_CALLS 100000u   /* per run: kernel and libsd calls, and scheduler turns (the busy-loop rule) */
#define IOP_CLOCK_HZ 36864000ull

enum { THS_RUN = 1, THS_READY = 2, THS_WAIT = 4, THS_SUSPEND = 8, THS_DORMANT = 0x10 };
/* The numbers are the info log's "wait" column: 3 was the semaphores' (no native module creates one). */
enum { TSW_NONE, TSW_SLEEP, TSW_DELAY, TSW_EVENTFLAG = 4, TSW_MBX, TSW_VBLANK, TSW_RPC };

typedef struct IThread {
    int used;
    u32 attr, option, entry, stack, stack_size, gp;
    int prio, init_prio;
    int status;
    int wait_type, wait_id;
    u32 wait_bits, wait_mode, wait_res;   /* event flags */
    u32 wait_msgvar;                      /* mailbox */
    int wait_result;
    int wakeups;
    u32 delay_event;
    u64 seq;
    int ie;                               /* interrupts enabled (CpuSuspendIntr) */
    IopCpu cpu;
    KernFiber *fiber;
} IThread;

typedef struct IEvf { int used; u32 attr, init, bits; } IEvf;
typedef struct IMbx { int used; u32 attr; u32 head, tail; } IMbx;   /* IOP addresses of messages */

typedef struct HTimer {
    int used, running;
    u32 source, prescale, mode, compare, handler, arg, gp;
    u32 event;
    int pending;
} HTimer;

typedef struct IntrHandler { u32 func, arg, gp; int registered, enabled; } IntrHandler;

typedef struct RpcQueueEntry { IopRpcReq req; struct RpcQueueEntry *next; int used; } RpcQueueEntry;
typedef struct RpcServer { u32 sd, sid, func, buff, qd; } RpcServer;
typedef struct RpcQueue { u32 qd; int thread; RpcQueueEntry *head, *tail; } RpcQueue;

static IThread g_th[MAX_ITHREADS];
static IEvf g_evf[MAX_EVFS];
static IMbx g_mbx[MAX_MBXS];
static HTimer g_htimer[MAX_HTIMERS];
static IntrHandler g_intr[64];
static RpcServer g_servers[32];
static int g_nservers;
static RpcQueue g_queues[16];
static int g_nqueues;

static IThread *g_cur;          /* running IOP thread (NULL: scheduler or interrupt) */
static KernFiber *g_sched_fiber;    /* fiber that called IopKern_Run */
static int g_running;           /* inside IopKern_Run */
static int g_in_intr;
static u32 g_run_calls;    /* the current run's calls and scheduler turns (RUN_CALLS) */
static u32 g_last_used;   /* the calls and turns of the last IopKern_Run (0: nothing ran) */
enum { GIVE_NONE, GIVE_INTR, GIVE_BUDGET };   /* why a thread gives way at a call's return */
static u64 g_seq;
static int g_active;           /* a module has been started: the IOP exists */

static u32 arg(IopCpu *c, int n)
{
    return n < 4 ? c->r[4 + n] : IopMem_Read32(c->r[29] + 16 + 4 * (u32)(n - 4));
}
static void ret(IopCpu *c, u32 v) { c->r[2] = v; }

/* The RPC queues' entries: a fixed pool (the original game allocates them). */
#define RPC_POOL 256
static RpcQueueEntry g_rpc_pool[RPC_POOL];
static RpcQueueEntry *rpc_entry_alloc(void)
{
    for (int i = 0; i < RPC_POOL; i++)
        if (!g_rpc_pool[i].used) {
            snd_memset(&g_rpc_pool[i], 0, sizeof(g_rpc_pool[i]));
            g_rpc_pool[i].used = 1;
            return &g_rpc_pool[i];
        }
    SndLog_Fatal("iopk", "more than 256 RPC calls queued on the IOP");
}
static void rpc_entry_free(RpcQueueEntry *e) { e->used = 0; }

/* Warnings once per key (the original's rt_log_once). */
static u32 g_once[256];
static u32 g_nonce;
int SndLog_Once(u32 key)
{
    for (u32 i = 0; i < g_nonce; i++)
        if (g_once[i] == key)
            return 0;
    if (g_nonce < 256)
        g_once[g_nonce++] = key;
    return 1;
}

void SndLog_Fatal(const char *sys, const char *msg) { SndHost_Fatal(sys, msg); }

/* ------------------------------------------------------------ scheduler */

static IThread *thread_by_id(int id)
{
    if (id == 0)
        return g_cur;
    if (id < 1 || id >= MAX_ITHREADS || !g_th[id].used)
        return NULL;
    return &g_th[id];
}

static int thread_id(const IThread *t) { return (int)(t - g_th); }

static int g_iop_parked;
static void make_ready(IThread *t)
{
    if (!g_running)
        g_iop_parked = 0;   /* made ready from outside the IOP run: an event or a request */
    t->status = THS_READY;
    t->wait_type = TSW_NONE;
    t->seq = g_seq++;
}

static IThread *pick_ready(void)
{
    IThread *best = NULL;
    for (int i = 1; i < MAX_ITHREADS; i++) {
        IThread *t = &g_th[i];
        if (t->used && t->status == THS_READY && (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)))
            best = t;
    }
    return best;
}

/* Leave the current thread's fiber; returns when the scheduler runs it again. */
static void to_scheduler(void)
{
    Kern_FiberSwitch(g_sched_fiber);
}

static int wait_current(int type, int id)
{
    IThread *t = g_cur;
    if (!t || g_in_intr) {
        SND_LOGW("iopk", "blocking call (wait type %d) outside a thread", type);
        return -1;
    }
    t->status = THS_WAIT;
    t->wait_type = type;
    t->wait_id = id;
    t->wait_result = 0;
    to_scheduler();
    return t->wait_result;
}

/* Called by functions that may have readied a higher-priority thread. */
static void maybe_preempt(void)
{
    IThread *t = g_cur;
    if (!t || g_in_intr || !t->ie)
        return;
    IThread *n = pick_ready();
    if (n && n->prio < t->prio) {
        make_ready(t);
        t->seq = 0;   /* first in its priority */
        to_scheduler();
    }
}

/* Native functions at IOP addresses (IopNativeFn; the native ADX driver). */
#define MAX_NATIVE 48
static struct { u32 addr; IopNativeFn fn; } g_native[MAX_NATIVE];
static int g_nnative;

void IopNative_Map(u32 addr, IopNativeFn fn)
{
    if (g_nnative == MAX_NATIVE)
        SndLog_Fatal("iopk", "more than 48 native IOP functions");
    g_native[g_nnative].addr = addr;
    g_native[g_nnative++].fn = fn;
}

static IopNativeFn native_at(u32 pc)
{
    for (int i = 0; i < g_nnative; i++)
        if (g_native[i].addr == pc)
            return g_native[i].fn;
    return NULL;
}

int IopNative_IsMapped(u32 addr)
{
    return native_at(addr) != NULL;
}

/* A function the kernel runs as IOP code: the native function at an IOP address (IopNative_Map). */
static IopNativeFn native_must(u32 addr, const char *what)
{
    IopNativeFn nf = native_at(addr);
    if (!nf) {
        SND_LOGE("iopk", "%s at 0x%08X is not a native function", what, addr);
        SndLog_Fatal("iopk", "an IOP address with no native function");
    }
    return nf;
}

static int has_pending_interrupts(void);

/* The return of a kernel or libsd call (a native call's, native_call_point): count it, and say whether the thread gives way:
 * when the run's budget is used, or an interrupt or timer expiry is pending and the thread's interrupts are on. */
static int call_return(void)
{
    IThread *t = g_cur;
    if (!t || g_in_intr || !g_running)
        return GIVE_NONE;
    if (++g_run_calls >= RUN_CALLS)
        return GIVE_BUDGET;
    if (t->ie && has_pending_interrupts())
        return GIVE_INTR;
    return GIVE_NONE;
}

/* A thread is a fiber that calls its entry, a native function at the IOP address the thread was created with; the
 * entry's return is the thread's end (v0 is kept in its registers, for the module start's result). */
static void thread_fiber(void *p)
{
    IThread *t = p;
    for (;;) {
        IopNativeFn nf = native_must(t->entry, "an IOP thread's entry");
        t->cpu.r[2] = nf(t->cpu.r[4], t->cpu.r[5], t->cpu.r[6], t->cpu.r[7]);
        SND_LOGD("iopk", "IOP thread %d returned (v0 %d)", thread_id(t), (s32)t->cpu.r[2]);
        t->status = THS_DORMANT;
        to_scheduler();
    }
}

static void thread_setup(IThread *t, u32 a0, u32 a1)
{
    snd_memset(&t->cpu, 0, sizeof(t->cpu));
    t->cpu.r[4] = a0;
    t->cpu.r[5] = a1;
    t->cpu.r[28] = t->gp;
    t->cpu.r[29] = (t->stack + t->stack_size - 0x20) & ~0xFu;
    t->ie = 1;
    t->wakeups = 0;
    t->prio = t->init_prio;
    if (!t->fiber)
        t->fiber = Kern_FiberCreate(thread_fiber, t, 256 * 1024);
}

/* Call the native function at `func` as the current thread's, with the stack of a nested call, and return v0 (the RPC
 * server function; its locals are at the module's addresses, as the original game has them). */
static u32 call_native(u32 func, u32 a0, u32 a1, u32 a2, u32 a3)
{
    IThread *t = g_cur;
    IopNativeFn nf = native_must(func, "an RPC server function");
    IopCpu saved = t->cpu;
    t->cpu.r[4] = a0;
    t->cpu.r[5] = a1;
    t->cpu.r[6] = a2;
    t->cpu.r[7] = a3;
    t->cpu.r[29] = (saved.r[29] - 0x40) & ~0xFu;
    u32 v0 = nf(a0, a1, a2, a3);
    t->cpu = saved;
    return v0;
}

/* Run an interrupt or timer handler in interrupt context: the native libsd's (IOP_NATIVE_INTR + n) or a native
 * module's (the sound engine's 4 ms timer). */
static u32 call_handler(u32 func, u32 arg0, u32 gp)
{
    (void)gp;
    u32 r;
    g_in_intr = 1;
    if (g_iop_native_libsd && func - IOP_NATIVE_INTR < 0x100u)
        r = g_iop_native_libsd->intr(func, arg0);
    else
        r = native_must(func, "an interrupt handler")(arg0, 0, 0, 0);
    g_in_intr = 0;
    return r;
}

/* A native function called by a native interrupt handler (a callback), in the interrupt context it runs in. */
u32 IopNative_CallbackIntr(u32 func, u32 a0, u32 a1, u32 gp)
{
    (void)gp;
    return native_must(func, "an interrupt callback")(a0, a1, 0, 0);
}

/* A native function called from a native interrupt handler (libsd's), with four arguments. */
u32 IopNative_CallIntr(u32 func, u32 a0, u32 a1, u32 a2, u32 a3)
{
    IopNativeFn nf = native_at(func);
    if (!nf)
        SndLog_Fatal("iopk", "IopNative_CallIntr: not a native function");
    return nf(a0, a1, a2, a3);
}

static void timer_fire(u64 now, u32 idx);

/* Deliver pending interrupts and timer expiries. Returns 1 if any ran. */
static int dispatch_interrupts(void)
{
    int ran = 0;
    u64 pend = IopHw_PendingIrqs();
    for (int irq = 0; pend && irq < 64; irq++) {
        if (!(pend & (1ull << irq)))
            continue;
        IntrHandler *h = &g_intr[irq];
        if (!h->registered || !h->enabled)
            continue;
        IopHw_AckIrq(irq);
        SND_LOGT("iopk", "interrupt %d -> handler 0x%08X (arg 0x%08X)", irq, h->func, h->arg);
        if (!call_handler(h->func, h->arg, h->gp)) {
            h->enabled = 0;   /* a handler that returns 0 disables its interrupt */
            SND_LOGD("iopk", "interrupt %d handler 0x%08X returned 0: disabled", irq, h->func);
        }
        ran = 1;
    }
    for (int i = 0; i < MAX_HTIMERS; i++) {
        HTimer *tm = &g_htimer[i];
        if (!tm->used || !tm->pending)
            continue;
        tm->pending = 0;
        u32 next = call_handler(tm->handler, tm->arg, tm->gp);
        ran = 1;
        if (!next) {
            SND_LOGD("iopk", "hard timer %d handler returned 0: stopped", i);
            tm->running = 0;
        } else {
            tm->compare = next;
            timer_fire(0, (u32)i);   /* schedule the next expiry */
        }
    }
    return ran;
}

static int has_pending_interrupts(void)
{
    u64 pend = IopHw_PendingIrqs();
    for (int irq = 0; pend && irq < 64; irq++)
        if ((pend & (1ull << irq)) && g_intr[irq].registered && g_intr[irq].enabled)
            return 1;
    for (int i = 0; i < MAX_HTIMERS; i++)
        if (g_htimer[i].used && g_htimer[i].pending)
            return 1;
    return 0;
}

/* An event fired, or something reached the IOP from outside (an interrupt, a request): its busy loop may go on. */
void IopKern_Unpark(void)
{
    g_iop_parked = 0;
}

void IopKern_Run(void)
{
    if (!g_active || g_running)
        return;
    g_running = 1;
    g_sched_fiber = Kern_FiberSelf();
    g_run_calls = 0;
    while (g_run_calls < RUN_CALLS) {
        IThread *t = pick_ready();
        if ((!t || t->ie) && dispatch_interrupts()) {
            g_run_calls++;
            continue;
        }
        if (!t)
            break;
        g_cur = t;
        t->status = THS_RUN;
        Kern_FiberSwitch(t->fiber);
        g_cur = NULL;
        g_run_calls++;
    }
    u32 used = g_run_calls;
    g_running = 0;
    g_last_used = used;
    /* The frame clock (spec 7): the IOP never moves time. A run that used its whole budget with a thread
     * still ready is an IOP busy loop (it waits for a timer or for the EE): the IOP is parked, nothing it can
     * run at this instant, until the next event fires or something reaches it (IopKern_Unpark). */
    g_iop_parked = used >= RUN_CALLS && pick_ready() != NULL;
    if (pick_ready())
        SndSys_Post();   /* budget used up: come back soon */
}

int IopKern_Busy(void)
{
    return g_active && !g_iop_parked && (pick_ready() != NULL || has_pending_interrupts());
}

/* --frame-clock, step 2 of a delivery point (plan 3.6): the IOP runs everything it can at this instant.
 * 0 when nothing ran (idle, or only interrupts it cannot take now), 1 when it ran and is idle again (what
 * it posted exists now), 2 when it used its whole budget with a thread still ready: a busy loop that waits
 * for time, which the caller does not repeat for (IopKern_Run charges its time). */
int IopKern_CatchUp(void)
{
    if (!IopKern_Busy())
        return 0;
    IopKern_Run();
    if (g_last_used == 0)
        return 0;
    return pick_ready() ? 2 : 1;
}

/* ------------------------------------------------------------ timed waits */

static void delay_done(u64 now, u32 id)
{
    (void)now;
    IThread *t = thread_by_id((int)id);
    if (t && t->status == THS_WAIT && t->wait_type == TSW_DELAY) {
        t->delay_event = 0;
        make_ready(t);
        SndSys_Post();
    }
}

static u64 clocks_to_ns(u64 clocks, u32 source, u32 prescale)
{
    u64 hz = source == 2 ? 13500000ull : source == 4 ? 15734ull : IOP_CLOCK_HZ;
    return clocks * (prescale ? prescale : 1) * NS_PER_SEC / hz;
}

static void timer_expired(u64 now, u32 idx)
{
    (void)now;
    HTimer *tm = &g_htimer[idx];
    tm->event = 0;
    if (tm->used && tm->running) {
        tm->pending = 1;
        SndSys_Post();
    }
}

static void timer_fire(u64 now, u32 idx)
{
    (void)now;
    HTimer *tm = &g_htimer[idx];
    if (tm->event)
        SndSys_EventCancel(tm->event);
    u64 period = clocks_to_ns(tm->compare ? tm->compare : 1, tm->source, tm->prescale);
    if (period < 20000)
        period = 20000;   /* sanity: at most 50 kHz */
    tm->event = SndSys_EventAdd(SndSys_TimeNs() + period, timer_expired, idx);
}

/* ------------------------------------------------------------ vblank */

void IopKern_Vblank(int start)
{
    if (!g_active)
        return;
    IopHw_RaiseIrq(start ? 0 : 11);   /* VBLANK start / end */
    for (int i = 1; i < MAX_ITHREADS; i++) {
        IThread *t = &g_th[i];
        if (t->used && t->status == THS_WAIT && t->wait_type == TSW_VBLANK && t->wait_id == (start ? 1 : 0))
            make_ready(t);
    }
}

/* ------------------------------------------------------------ kernel: sysclib, stdio, sysmem */

static void h_memcpy(IopCpu *c)
{
    u32 d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
    for (u32 i = 0; i < n; i++)
        IopMem_Write8(d + i, IopMem_Read8(s + i));
    ret(c, d);
}
static void h_memset(IopCpu *c)
{
    u32 d = arg(c, 0), v = arg(c, 1), n = arg(c, 2);
    for (u32 i = 0; i < n; i++)
        IopMem_Write8(d + i, (u8)v);
    ret(c, d);
}
static void h_bzero(IopCpu *c)
{
    u32 d = arg(c, 0), n = arg(c, 1);
    for (u32 i = 0; i < n; i++)
        IopMem_Write8(d + i, 0);
}
/* The IOP's long is 32 bits, as the host's is on Windows but not on Linux:
 * out-of-range values saturate at the 32-bit limits on both. */
/* ------------------------------------------------------------ kernel: loadcore, intrman */

static void h_RegisterIntrHandler(IopCpu *c)
{
    u32 irq = arg(c, 0);
    if (irq >= 64) {
        ret(c, (u32)-1);
        return;
    }
    g_intr[irq] = (IntrHandler){arg(c, 2), arg(c, 3), c->r[28], 1, g_intr[irq].enabled};
    SND_LOGD("iopk", "interrupt %u handler 0x%08X", irq, arg(c, 2));
    ret(c, 0);
}
static void h_ReleaseIntrHandler(IopCpu *c) { u32 irq = arg(c, 0); if (irq < 64) g_intr[irq].registered = 0; ret(c, 0); }
static void h_EnableIntr(IopCpu *c) { u32 irq = arg(c, 0) & 0xFF; if (irq < 64) g_intr[irq].enabled = 1; ret(c, 0); }
/* DisableIntr(irq, &res): res = the irq if it was enabled (what EnableIntr
 * takes to restore it), else KE_INTRDISABLE, which is also the result. */
static void h_DisableIntr(IopCpu *c)
{
    u32 irq = arg(c, 0) & 0xFF;
    s32 res = -103, r = -103;   /* KE_INTRDISABLE */
    if (irq >= 64) {
        r = -101;               /* KE_ILLEGAL_INTRCODE */
    } else if (g_intr[irq].enabled) {
        res = (s32)irq;
        r = 0;
    }
    if (irq < 64)
        g_intr[irq].enabled = 0;
    if (arg(c, 1))
        IopMem_Write32(arg(c, 1), (u32)res);
    ret(c, (u32)r);
}
static void h_CpuSuspendIntr(IopCpu *c)
{
    int prev = g_cur ? g_cur->ie : 0;
    if (arg(c, 0))
        IopMem_Write32(arg(c, 0), (u32)prev);
    if (g_cur)
        g_cur->ie = 0;
    ret(c, prev ? 0 : (u32)-102);   /* KE_CPUDI when already disabled */
}
static void h_CpuResumeIntr(IopCpu *c)
{
    if (g_cur) {
        g_cur->ie = arg(c, 0) ? 1 : 0;
        if (g_cur->ie && has_pending_interrupts()) {
            make_ready(g_cur);
            g_cur->seq = 0;
            to_scheduler();   /* let pending interrupts in */
        }
    }
    ret(c, 0);
}
static void h_QueryIntrContext(IopCpu *c) { ret(c, (u32)g_in_intr); }

/* ------------------------------------------------------------ kernel: threads */

static void h_CreateThread(IopCpu *c)
{
    u32 p = arg(c, 0);
    for (int i = 1; i < MAX_ITHREADS; i++) {
        IThread *t = &g_th[i];
        if (t->used)
            continue;
        KernFiber *keep = t->fiber;   /* a parked fiber of a start thread that ended is reused */
        snd_memset(t, 0, sizeof(*t));
        t->fiber = keep;
        t->used = 1;
        t->attr = IopMem_Read32(p);
        t->option = IopMem_Read32(p + 4);
        t->entry = IopMem_Read32(p + 8);
        t->stack_size = (IopMem_Read32(p + 12) + 0xFF) & ~0xFFu;
        t->init_prio = t->prio = (int)IopMem_Read32(p + 16);
        t->stack = IopKern_HeapAlloc(t->stack_size);
        t->gp = c->r[28];
        t->status = THS_DORMANT;
        if (!t->stack) {
            t->used = 0;
            ret(c, (u32)-400);
            return;
        }
        SND_LOGD("iopk", "CreateThread %d: entry 0x%08X prio %d stack 0x%X", i, t->entry, t->prio, t->stack_size);
        ret(c, (u32)i);
        return;
    }
    ret(c, (u32)-400);
}

static void h_StartThread(IopCpu *c)
{
    IThread *t = thread_by_id((int)arg(c, 0));
    if (!t || t->status != THS_DORMANT) {
        ret(c, (u32)-407);
        return;
    }
    thread_setup(t, arg(c, 1), 0);
    make_ready(t);
    ret(c, 0);
    maybe_preempt();
}

static void h_GetThreadId(IopCpu *c) { ret(c, g_cur ? (u32)thread_id(g_cur) : (u32)-1); }

static void h_iReferThreadStatus(IopCpu *c)
{
    IThread *t = thread_by_id((int)arg(c, 0));
    u32 info = arg(c, 1);
    SND_LOGT("iopk", "ReferThreadStatus(%d) -> status %d wait %d (ra 0x%08X)%s", (int)arg(c, 0), t ? t->status : -1,
         t ? t->wait_type : -1, c->r[31], g_in_intr ? " in interrupt" : "");
    if (!t) {
        ret(c, (u32)-406);
        return;
    }
    if (info) {
        IopMem_Write32(info + 0, t->attr);
        IopMem_Write32(info + 4, t->option);
        IopMem_Write32(info + 8, (u32)t->status);
        IopMem_Write32(info + 12, t->entry);
        IopMem_Write32(info + 16, t->stack);
        IopMem_Write32(info + 20, t->stack_size);
        IopMem_Write32(info + 24, t->gp);
        IopMem_Write32(info + 28, (u32)t->init_prio);
        IopMem_Write32(info + 32, (u32)t->prio);
        IopMem_Write32(info + 36, (u32)t->wait_type);
        IopMem_Write32(info + 40, (u32)t->wait_id);
        IopMem_Write32(info + 44, (u32)t->wakeups);
    }
    ret(c, 0);
}

static void h_SleepThread(IopCpu *c)
{
    if (g_cur && g_cur->wakeups > 0) {
        g_cur->wakeups--;
        ret(c, 0);
        return;
    }
    ret(c, (u32)wait_current(TSW_SLEEP, 0));
}

static void wakeup(IopCpu *c)
{
    IThread *t = thread_by_id((int)arg(c, 0));
    SND_LOGT("iopk", "WakeupThread(%d): status %d wait %d (ra 0x%08X)%s", (int)arg(c, 0), t ? t->status : -1,
         t ? t->wait_type : -1, c->r[31], g_in_intr ? " in interrupt" : "");
    if (!t || t == g_cur) {
        ret(c, (u32)-406);
        return;
    }
    if (t->status == THS_WAIT && t->wait_type == TSW_SLEEP)
        make_ready(t);
    else
        t->wakeups++;
    ret(c, 0);
}
static void h_iWakeupThread(IopCpu *c) { wakeup(c); }
static void h_DelayThread(IopCpu *c)
{
    IThread *t = g_cur;
    if (!t) {
        ret(c, (u32)-1);
        return;
    }
    u64 us = arg(c, 0);
    t->delay_event = SndSys_EventAdd(SndSys_TimeNs() + us * 1000ull, delay_done, (u32)thread_id(t));
    wait_current(TSW_DELAY, 0);
    ret(c, 0);
}

static u64 sys_clock(void) { return SndSys_TimeNs() * IOP_CLOCK_HZ / NS_PER_SEC; }
static void h_GetSystemTimeLow(IopCpu *c) { ret(c, (u32)sys_clock()); }
static void h_SysClock2USec(IopCpu *c)
{
    u32 p = arg(c, 0);
    u64 clk = IopMem_Read32(p) | ((u64)IopMem_Read32(p + 4) << 32);
    u64 us = clk * 1000000ull / IOP_CLOCK_HZ;
    IopMem_Write32(arg(c, 1), (u32)(us / 1000000ull));
    IopMem_Write32(arg(c, 2), (u32)(us % 1000000ull));
}
/* ------------------------------------------------------------ kernel: event flags */

static IEvf *evf(u32 id) { return id > 0 && id < MAX_EVFS && g_evf[id].used ? &g_evf[id] : NULL; }

static void h_CreateEventFlag(IopCpu *c)
{
    u32 p = arg(c, 0);
    for (int i = 1; i < MAX_EVFS; i++) {
        if (g_evf[i].used)
            continue;
        g_evf[i] = (IEvf){1, IopMem_Read32(p), IopMem_Read32(p + 8), IopMem_Read32(p + 8)};
        ret(c, (u32)i);
        return;
    }
    ret(c, (u32)-409);
}
static int evf_satisfied(u32 bits, u32 want, u32 mode)
{
    return (mode & 1) ? (bits & want) != 0 : (bits & want) == want;
}

static void set_event(IopCpu *c)
{
    u32 id = arg(c, 0);
    IEvf *e = evf(id);
    SND_LOGT("iopk", "SetEventFlag(%u, 0x%X)%s", id, arg(c, 1), g_in_intr ? " in interrupt" : "");
    if (!e) {
        ret(c, (u32)-409);
        return;
    }
    e->bits |= arg(c, 1);
    for (int i = 1; i < MAX_ITHREADS && e->bits; i++) {
        IThread *t = &g_th[i];
        if (!(t->used && t->status == THS_WAIT && t->wait_type == TSW_EVENTFLAG && t->wait_id == (int)id))
            continue;
        if (!evf_satisfied(e->bits, t->wait_bits, t->wait_mode))
            continue;
        if (t->wait_res)
            IopMem_Write32(t->wait_res, e->bits);
        if (t->wait_mode & 0x10)
            e->bits = 0;
        make_ready(t);
    }
    ret(c, 0);
}
static void h_SetEventFlag(IopCpu *c) { set_event(c); maybe_preempt(); }
static void h_iSetEventFlag(IopCpu *c) { set_event(c); }
static void h_ClearEventFlag(IopCpu *c)
{
    IEvf *e = evf(arg(c, 0));
    if (!e) {
        ret(c, (u32)-409);
        return;
    }
    e->bits &= arg(c, 1);
    ret(c, 0);
}
static void h_WaitEventFlag(IopCpu *c)
{
    u32 id = arg(c, 0), want = arg(c, 1), mode = arg(c, 2), res = arg(c, 3);
    IEvf *e = evf(id);
    if (!e) {
        ret(c, (u32)-409);
        return;
    }
    if (evf_satisfied(e->bits, want, mode)) {
        if (res)
            IopMem_Write32(res, e->bits);
        if (mode & 0x10)
            e->bits = 0;
        ret(c, 0);
        return;
    }
    g_cur->wait_bits = want;
    g_cur->wait_mode = mode;
    g_cur->wait_res = res;
    ret(c, (u32)wait_current(TSW_EVENTFLAG, (int)id));
}
static void h_ReferEventFlagStatus(IopCpu *c)
{
    IEvf *e = evf(arg(c, 0));
    u32 info = arg(c, 1);
    if (!e) {
        ret(c, (u32)-409);
        return;
    }
    int waiting = 0;
    for (int i = 1; i < MAX_ITHREADS; i++)
        if (g_th[i].used && g_th[i].status == THS_WAIT && g_th[i].wait_type == TSW_EVENTFLAG && g_th[i].wait_id == (int)arg(c, 0))
            waiting++;
    IopMem_Write32(info + 0, e->attr);
    IopMem_Write32(info + 4, 0);
    IopMem_Write32(info + 8, e->init);
    IopMem_Write32(info + 12, e->bits);
    IopMem_Write32(info + 16, (u32)waiting);
    ret(c, 0);
}

/* ------------------------------------------------------------ kernel: message boxes */

static void h_CreateMbx(IopCpu *c)
{
    for (int i = 1; i < MAX_MBXS; i++) {
        if (g_mbx[i].used)
            continue;
        g_mbx[i] = (IMbx){1, IopMem_Read32(arg(c, 0)), 0, 0};
        ret(c, (u32)i);
        return;
    }
    ret(c, (u32)-410);
}
static IMbx *mbx(u32 id) { return id > 0 && id < MAX_MBXS && g_mbx[id].used ? &g_mbx[id] : NULL; }
static void send_mbx(IopCpu *c)
{
    u32 id = arg(c, 0), msg = arg(c, 1);
    IMbx *m = mbx(id);
    if (!m) {
        ret(c, (u32)-410);
        return;
    }
    for (int i = 1; i < MAX_ITHREADS; i++) {
        IThread *t = &g_th[i];
        if (t->used && t->status == THS_WAIT && t->wait_type == TSW_MBX && t->wait_id == (int)id) {
            IopMem_Write32(t->wait_msgvar, msg);
            make_ready(t);
            ret(c, 0);
            return;
        }
    }
    IopMem_Write32(msg, 0);   /* next */
    if (m->tail)
        IopMem_Write32(m->tail, msg);
    else
        m->head = msg;
    m->tail = msg;
    ret(c, 0);
}
static void h_SendMbx(IopCpu *c) { send_mbx(c); maybe_preempt(); }
static void h_ReceiveMbx(IopCpu *c)
{
    u32 var = arg(c, 0), id = arg(c, 1);
    IMbx *m = mbx(id);
    if (!m) {
        ret(c, (u32)-410);
        return;
    }
    if (m->head) {
        u32 msg = m->head;
        m->head = IopMem_Read32(msg);
        if (!m->head)
            m->tail = 0;
        IopMem_Write32(var, msg);
        ret(c, 0);
        return;
    }
    g_cur->wait_msgvar = var;
    ret(c, (u32)wait_current(TSW_MBX, (int)id));
}
/* ------------------------------------------------------------ kernel: timrman, vblank */

static HTimer *htimer(u32 id) { return id >= 1 && id <= MAX_HTIMERS && g_htimer[id - 1].used ? &g_htimer[id - 1] : NULL; }

static void h_AllocHardTimer(IopCpu *c)
{
    for (int i = 0; i < MAX_HTIMERS; i++) {
        if (g_htimer[i].used)
            continue;
        snd_memset(&g_htimer[i], 0, sizeof(HTimer));
        g_htimer[i].used = 1;
        g_htimer[i].source = arg(c, 0);
        g_htimer[i].prescale = arg(c, 2);
        SND_LOGD("iopk", "AllocHardTimer(source %u, size %u, prescale %u) -> %d", arg(c, 0), arg(c, 1), arg(c, 2), i + 1);
        ret(c, (u32)(i + 1));
        return;
    }
    ret(c, (u32)-150);
}
static void h_SetTimerHandler(IopCpu *c)
{
    HTimer *t = htimer(arg(c, 0));
    if (!t) {
        ret(c, (u32)-151);
        return;
    }
    t->compare = arg(c, 1);
    t->handler = arg(c, 2);
    t->arg = arg(c, 3);
    t->gp = c->r[28];
    ret(c, 0);
}
static void h_SetupHardTimer(IopCpu *c)
{
    HTimer *t = htimer(arg(c, 0));
    if (!t) {
        ret(c, (u32)-151);
        return;
    }
    t->source = arg(c, 1);
    t->mode = arg(c, 2);
    t->prescale = arg(c, 3);
    ret(c, 0);
}
static void h_StartHardTimer(IopCpu *c)
{
    HTimer *t = htimer(arg(c, 0));
    if (!t) {
        ret(c, (u32)-151);
        return;
    }
    t->running = 1;
    SND_LOGI("iopk", "hard timer %u: compare %u, source %u, prescale %u (period %llu ns)", arg(c, 0), t->compare, t->source,
         t->prescale, (unsigned long long)clocks_to_ns(t->compare ? t->compare : 1, t->source, t->prescale));
    timer_fire(0, arg(c, 0) - 1);
    ret(c, 0);
}
static void h_WaitVblankEnd(IopCpu *c) { ret(c, (u32)wait_current(TSW_VBLANK, 0)); }

/* ------------------------------------------------------------ kernel: sifman, sifcmd */

static void h_sceSifSetDma(IopCpu *c)
{
    u32 t = arg(c, 0), n = arg(c, 1);
    for (u32 i = 0; i < n; i++, t += 16) {
        u32 src = IopMem_Read32(t), dst = IopMem_Read32(t + 4), size = IopMem_Read32(t + 8);
        u32 n4 = (size + 3) & ~3u, a = iop_phys(src) & (IOP_RAM_SIZE - 1);
        if (n4 > IOP_RAM_SIZE - a) {
            SND_LOG_ONCE("iopk", 0x300, "IOP sceSifSetDma from 0x%08X (0x%X bytes) leaves IOP RAM: cut", src, n4);
            n4 = IOP_RAM_SIZE - a;
        }
        SndSys_ToEe(dst, IopMem_Ptr(a), n4);
    }
    ret(c, 1);   /* transfer id (never 0) */
}
static void h_sceSifInitRpc(IopCpu *c) { ret(c, 0); }

static void h_sceSifRegisterRpc(IopCpu *c)
{
    u32 sd = arg(c, 0), sid = arg(c, 1), func = arg(c, 2), buff = arg(c, 3), qd = arg(c, 6);
    IopMem_Write32(sd + 0, sid);
    IopMem_Write32(sd + 4, func);
    IopMem_Write32(sd + 8, buff);
    if (g_nservers < 32)
        g_servers[g_nservers++] = (RpcServer){sd, sid, func, buff, qd};
    SND_LOGI("iopk", "RPC server 0x%08X registered (function 0x%08X, buffer 0x%06X)", sid, func, buff);
    SndSys_RpcRegister(sid, sd, buff);
    ret(c, 0);
}

static RpcQueue *queue_by_qd(u32 qd)
{
    for (int i = 0; i < g_nqueues; i++)
        if (g_queues[i].qd == qd)
            return &g_queues[i];
    return NULL;
}

static void h_sceSifSetRpcQueue(IopCpu *c)
{
    u32 qd = arg(c, 0);
    RpcQueue *q = queue_by_qd(qd);
    if (!q && g_nqueues < 16) {
        q = &g_queues[g_nqueues++];
        snd_memset(q, 0, sizeof(*q));
        q->qd = qd;
    }
    if (q)
        q->thread = (int)arg(c, 1);
    IopMem_Write32(qd, arg(c, 1));
    ret(c, 0);
}

void IopKern_RpcCall(const IopRpcReq *req)
{
    for (int i = 0; i < g_nservers; i++) {
        if (g_servers[i].sd != req->sd)
            continue;
        RpcQueue *q = queue_by_qd(g_servers[i].qd);
        if (!q) {
            SND_LOGE("iopk", "RPC for server 0x%08X without a queue", g_servers[i].sid);
            return;
        }
        RpcQueueEntry *e = rpc_entry_alloc();
        e->req = *req;
        if (q->tail)
            q->tail->next = e;
        else
            q->head = e;
        q->tail = e;
        IThread *t = thread_by_id(q->thread);
        if (t && t->status == THS_WAIT && t->wait_type == TSW_RPC)
            make_ready(t);
        g_iop_parked = 0;
        SndSys_Post();
        return;
    }
    SND_LOGE("iopk", "RPC for unknown server 0x%08X", req->sd);
}

static void h_sceSifRpcLoop(IopCpu *c)
{
    RpcQueue *q = queue_by_qd(arg(c, 0));
    if (!q) {
        SND_LOGE("iopk", "sceSifRpcLoop on an unknown queue");
        ret(c, 0);
        return;
    }
    for (;;) {
        while (!q->head)
            wait_current(TSW_RPC, 0);
        RpcQueueEntry *e = q->head;
        q->head = e->next;
        if (!q->head)
            q->tail = NULL;
        u32 func = 0, buff = 0;
        for (int i = 0; i < g_nservers; i++)
            if (g_servers[i].sd == e->req.sd) {
                func = g_servers[i].func;
                buff = g_servers[i].buff;
            }
        SND_LOGD("iopk", "RPC sd %08X fno 0x%X size %u: %08X %08X %08X %08X", e->req.sd, e->req.fno, e->req.size,
             IopMem_Read32(buff), IopMem_Read32(buff + 4), IopMem_Read32(buff + 8), IopMem_Read32(buff + 12));
        if (SndHost_LogLevel() >= SND_LOG_TRACE) {   /* the whole request, at trace level */
            char hx[2 * 1024 + 1];
            u32 n = e->req.size < 1024 ? e->req.size : 1024;
            for (u32 i = 0; i < n; i++)
                { u8 b = IopMem_Read8(buff + i); hx[2 * i] = "0123456789abcdef"[b >> 4]; hx[2 * i + 1] = "0123456789abcdef"[b & 15]; }
            hx[2 * n] = 0;
            SndHost_Log(SND_LOG_TRACE, "rpcx", "X %x %x %x %s", e->req.sd, e->req.fno, e->req.size, hx);
        }
        u32 rbuf = func ? call_native(func, e->req.fno, buff, e->req.size, 0) : 0;
        if (rbuf && SndHost_LogLevel() >= SND_LOG_TRACE) {
            char hx[2 * 1024 + 1];
            for (u32 i = 0; i < 1024; i++)
                { u8 b = IopMem_Read8(rbuf + i); hx[2 * i] = "0123456789abcdef"[b >> 4]; hx[2 * i + 1] = "0123456789abcdef"[b & 15]; }
            hx[2048] = 0;
            SndHost_Log(SND_LOG_TRACE, "rpcx", "Y %x %x %x %s %x", e->req.sd, e->req.fno, rbuf, hx, e->req.recv_size);
        }
        if (rbuf)
            SND_LOGD("iopk", "  -> reply %08X: %08X %08X %08X %08X", rbuf, IopMem_Read32(rbuf), IopMem_Read32(rbuf + 4),
                 IopMem_Read32(rbuf + 8), IopMem_Read32(rbuf + 12));
        SndSys_RpcDone(&e->req, rbuf);
        rpc_entry_free(e);
    }
}

static void h_sceSifGetOtherData(IopCpu *c)
{
    u32 src = arg(c, 1), dst = arg(c, 2), size = arg(c, 3);
    u8 tmp[16];
    for (u32 i = 0; i < size; i += 16) {
        SndSys_FromEe((src + i) & ~15u, tmp, 16);   /* the original's mem_read128: an aligned quadword */
        u32 k = size - i < 16 ? size - i : 16;
        for (u32 j = 0; j < k; j++)
            IopMem_Write8(dst + i + j, tmp[j]);
    }
    ret(c, 0);
}

/* ------------------------------------------------------------ the kernel functions by name */

typedef void (*KernFn)(IopCpu *c);
typedef struct KernEntry {
    const char *name;
    KernFn fn;
} KernEntry;

/* The kernel functions the native modules call, by name (IopNative_Call, IopKern_Call). */
#define H(f) {#f, h_##f}
static const KernEntry g_kern_entries[] = {
    H(RegisterIntrHandler), H(ReleaseIntrHandler), H(EnableIntr), H(DisableIntr), H(CpuSuspendIntr), H(CpuResumeIntr),
    H(QueryIntrContext),
    H(sceSifSetDma), H(sceSifInitRpc), H(sceSifRegisterRpc), H(sceSifSetRpcQueue), H(sceSifRpcLoop),
    H(sceSifGetOtherData),
    H(memcpy), H(memset), H(bzero),
    H(CreateThread), H(StartThread), H(GetThreadId), H(iReferThreadStatus),
    H(SleepThread), H(iWakeupThread), H(DelayThread), H(SysClock2USec), H(GetSystemTimeLow),
    H(CreateEventFlag), H(SetEventFlag), H(iSetEventFlag), H(ClearEventFlag), H(WaitEventFlag), H(ReferEventFlagStatus),
    H(CreateMbx), H(SendMbx), H(ReceiveMbx),
    H(AllocHardTimer), H(SetTimerHandler), H(SetupHardTimer), H(StartHardTimer),
    H(WaitVblankEnd),
};
#undef H

const IopNativeLib *g_iop_native_libsd;
const IopNativeModule *g_iop_native_adx;
const IopNativeModule *g_iop_native_cse;

u32 IopKern_Call(const IopCpu *caller, const char *name, u32 a0, u32 a1, u32 a2, u32 a3)
{
    for (u32 h = 0; h < sizeof(g_kern_entries) / sizeof(g_kern_entries[0]); h++) {
        if (snd_strcmp(g_kern_entries[h].name, name) == 0) {
            IopCpu c;
            snd_memset(&c, 0, sizeof(c));
            c.r[4] = a0;
            c.r[5] = a1;
            c.r[6] = a2;
            c.r[7] = a3;
            if (caller) {
                c.r[28] = caller->r[28];
                c.r[29] = caller->r[29];
            }
            if (SndHost_LogLevel() >= SND_LOG_TRACE && (g_cur || g_in_intr)) {   /* the native modules' calls, for the logs */
                SndHost_Log(SND_LOG_TRACE, "kc", "K %d %s %x %x %x %x %x", g_in_intr ? -1 : thread_id(g_cur), g_kern_entries[h].name, a0, a1, a2, a3, 0);
                g_kern_entries[h].fn(&c);
                SndHost_Log(SND_LOG_TRACE, "kc", "k %d %s %x", g_in_intr ? -1 : thread_id(g_cur), g_kern_entries[h].name, c.r[2]);
            } else
                g_kern_entries[h].fn(&c);
            return c.r[2];
        }
    }
    SndLog_Fatal("iopk", "IopKern_Call: no such kernel function");
}

/* The return of a call made by native code on a thread is a call point: the thread gives way
 * when the run's budget is used or an interrupt is pending. */
static void native_call_point(void)
{
    IThread *t = g_cur;
    int why = call_return();
    if (why == GIVE_NONE)
        return;
    make_ready(t);
    if (why == GIVE_INTR)
        t->seq = 0;   /* an interrupt, not a rotation: first in its priority */
    to_scheduler();
}

u32 IopNative_CallN(const char *name, const u32 *a, int n)
{
    u32 args[4] = {0, 0, 0, 0};
    if (!g_cur)
        SndLog_Fatal("iopk", "IopNative_Call outside a thread");
    for (int i = 0; i < n && i < 4; i++)
        args[i] = a[i];
    for (int i = 4; i < n; i++)   /* the arguments past the fourth are on the thread's stack, as the ABI has them */
        IopMem_Write32(g_cur->cpu.r[29] + 16 + 4 * (u32)(i - 4), a[i]);
    u32 v = IopKern_Call(&g_cur->cpu, name, args[0], args[1], args[2], args[3]);
    native_call_point();
    return v;
}

u32 IopNative_Call(const char *name, u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 a[4] = {a0, a1, a2, a3};
    return IopNative_CallN(name, a, 4);
}

u32 IopNative_StackTop(void)
{
    if (!g_cur)
        SndLog_Fatal("iopk", "IopNative_StackTop outside a thread");
    return g_cur->stack + g_cur->stack_size;
}

/* A libsd export with a fifth argument, which is on the thread's stack as the ABI has it. */
u32 IopNative_Sd5(u32 index, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4)
{
    if (!g_cur)
        SndLog_Fatal("iopk", "IopNative_Sd5 outside a thread");
    IopMem_Write32(g_cur->cpu.r[29] + 16, a4);
    return IopNative_Sd(index, a0, a1, a2, a3);
}

u32 IopNative_Sd(u32 index, u32 a0, u32 a1, u32 a2, u32 a3)
{
    IopCpu c;
    if (!g_cur || !g_iop_native_libsd)
        SndLog_Fatal("iopk", "IopNative_Sd outside a thread or without the native libsd");
    c = g_cur->cpu;
    c.r[4] = a0;
    c.r[5] = a1;
    c.r[6] = a2;
    c.r[7] = a3;
    g_iop_native_libsd->call(&c, index);
    native_call_point();
    return c.r[2];
}

/* ------------------------------------------------------------ a module's start */

/* A module's start function on a start thread, as modload's StartModule runs it for a module loaded from a buffer: the
 * native function at `entry`, called with (argc, argv), on a thread of priority 8 with gp `gp` and a stack of 0x4000 bytes
 * from the IOP heap (freed when the start has returned). The IOP runs until the start function has returned, then until
 * the threads it started have reached their first wait (the servers register). 0, with the function's v0 in *res and
 * *returned saying whether it returned; -1 when no thread is free. */
int IopKern_StartModule(u32 entry, u32 gp, u32 argc, u32 argv, int *res, int *returned)
{
    g_active = 1;
    int tid = 0;
    for (int i = 1; i < MAX_ITHREADS && !tid; i++)
        if (!g_th[i].used)
            tid = i;
    if (!tid)
        return -1;
    IThread *t = &g_th[tid];
    snd_memset(t, 0, sizeof(*t));
    t->used = 1;
    t->entry = entry;
    t->gp = gp;
    t->stack_size = 0x4000;
    t->stack = IopKern_HeapAlloc(t->stack_size);
    t->init_prio = 8;
    thread_setup(t, argc, argv);
    make_ready(t);
    for (int guard = 0; guard < 1000 && t->status != THS_DORMANT; guard++)
        IopKern_Run();
    *res = (int)t->cpu.r[2];
    *returned = t->status == THS_DORMANT;
    t->used = 0;
    IopKern_HeapFree(t->stack);
    /* Let the threads it started reach their first wait (servers register). */
    for (int guard = 0; guard < 100 && IopKern_Busy(); guard++)
        IopKern_Run();
    return 0;
}

void IopKern_Stats(void)
{
    if (!g_active)
        return;
    int n = 0;
    for (int i = 1; i < MAX_ITHREADS; i++)
        if (g_th[i].used)
            n++;
    SND_LOGI("iopk", "IOP: %d threads", n);
    for (int i = 1; i < MAX_ITHREADS; i++) {
        IThread *t = &g_th[i];
        if (t->used)
            SND_LOGI("iopk", "  thread %d: prio %d status %d wait %d/%d entry 0x%08X", i, t->prio, t->status, t->wait_type,
                 t->wait_id, t->entry);
        if (t->used && t->status == THS_WAIT && t->wait_type == TSW_EVENTFLAG && evf((u32)t->wait_id))
            SND_LOGI("iopk", "    waits for bits 0x%X (mode %u) of event flag %d, which has 0x%X", t->wait_bits, t->wait_mode,
                 t->wait_id, evf((u32)t->wait_id)->bits);
    }
    Spu2_Stats();
}
