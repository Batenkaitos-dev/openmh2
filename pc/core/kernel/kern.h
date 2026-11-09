#ifndef MH2_KERNEL_KERN_H
#define MH2_KERNEL_KERN_H

/*
 * The kernel's interface to the other parts of the platform layer:
 * the frame clock's time and events,
 * the waits and delivery points the libraries call, interrupts, the SIF's
 * registry of IOP services, the IOP's scheduling, raw fibers, and the VBlank's
 * steps. The rules are the frame clock's ("the specification"
 * below, with its section numbers); this file says which call is which rule.
 *
 * Who includes it: the platform layer's game-facing files (the .c files under pc/core/
 * and pc/core/api/, compiled with the game's flags and calling
 * convention). A host_*.c file does not call these functions (the calling
 * conventions differ: pc/game_flags.txt).
 *
 * Contexts. Platform code runs in one of four contexts, and some calls
 * behave differently in each:
 *  - a game thread (the game called a library function, or a game thread
 *    runs platform code it reached through the API);
 *  - an interrupt handler (the kernel is running a handler: the game's, or
 *    a library's own, such as the SIF's);
 *  - an event (a function given to Kern_EventAdd, while it fires);
 *  - the idle loop (no thread is ready).
 * A blocking wait is only allowed in a game thread outside a handler. From a
 * handler or an event, the calls that ready a thread only ask for a thread
 * switch, which the delivery point makes when it ends (the specification,
 * 4, step 3).
 */
#include "common.h"

/* ------------------------------------------------------------------ time */

/* Guest time in nanoseconds since the run began (the specification, the
 * start). It moves only at the jump (1): never during a call that does not
 * wait. Nothing the game can observe reads a host clock. */
u64 Kern_Now(void);

/* The number of VBlank starts so far (VBlank n starts at n x 16,683,350 ns). */
u64 Kern_VblankCount(void);

/* The calendar the game reads (sceCdReadClock): whole seconds since
 * 1970-01-01 UTC, 1140058800 (2006-02-16 03:00:00 UTC) plus whole guest
 * seconds, or the start the host gave plus whole guest seconds.
 * Kern_CalendarOffset is the time zone sceCdReadClock shows it in; the
 * memory card's times are JST's always (the original's rt_calendar).
 * The modes, chosen before the kernel starts:
 *   - checks and deterministic runs: 1140058800 in JST (+9 h, 32400 s);
 *   - play (main.c's --local-date): the host's date and zone, read once
 *     (Kern_SetCalendarLocal).
 * Kern_CalendarStart gives the start and zone in force.
 * Kern_SetCalendarLocal takes the host's date and zone, Kern_SetCalendar sets both
 * from the caller's values,
 * Kern_SetCalendarEpoch the epoch only. */
u64 Kern_CalendarSeconds(void);
s32 Kern_CalendarOffset(void);
void Kern_CalendarStart(u64 *epochSeconds, s32 *utcOffsetSeconds);
void Kern_SetCalendar(u64 epochSeconds, s32 utcOffsetSeconds);
void Kern_SetCalendarEpoch(u64 seconds);
void Kern_SetCalendarLocal(void);

/* ---------------------------------------------------------------- events */

/*
 * A timed event (the specification, 2): fn(now, param) is called at the
 * first delivery point at or after `when` (on the frame clock, at `when`
 * itself: time only jumps to an event's time). Events with the same time
 * fire in the order they were added. Firing takes the event off the queue
 * first; fn may add or cancel events. Every event that fires also unparks
 * the IOP (7). An event runs no game code: what it does to the game is to
 * post (Kern_RaiseIntc, Kern_RaiseDmac, Kern_SemaSignal), and the delivery
 * point takes it.
 *
 * Kern_EventAdd returns the event's id (never 0). More than 256 queued
 * events is fatal. Kern_EventCancel of an id that has fired or was
 * cancelled does nothing.
 */
typedef void (*KernEventFn)(u64 now, u32 param);
u32 Kern_EventAdd(u64 when, KernEventFn fn, u32 param);
void Kern_EventCancel(u32 id);

/* ------------------------------------------------------------ interrupts */

/* The INTC causes (the specification, 4.2). */
enum {
    KERN_INTC_GS = 0, KERN_INTC_SBUS = 1, KERN_INTC_VBLANK_S = 2, KERN_INTC_VBLANK_E = 3,
    KERN_INTC_VIF0 = 4, KERN_INTC_VIF1 = 5, KERN_INTC_VU0 = 6, KERN_INTC_VU1 = 7, KERN_INTC_IPU = 8,
    KERN_INTC_TIM0 = 9, KERN_INTC_TIM1 = 10, KERN_INTC_TIM2 = 11, KERN_INTC_TIM3 = 12
};

/*
 * Raising an interrupt posts it: the INTC cause's I_STAT bit, or the DMAC
 * channel's D_STAT bit (CIS). Nothing runs at the call. It is taken at the
 * next delivery point where it is unmasked and the context's interrupts are
 * on (4.1, 4.2). A cause raised inside a handler is taken when the handler
 * returns, in the same delivery point.
 */
void Kern_RaiseIntc(int cause);
void Kern_RaiseDmac(int channel);

/*
 * The registers behind them, for gfx's register dispatch (Pal_EeRegRead32,
 * Pal_EeRegWrite32), which calls these for their addresses:
 *  - INTC: I_STAT 0x1000F000 (a write of 1 clears the bit), I_MASK
 *    0x1000F010 (a write of 1 toggles it).
 *  - D_STAT 0x1000E010: the channels' status bits 0-15 (a write of 1
 *    clears) and their mask bits 16-31 (a write of 1 toggles). The other
 *    DMAC registers are gfx's.
 *  - The EE timers, 0x10000000-0x10001FFF: timer n = (addr >> 11) & 3,
 *    register addr & 0xF0 (0x00 COUNT, 0x10 MODE, 0x20 COMP, 0x30 HOLD).
 *    The specification, 2.2.
 * None of these writes is a delivery point (4.1).
 */

/*
 * One entry for gfx's dispatch: every register of the EE timers
 * (0x10000000-0x10001FFF), INTC (0x1000F000-0x1000F01F), the SIO
 * (0x1000F100-0x1000F1FF: a byte to TXFIFO 0x1000F180 is the console's, one
 * log line per newline) and the SIF (0x1000F200-0x1000F2FF: MSCOM, SMCOM,
 * MSFLG (a write sets bits), SMFLG (a write clears bits)), as the original's
 * hardware model. Another address there reads 0.
 */
u32 Kern_EeRegRead32(u32 addr);
void Kern_EeRegWrite32(u32 addr, u32 value);

/*
 * SetVSyncFlag (syscall 0x73, for gfx's sceGsSyncV waits): the next VBlank
 * start writes *flag = 1 and *csr = the CSR word (Gfx_GsCsrForVSyncFlag:
 * the CSR as a read gives it, FIELD included), step 10, and then drops the
 * words: one write per call, as a PS2's kernel must (the original's slots
 * are its caller's stack).
 * A second call before that VBlank replaces the words.
 */
void Kern_SetVSyncFlag(u32 *flag, u64 *csr);
u32 Kern_IntcRead(u32 addr);
void Kern_IntcWrite(u32 addr, u32 value);
u32 Kern_DmacStatRead(void);
void Kern_DmacStatWrite(u32 value);
u32 Kern_TimerRead(u32 addr);
void Kern_TimerWrite(u32 addr, u32 value);

/*
 * DIntr and EIntr as a library uses them (never a delivery point: the
 * specification, 6.3): interrupts off or on for the running context. Each
 * returns the enable's old bit (1 on).
 */
int Kern_LibDIntr(void);
int Kern_LibEIntr(void);

/* 1 while the kernel runs an interrupt handler (the game's or a library's). */
int Kern_InHandler(void);

/* The running thread's id (in a handler, the interrupted thread's; 0 in the
 * idle loop). The game's threads are numbered from 1 (the main thread). */
s32 Kern_ThreadId(void);
/*
 * Whose stack holds p (verify's stack token): KERN_STACK_HANDLER while an
 * interrupt handler (or an alarm's callback) runs and p lies below the
 * stack pointer its fiber had when the handler started (the token `SI`);
 * else the id of the game thread whose fiber stack holds p; else -1.
 */
#define KERN_STACK_HANDLER (-2)
s32 Kern_StackOwner(const void *p);

/* ------------------------------------------------------------------ waits */

/*
 * Semaphores. These are the game's semaphores (the same ids and objects as
 * CreateSema's, from 1, the lowest unused): a library that waits on the PS2
 * through a semaphore of its own creates it here, so that the ids the game
 * gets afterwards are the PS2's.
 *  - Kern_SemaWait with the count at zero is a blocking wait (8): the thread
 *    waits, the instant is settled (a delivery point, also when nothing is
 *    pending), then the highest-priority ready thread runs, or time jumps.
 *    With the count above zero it takes one and returns at once (no
 *    delivery point). Returns the id, or a negative kernel error (-408
 *    unknown semaphore, -425 deleted while waiting; -1 outside a thread or
 *    in a handler).
 *  - Kern_SemaSignal readies the waiter that waited first, or counts one
 *    up. From a thread it may switch to a strictly higher-priority thread at
 *    once (8.1); from a handler or an event the switch waits for the end of
 *    the delivery point. Not a delivery point.
 *  - Kern_SemaPoll takes one if it can (the id), else -419; never waits.
 */
s32 Kern_SemaCreate(s32 initCount, s32 maxCount, u32 attr, u32 option);
s32 Kern_SemaDelete(s32 id);
s32 Kern_SemaWait(s32 id);
s32 Kern_SemaSignal(s32 id);
s32 Kern_SemaPoll(s32 id);

/*
 * A library wait (the specification, 6): a library function whose own code
 * waits in a busy loop on the PS2 (the polled-wait table's `wait` records). It keeps the
 * processor: no thread of equal or lower priority runs while it waits. Write
 * it as
 *
 *     Kern_LibWaitArrive();
 *     while (!done) {
 *         Kern_LibWaitStep();
 *     }
 *
 * - Kern_LibWaitArrive: the call reached the loop's head other than by going
 *   round (it fell into it). One delivery point: the due events fire, the
 *   rounds run (the IOP, the SIF, the interrupts), a requested thread switch
 *   is made. No time moves; no w count. A function with `entry` in the polled-wait table
 *   (sceGsSyncPath) arrives at its entry, whether or not it loops.
 * - Kern_LibWaitStep: one going-round of the loop. One w count for the
 *   calling thread, the poll flag cleared, the jump (time
 *   goes to the next event if the instant is settled), then a delivery point.
 * In a handler, an arrival does nothing and a step moves time and fires the
 * due events but delivers nothing (4.4).
 */
void Kern_LibWaitArrive(void);
void Kern_LibWaitStep(void);

/*
 * The wait of an RPC answered at once. A library client (libcdvd,
 * libmc, libpad2, fileio, ...) makes an RPC that the PS2's IOP answers at the
 * request; the platform's version has done the service's work natively and
 * then calls this once for each RPC the PS2's client makes, in its place:
 *  - a semaphore is created for the call (it takes the lowest free id for the
 *    call's length, as the SDK's client's does) and the reply's RPC_END
 *    packet is queued on the SIF0 queue, behind any packets already there;
 *  - the thread waits on it: a blocking wait, so a delivery point, where the
 *    SIF moves the packet (one per round) and the SIF's DMAC handler (channel
 *    5) signals the semaphore, in the order of 4.2 with the other
 *    interrupts;
 *  - the semaphore is deleted.
 * The trace sees `w1 t0 s0` when nothing else was ready (8). Another thread
 * of the same priority that was ready runs first (8). Returns 0, or a
 * negative value when the SIF's RPC layer is not set up (sceSifInitRpc) or
 * no semaphore is free; the caller then fails as the PS2's client does.
 */
s32 Kern_RpcWait(void);

/* ---------------------------------------------------------------- the SIF */

/*
 * The registry of IOP services (the specification, 7.1). The game's
 * sceSifBindRpc and sceSifCallRpc (the kernel's) reach a service by its
 * id. A module registers each service it provides before the game runs (from
 * a function the kernel calls at start-up: Kern_RegisterServices below).
 *
 * call(): the request of sceSifCallRpc. `send` is the EE buffer the game
 * passed (ssize bytes), `recv` the EE buffer the reply goes to (rsize bytes).
 * Return KERN_SIF_ANSWERED when the reply is in `recv` now (an HLE service:
 * the RPC_END packet is queued at once), or KERN_SIF_LATER when the service
 * answers later (an IOP module's thread, at a later IOP catch-up): it then
 * calls Kern_SifAnswer(c->token) once the reply is written, and the RPC_END
 * packet is queued then. A bind reaches no function: an unknown id gets a
 * stub service, as in the original game.
 */
typedef struct KernSifCall {
    u32 service;     /* the service id */
    s32 fno;         /* the function number */
    u32 mode;        /* the client's mode (bit 0: no wait) */
    void *send;
    s32 ssize;
    void *recv;
    s32 rsize;
    u32 token;       /* for Kern_SifAnswer */
} KernSifCall;

enum { KERN_SIF_ANSWERED = 1, KERN_SIF_LATER = 0 };

/*
 * server, buff: the IOP addresses a bind answers with (the client's words 9
 * and 5, which the game can see in its own clients). 0 for an HLE service:
 * the kernel gives each the original's (in the original's order of
 * registration). An IOP module's service (snd's backend) gives its own.
 */
typedef struct KernSifService {
    u32 id;
    const char *name;
    int (*call)(KernSifCall *c);
    u32 server, buff;
} KernSifService;

void Kern_SifRegister(const KernSifService *service);
void Kern_SifAnswer(u32 token);

/*
 * An IOP-to-EE SIF command (an HLE service's: fileio's answers, cdvd's
 * callbacks): cid and a payload of at most 112 bytes, queued on the SIF0
 * queue behind the packets already there; sent from a service's call(), it
 * follows that call's RPC_END. When it reaches the EE (a delivery point, the
 * SIF's DMAC handler) the handler the game's libraries set for cid with
 * sceSifAddCmdHandler(cid, fn, data) runs as fn(packet, data): packet is
 * the SDK's whole packet, a 16-byte header (byte 0 the packet's size, 16 +
 * size; word 2 cid; word 3 0) and then the payload. It runs in an interrupt
 * handler, so it may only use the i- calls. No handler: nothing.
 */
typedef void (*KernSifCmdFn)(void *packet, void *data);
void Kern_SifSendCmd(u32 cid, const void *payload, u32 size);

/*
 * Each module's start-up hook, called by the kernel once, before the game's
 * main thread starts, after io's Io_Init and gfx's Gfx_Init (which the
 * kernel calls itself): register services, set VBlank hooks. The kernel calls
 * every hook in this list that is defined (weak symbols: a module defines its
 * own, and leaves the others alone). The host's IPC defines Kern_InitIpc
 * (Ipc_Init).
 */
void Kern_InitIo(void);
void Kern_InitGfx(void);
void Kern_InitSnd(void);
void Kern_InitVerify(void);
void Kern_InitIpc(void);

/* ------------------------------------------------------------- the IOP */

/*
 * The IOP. The kernel schedules it (the
 * specification, 7) by calling snd's functions:
 *  - at each delivery point's step 2a, IopKern_CatchUp() while IopKern_Busy()
 *    (the parking is snd's);
 *  - IopKern_Unpark() for every event that fires;
 *  - IopKern_Vblank(1) at the VBlank start's step 9, IopKern_Vblank(0) at its
 *    end;
 *  - SIF DMA to and from IOP memory through IopMem_Write, ReadMem and
 *    MemPtr; the iopheap service through IopKern_HeapAlloc and HeapFree; the
 *    reboot through IopKern_Reset; loadfile's buffer loads through
 *    IopKern_IrxName, HasNativeModule and LoadModule; the module services' calls
 *    through IopKern_RpcCall.
 * Until snd's are on main, pc/core/kernel/iop_default.c defines weak
 * stand-ins (no IOP: nothing runs; the original's heap).
 *
 * What snd calls back (its snd_kernel.h, through its glue file):
 *  - Kern_Now, Kern_EventAdd, Kern_EventCancel (above);
 *  - SndSys_Post: nothing to do on the frame clock (the next delivery point
 *    asks IopKern_Busy);
 *  - a module's RPC server: Kern_SifRegister with its server and buff;
 *  - the server's answer: copy the reply to the EE (the request's
 *    ee_receive, at once), then Kern_SifAnswer(token), where token is the
 *    request's `client` word as the kernel gave it.
 */

/*
 * Raw fibers, for snd's IOP threads; the EE's threads are built on
 * the same. A fiber is a host stack from the low pool (below 0x10000000)
 * and a saved context; the switch saves ebx, esi, edi, ebp, the x87
 * control word and MXCSR (and on Windows the thread information block's
 * exception list and stack bounds). Not ucontext or Win32 fibers: Windows'
 * fibers can't be given a stack at an address of ours, so both systems use
 * the same switch (fiber.c).
 *  - Kern_FiberCreate(fn, arg, stackSize): fn(arg) runs at the fiber's first
 *    switch; it must never return (switch away for good instead).
 *  - Kern_FiberSelf(): the running fiber (the run's first one, too).
 *  - Kern_FiberSwitch(to): saves the running fiber and runs `to`.
 */
typedef struct KFiber KernFiber;
KernFiber *Kern_FiberCreate(void (*fn)(void *arg), void *arg, u32 stackSize);
KernFiber *Kern_FiberSelf(void);
void Kern_FiberSwitch(KernFiber *to);

/* ---------------------------------------------------------- the VBlank */

/*
 * The VBlank's start, in order (the specification, 9). The kernel calls the
 * other modules' work itself:
 *   1 the count; 2 KERN_VB_PACE (the host's hook, real-time play only);
 *   3 the SPU2 advances to now (snd's Spu2_Advance);
 *   4-6 are not used in the product;
 *   7 KERN_VB_INPUT (the host's input glue, which hands the pads to io),
 *   then io's Io_PadVBlank; 8 gfx's Gfx_VBlankStart; 9 the IOP's VBlank
 *   (IopKern_Vblank(1)); 10 SetVSyncFlag's words (the CSR word from
 *   Gfx_GsCsrForVSyncFlag); 11 INTC cause 2; 12 the next events; 13 the
 *   stop at --frames, or at the player's quit (Ipc_QuitRequested).
 * VBlank end (1,270,000 ns after the start): KERN_VB_END (the host's hook:
 * the movie clock), gfx's Gfx_VBlankEnd, INTC cause 3, IopKern_Vblank(0).
 * A hook gets the VBlank's number n (at the end, the n of the VBlank that
 * ends). A hook runs inside the kernel's event: it must not wait (2).
 */
enum {
    KERN_VB_PACE = 0,       /* 2: real-time play: pacing to the host clock (host) */
    KERN_VB_INPUT,          /* 7, before io's pads: the host's input (Io_PadSetState) */
    KERN_VB_END,            /* the VBlank's end, first: the movie clock (host) */
    KERN_VB_STEPS
};
typedef void (*KernVblankFn)(u64 vblank);
void Kern_SetVblankHook(int step, KernVblankFn fn);

/* ------------------------------------------------------------ save states */

/* Data that belongs to the process, not to the game: a save state (state.h) skips these sections. The files of
 * the host seam get them whole (hostsec.h); a file that holds a handle of the host declares it with these. */
#define KERN_HOSTBSS __attribute__((section("hostbss")))
#define KERN_HOSTDAT __attribute__((section("hostdat")))

#endif /* MH2_KERNEL_KERN_H */
