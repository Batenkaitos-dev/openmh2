/* The native Capcom Sound Engine (cse_iop.h): CSE.IRX's IOP side.
 *
 * Specified at its interfaces from the original's recorder (--sd-record) and, up to the first key-on, from a trace of the
 * module running on the R3000A interpreter, which only the original game has now; since then
 * the voice engine (cse_eng.c) is written from the module's code (the ruling on question 15). The oracle is the original's
 * --sd-record stream of a route. This file is the rest of the module:
 * - the module's start creates the init thread (priority 30, the `thpri` default), which starts the THMN server
 *   thread, initialises libsd (sceSdInit, the SPU's two cores: every voice, the core registers, the switches),
 *   sets the transfer-end handlers and starts the COMM and STAT server threads;
 * - the COMM request 0x309 (init) makes the other six threads (the driver, the 4 ms tick, effects, voice
 *   transfers, SIF transfers, the poller), their three mailboxes, the 4 ms hard timer, and initialises the SPU
 *   again; requests 1 and 3 are queued (64 slots of 64 bytes), the reply's first word is the queue's depth;
 * - every 4 ms the tick thread sets bits of event flag 3; the poller reads every voice's envelope, takes the
 *   queued requests and wakes the driver, which carries them out on the SPU and wakes the poller again;
 * - the replies are the server's whole 1,024-byte IOP buffer: this module keeps its objects at the original's
 *   addresses (relative to the module's base), so the stale bytes are the original's.
 * What no observation has shown aborts with its name. */
#include "cse/cse_iop.h"
#include "cse/cse_eng.h"

#define OBS_BASE 0x16E300u
#define AT(abs) (sBase + ((abs) - OBS_BASE))

/* The original's layout (module at 0x16E300 there) */
enum {
    A_START = 0x16E300, A_INIT = 0x16E6B4, A_THMN = 0x16FDC4, A_COMMTH = 0x16F884, A_STATTH = 0x16F8EC,
    A_DRV = 0x16FC74, A_TICK = 0x16F1A0, A_EFF = 0x16EB90, A_VTR = 0x1702A0, A_SIF = 0x16F484, A_POLL = 0x16FB18,
    A_FCOMM = 0x16F954, A_FSTAT = 0x16F9F4, A_FTHMN = 0x16FE2C, A_TIMER = 0x16F160, A_TRANS = 0x173D0C,
    A_RBUF = 0x17619C, A_RECV = 0x1761DC, A_SRECV = 0x17617C, A_TRECV = 0x17684C,
    A_QUEUE = 0x1768B8, A_ARG_STAT = 0x17621C, A_ARG_COMM = 0x176228, A_ARG_POLL = 0x176234, A_ARG_DRV = 0x176240,
    A_ARG_TICK = 0x175B7C, A_ARG_EFF = 0x175A14, A_ARG_VTR = 0x176864, A_ARG_SIF = 0x175F8C, A_ARG_THMN = 0x17685C,
    A_STAGE = 0x175B8C, A_VMSG = 0x17686C, A_UMSG = 0x175F94,
    A_MSG0 = 0x175A1C, A_MSG1 = 0x175A48, A_MSG2 = 0x175A74, A_MSG3 = 0x175AA0,
    /* this file's: unobserved places in the module's image */
    A_SCR = 0x179800
};
#define SCR(n) AT(A_SCR + (n))
enum { S_OUT = 0, S_TPARAM = 0x10, S_EPARAM = 0x30, S_MPARAM = 0x40, S_RES = 0x50, S_REFER = 0x60, S_MSG = 0x90,
       S_TMP = 0xB0, S_CLK = 0xF0 };

#define EVF 3u             /* the module's event flag (the id it gets) */
#define QSLOTS 64u
#define SLOT 0x40u

static u32 sBase;
static u32 sEvf, sMbx[3];
static u32 sHead, sTail;     /* the queue */
static u32 sEffBusy;   /* the messages in use (bit n: slot n of 0x771C) */
static u32 sRounds;

static void Unobserved(const char *what)
{
    SndHost_Log(SND_LOG_ERR, "cse", "%s: never observed in the original game", what);
    SndLog_Fatal("cse", what);
}

/* OBSERVED OUTPUT, not code: the words of the module's data that the replies show past the request (offsets from the COMM
 * reply buffer): the argument blocks of the server threads (+0x80, +0x90, +0xA0 ...: a thread id, a name, a stack) and the table of
 * thread records (an id and a four-character-word name each: "SifRpcomThread" and so on, ASCII packed little-endian).
 * The EE receives the whole 1,024-byte reply buffer of every COMM call (the original's SIF DMA copies it), so these bytes are
 * part of the interface; the original's module leaves them there as the stack and data residue of its start-up.
 * Where read from: the village route, the replies of the first COMM call (fno 0x309)
 * and of the calls after it, as the original's --sd-record stream shows them, first seen in the
 * trace (Y lines) of the core running the interpreted module, which was removed with step 6b.
 * The check: the village run on the original game and on the core, comparing every request and the
 * whole reply of each call, by hash (equal through VBlank 120); a changed value shows there as the first differing call. */
static const struct { u16 off; u32 v; } sData[] = {
    {0x80, 6}, {0x84, 0x53544154}, {0x88, 0x170EEC}, {0x8C, 1}, {0x90, 0x434F4D4D}, {0x94, 0x170D9C}, {0x98, 0xC},
    {0xA0, 0x17106C}, {0xA4, 7}, {0xA8, 0xFFFFFFFF}, {0xAC, 8}, {0xB0, 1},
    {0xB4, 0x52666953}, {0xB8, 0x6F436370}, {0xBC, 0x68546D6D}, {0xC0, 0x64616572},
    {0xD0, 6}, {0xD4, 0x52666953}, {0xD8, 0x74536370}, {0xDC, 0x68547461}, {0xE0, 0x64616572},
    {0xF0, 7}, {0xF4, 0x54757053}, {0xF8, 0x61657268}, {0xFC, 0x20202064},
    {0x110, 8}, {0x114, 0x6D695448}, {0x118, 0x68547265}, {0x11C, 0x64616572},
    {0x130, 9}, {0x134, 0x65666645}, {0x138, 0x68547463}, {0x13C, 0x64616572},
    {0x150, 0xA}, {0x154, 0x61725456}, {0x158, 0x6854736E}, {0x15C, 0x64616572},
    {0x170, 0xB}, {0x174, 0x44666953}, {0x178, 0x6854616D}, {0x17C, 0x64616572},
    {0x190, 0xC}, {0x194, 0x44646E53}, {0x198, 0x68547672}, {0x19C, 0x64616572}};

static u32 R32(u32 a) { return IopMem_Read32(a); }
static void W32(u32 a, u32 v) { IopMem_Write32(a, v); }

/* ------------------------------------------------------------------- the kernel and libsd, as the module calls them */

u32 CseK(const char *name, u32 a0, u32 a1, u32 a2, u32 a3) { return IopNative_Call(name, a0, a1, a2, a3); }
u32 CseSd(u32 fn, u32 a, u32 b, u32 c, u32 d) { return IopNative_Sd(fn, a, b, c, d); }
#define K CseK
#define Sd CseSd

/* A libsd call the module brackets with CpuSuspendIntr/CpuResumeIntr. */
u32 CseSdW(u32 fn, u32 a, u32 b, u32 c, u32 d)
{
    K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
    u32 st = R32(SCR(S_OUT));
    u32 r = Sd(fn, a, b, c, d);
    K("CpuResumeIntr", st, 0, 0, 0);
    return r;
}
#define SdW CseSdW

u32 CseScr(u32 n) { return SCR(n); }
void CseSysClock(u32 stat) { (void)stat; K("SysClock2USec", SCR(S_CLK), SCR(S_OUT + 4), SCR(S_OUT + 8), 0); }

static void SetEvf(u32 bits)
{
    K("GetThreadId", 0, 0, 0, 0);
    K("SetEventFlag", sEvf, bits, 0, 0);
}

static void ClearEvf(u32 mask)
{
    K("GetThreadId", 0, 0, 0, 0);
    K("ClearEventFlag", sEvf, mask, 0, 0);
}

static void WaitEvf(u32 bits)
{
    K("GetThreadId", 0, 0, 0, 0);
    K("WaitEventFlag", sEvf, bits, 1, SCR(S_RES));
}

static u32 NewThread(u32 entry, u32 prio)
{
    u32 p = SCR(S_TPARAM);
    W32(p, 0x02000000);
    W32(p + 4, 0);
    W32(p + 8, entry);
    W32(p + 12, 0x800);
    W32(p + 16, prio);
    return K("CreateThread", p, 0, 0, 0);
}

static u32 NewMbx(void)
{
    W32(SCR(S_MPARAM), 0);
    W32(SCR(S_MPARAM) + 4, 0);
    return K("CreateMbx", SCR(S_MPARAM), 0, 0, 0);
}

/* ------------------------------------------------------------------- the threads */

/* An RPC server thread: its queue and server data are locals on its stack, at the original's offsets. */
static void Serve(u32 sid, u32 func, u32 buff, u32 th)
{
    u32 top = IopNative_StackTop(), sd = top - 0x7C, qd = top - 0x38;
    u32 args[7];
    K("sceSifInitRpc", 0, 0, 0, 0);
    K("sceSifSetRpcQueue", qd, th, 0, 0);
    args[0] = sd; args[1] = sid; args[2] = func; args[3] = buff; args[4] = 0; args[5] = 0; args[6] = qd;
    IopNative_CallN("sceSifRegisterRpc", args, 7);
    K("sceSifRpcLoop", qd, sid, func, buff);
}

static u32 ThmnThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    Serve(0x54484D4E, AT(A_FTHMN), AT(A_TRECV), 3);
    return 0;
}

static u32 CommThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    Serve(0x434F4D4D, AT(A_FCOMM), AT(A_RECV), 1);
    return 0;
}

static u32 StatThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    Serve(0x53544154, AT(A_FSTAT), AT(A_SRECV), 6);
    return 0;
}

static u32 TickThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    for (;;) {
        K("SleepThread", 0, 0, 0, 0);
        W32(AT(A_RBUF) + 0x9C, R32(AT(A_RBUF) + 0x9C) + 1);   /* the ticks counted */
        SetEvf(1);
        SetEvf(0x40);
    }
    return 0;
}

/* The 4 ms hard timer's handler: wakes the tick thread, counts, and asks for the next 4 ms. */
static u32 TimerHandler(u32 arg, u32 a1, u32 a2, u32 a3)
{
    (void)a1; (void)a2; (void)a3;
    W32(arg, R32(arg) + 1);
    IopKern_Call(NULL, "iWakeupThread", 8, 0, 0, 0);
    return 0x24000;
}

static u32 sVtrCur;   /* the message the transfer under way belongs to (the module keeps it in the work-area record at 0xA740) */

/* The transfer-end handler of the voice transfers (0x5A0C; libsd calls it with the channel and the work-area record): frees the transfer's
 * message, sets the work area's bit (4 for channel 0, 8 for channel 1) and wakes the thread that posted the message if it sleeps. */
static u32 TransHandler(u32 ch, u32 arg, u32 a2, u32 a3)
{
    u32 msg = sVtrCur;
    (void)arg; (void)a2; (void)a3;
    IopKern_Call(NULL, "CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
    sVtrCur = 0;
    if (msg)
        W32(msg + 8, 0);
    IopKern_Call(NULL, "CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
    IopKern_Call(NULL, "GetThreadId", 0, 0, 0, 0);
    IopKern_Call(NULL, "SetEventFlag", sEvf, ch == 0 ? 4 : 8, 0, 0);
    if (msg) {
        IopKern_Call(NULL, "iReferThreadStatus", R32(msg + 0xC), SCR(S_REFER), 0, 0);
        if (R32(SCR(S_REFER) + 8) == 4)
            IopKern_Call(NULL, "iWakeupThread", R32(msg + 0xC), 0, 0, 0);
    }
    return 0;
}

/* The poller (0x1818): every tick, the SPU's voices and mixer read (0x68B4), the engine's tick with the queued requests
 * (0x2D6C, through the pointer at 0x7F3C), then the driver's turn. */
static u32 PollThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    WaitEvf(0x40);
    for (;;) {
        ClearEvf(~0x40u);
        WaitEvf(0x80);
        ClearEvf(~0x80u);
        CseEng_Poll();
        CseEng_Callback(sRounds++);
        SetEvf(2);
        WaitEvf(0x40);
    }
    return 0;
}

/* 0x2240: the oldest queued request (the ring of 64 slots of 64 bytes at 0x85B8), taken between CpuSuspendIntr and CpuResumeIntr */
int CseDeq(u32 *w)
{
    int r = -1;
    K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
    if (sHead != sTail) {
        u32 slot = AT(A_QUEUE) + SLOT * (sHead % QSLOTS);
        K("memcpy", SCR(S_TMP), slot, SLOT, 0);
        for (u32 i = 0; i < 16; i++)
            w[i] = R32(SCR(S_TMP) + 4 * i);
        sHead++;
        r = (int)(sTail - sHead);
    }
    K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
    return r;
}

/* 0x708: an effect request's message to the effect thread, in the first of the eight slots (0x771C, 0x2C bytes each) that is free */
void CseEffMsg(u32 core, u32 mode, s32 depth, u32 delay, u32 fb, u32 end)
{
    u32 i, m;
    K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
    for (i = 0; i < 8 && (sEffBusy >> i & 1); i++)
        ;
    if (i < 8)
        sEffBusy |= 1u << i;
    K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
    if (i == 8)
        Unobserved("all eight messages of the effect thread in use");
    m = AT(A_MSG0) + 0x2C * i;
    W32(m + 0x10, core);
    W32(m + 0x14, core);
    W32(m + 0x18, mode);
    IopMem_Write16(m + 0x1C, (u16)depth);
    IopMem_Write16(m + 0x1E, (u16)depth);
    W32(m + 0x20, delay);
    W32(m + 0x24, fb);
    W32(m + 0x28, end);
    K("GetThreadId", 0, 0, 0, 0);
    W32(m + 8, 2);
    K("SendMbx", sMbx[0], m, 0, 0);
}

/* The driver (0x1974): the shadow written to the SPU (0x6878), then a wait of 4 us. */
static u32 DriverThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    SetEvf(0x80);
    for (;;) {
        WaitEvf(2);
        ClearEvf(~2u);
        CseEng_Drive();
        K("DelayThread", 4, 0, 0, 0);
        SetEvf(0x80);
    }
    return 0;
}

/* 0xB1C: the effect volumes (EVOLL, EVOLR of a core) raised to depth in steps of 8, a wait after each step */
static void EffRampUp(u32 core, s32 depth)
{
    s32 a = 0, b = 0;
    while (a < depth || b < depth) {
        a += 8;
        Sd(SD_SETPARAM, (core | 0xB80) & 0xFFFF, (u32)a & 0xFFFF, 0, 0);
        b += 8;
        Sd(SD_SETPARAM, (core | 0xC80) & 0xFFFF, (u32)b & 0xFFFF, 0, 0);
        K("DelayThread", 0x64, 0, 0, 0);
    }
    Sd(SD_SETPARAM, (core | 0xB80) & 0xFFFF, (u32)depth & 0xFFFF, 0, 0);
    Sd(SD_SETPARAM, (core | 0xC80) & 0xFFFF, (u32)depth & 0xFFFF, 0, 0);
}

/* 0xBC8: the effect volumes read, then moved to target in steps of 8 either way, a wait after each step */
static void EffRampTo(u32 core, s32 target)
{
    s32 a = (s32)(Sd(SD_GETPARAM, (core | 0xB80) & 0xFFFF, 0, 0, 0) & 0xFFFF);
    s32 b = (s32)(Sd(SD_GETPARAM, (core | 0xC80) & 0xFFFF, 0, 0, 0) & 0xFFFF);
    while (a != target || b != target) {
        if (a < target) { a += 8; if (target < a) a = target; }
        else if (target < a) { a -= 8; if (a < target) a = target; }
        if (b < target) { b += 8; if (target < b) b = target; }
        else if (target < b) { b -= 8; if (b < target) b = target; }
        Sd(SD_SETPARAM, (core | 0xB80) & 0xFFFF, (u32)a & 0xFFFF, 0, 0);
        Sd(SD_SETPARAM, (core | 0xC80) & 0xFFFF, (u32)b & 0xFFFF, 0, 0);
        K("DelayThread", 0x64, 0, 0, 0);
    }
}

/* The effect thread (0x890). Each message (0x2C bytes, in the eight slots from 0x771C) is an effect setting of one core: its core at +0x10,
 * the sceSdEffectAttr from +0x14 (core, mode, the two depths as halfwords, delay, feedback), the end address at +0x28. A setting that
 * changes the mode or the end address (or asks for the work area to be cleared: mode bit 0x100) lowers the effect volume, switches the
 * effect off, moves the end address, clears the work area, waits, writes the attributes with depth 0 and switches the effect on when the mode
 * is not 0, then raises the volume to the depth; an unchanged one only moves the volume. */
static u32 EffThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 var = IopNative_StackTop() - 0x24, loc = IopNative_StackTop() - 0x38;
    (void)a0; (void)a1; (void)a2; (void)a3;
    for (;;) {
        K("ReceiveMbx", var, sMbx[0], 0, 0);
        u32 msg = R32(var), i = (msg - AT(A_MSG0)) / 0x2C, core, same, mode, eea;
        s32 depth;
        if (msg < AT(A_MSG0) || i > 7 || (msg - AT(A_MSG0)) % 0x2C)
            Unobserved("an effect message of the sound engine other than the slots");
        core = R32(msg + 0x10);
        Sd(SD_GETEFFECTATTR, core, loc, 0, 0);
        eea = Sd(SD_GETADDR, 0x1D00 | core, 0, 0, 0);
        mode = R32(msg + 0x18);
        same = mode == R32(loc + 4) && R32(msg + 0x28) == eea && !(mode & 0x100);
        if (!same) {
            EffRampTo(core, 0);
            Sd(SD_SETCOREATTR, 2 | core, 0, 0, 0);
            Sd(SD_SETADDR, 0x1D00 | core, R32(msg + 0x28), 0, 0);
            WaitEvf(4);   /* 0x58F0: the work area (the first of two; the second is never free) */
            K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
            K("ReferEventFlagStatus", sEvf, SCR(S_REFER), 0, 0);
            ClearEvf(~4u);
            K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
            Sd(SD_CLEAREFFECT, core, 0, mode, 0);
            SetEvf(4);    /* 0x59C4 */
            W32(msg + 0x18, mode & ~0x100u);
        }
        K("DelayThread", 0x64, 0, 0, 0);
        K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
        depth = (s16)IopMem_Read16(msg + 0x1C);
        if (!same) {
            IopMem_Write16(msg + 0x1C, 0);
            IopMem_Write16(msg + 0x1E, 0);
            Sd(SD_SETEFFECTATTR, core, msg + 0x14, 0, 0);
        }
        Sd(SD_SETCOREATTR, 2 | core, R32(msg + 0x18) != 0, 0, 0);
        W32(msg + 8, 0);
        sEffBusy &= ~(1u << i);
        K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
        K("DelayThread", 0x64, 0, 0, 0);
        if (same)
            EffRampTo(core, depth);
        else
            EffRampUp(core, depth);
    }
    return 0;
}

/* The upload messages (0x7C94: sixteen of 0x1C bytes: state at +8, 0 free, 1 taken, 2 sent; the posting thread's id at +0xC; the EE address,
 * the SPU address and the size at +0x10, +0x14, +0x18) and the counters of the free and used ones (0x7E58, 0x7E54, which the STAT server
 * reports). The voice-transfer messages (0x856C: two) have the same shape; the EE address field holds the staging buffer. */
#define CNT_USED AT(0x176154)
#define CNT_FREE AT(0x176158)

/* 0x1010: an upload request (COMM 0x30000000) becomes a message to the SIF-transfer thread */
static u32 UploadMsg(u32 ee, u32 spu, u32 size)
{
    u32 i, m = AT(A_UMSG);
    K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
    for (i = 0; i < 16; i++, m += 0x1C)
        if (R32(m + 8) == 0) {
            W32(m + 8, 1);
            W32(CNT_FREE, R32(CNT_FREE) - 1);
            W32(CNT_USED, R32(CNT_USED) + 1);
            break;
        }
    K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
    if (i == 16)
        return (u32)-1;
    K("memset", AT(A_STAGE), 0, 0x400, 0);
    W32(m + 0x10, ee);
    W32(m + 0x14, spu);
    W32(m + 0x18, size);
    W32(m + 0xC, K("GetThreadId", 0, 0, 0, 0));
    W32(m + 8, 2);
    K("SendMbx", sMbx[2], m, 0, 0);
    return 0;
}

/* 0x1E88: a piece of an upload becomes a message to the voice-transfer thread (the first of two slots that is free) */
static u32 PieceMsg(u32 src, u32 dst, u32 n)
{
    u32 i, m = AT(A_VMSG);
    K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
    for (i = 0; i < 2; i++, m += 0x1C)
        if (R32(m + 8) == 0) {
            W32(m + 8, 1);
            break;
        }
    K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
    if (i == 2)
        return (u32)-1;
    W32(m + 0x10, src);
    W32(m + 0x14, dst);
    W32(m + 0x18, n);
    W32(m + 0xC, K("GetThreadId", 0, 0, 0, 0));
    W32(m + 8, 2);
    K("SendMbx", sMbx[1], m, 0, 0);
    return 0;
}

/* The voice-transfer thread (0x1FA0): each message is a piece of an upload in the staging buffer, DMAed to the SPU's memory. */
static u32 VtrThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 var = IopNative_StackTop() - 0x24;
    (void)a0; (void)a1; (void)a2; (void)a3;
    for (;;) {
        K("ReceiveMbx", var, sMbx[1], 0, 0);
        u32 msg = R32(var);
        WaitEvf(4);   /* 0x58F0: the work area */
        K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
        K("GetThreadId", 0, 0, 0, 0);
        K("ReferEventFlagStatus", sEvf, SCR(S_REFER), 0, 0);
        ClearEvf(~4u);
        K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
        sVtrCur = msg;
        while (Sd(SD_VTSTATUS, 0, 0, 0, 0) != 1)
            K("DelayThread", 0x64, 0, 0, 0);
        K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
        if ((int)IopNative_Sd5(SD_VOICETRANS, 0, 0, R32(msg + 0x10), R32(msg + 0x14), R32(msg + 0x18)) < 0)
            Unobserved("a voice transfer that fails (0x1FA0 retries and stops it)");
        K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
    }
    return 0;
}

/* The SIF-transfer thread (0x1184): an upload is fetched from the EE's memory 1 KiB at a time into the staging buffer, each
 * piece handed to the voice-transfer thread and waited for. */
static u32 SifThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 top = IopNative_StackTop(), var = top - 0x24, rd = top - 0x40;
    (void)a0; (void)a1; (void)a2; (void)a3;
    for (;;) {
        K("ReceiveMbx", var, sMbx[2], 0, 0);
        u32 msg = R32(var), size, off = 0;
        K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
        SetEvf(0x20);
        SetEvf(0x10);
        W32(msg + 8, 0);
        W32(CNT_FREE, R32(CNT_FREE) + 1);
        W32(CNT_USED, R32(CNT_USED) - 1);
        K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
        size = R32(msg + 0x18);
        while (size) {
            u32 n = size > 0x400 ? 0x400 : size;
            if ((int)K("sceSifGetOtherData", rd, R32(msg + 0x10) + off, AT(A_STAGE), n) < 0)
                Unobserved("an EE transfer that fails (0x1184 retries)");
            size -= n;
            off += n;
            if (size == 0)
                ClearEvf(~0x10u);
            if ((int)PieceMsg(AT(A_STAGE), R32(msg + 0x14), n) < 0)
                Unobserved("both voice-transfer messages in use (0x1184 retries)");
            K("SleepThread", 0, 0, 0, 0);
            W32(msg + 0x14, R32(msg + 0x14) + n);
        }
        K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
        ClearEvf(~0x20u);
        K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
    }
    return 0;
}

/* ------------------------------------------------------------------- the RPC servers */

static u32 CommFn(u32 fno, u32 buff, u32 size, u32 unused)
{
    (void)size; (void)unused;
    switch (fno) {
    case 0x309: {   /* init: the driver's threads, the mailboxes, the hard timer, the SPU again */
        u32 t;
        t = NewThread(AT(A_DRV), 36);
        K("StartThread", t, AT(A_ARG_DRV), 0, 0);
        t = NewThread(AT(A_TICK), 30);
        K("StartThread", t, AT(A_ARG_TICK), 0, 0);
        {
            u32 id = K("AllocHardTimer", 1, 0x20, 1, 0);
            K("SetupHardTimer", id, 1, 0, 1);
            K("SetTimerHandler", id, 0x24000, AT(A_TIMER), AT(A_ARG_TICK));
            sMbx[0] = NewMbx();
            sEffBusy = 0;
            CseEffMsg(0, 0x100, 0, 0, 0, 0x1FFFFF);   /* 0x558: each core's effect off, work area cleared */
            CseEffMsg(1, 0x100, 0, 0, 0, 0x1FFFFF);
            t = NewThread(AT(A_EFF), 34);
            K("StartThread", t, AT(A_ARG_EFF), 0, 0);
            sMbx[1] = NewMbx();
            t = NewThread(AT(A_VTR), 35);
            K("StartThread", t, AT(A_ARG_VTR), 0, 0);
            sMbx[2] = NewMbx();
            for (u32 k = 0; k < 16; k++)   /* 0xF4C: the upload messages free, the counters of free and used ones */
                W32(AT(A_UMSG) + 0x1C * k + 8, 0);
            for (u32 k = 0; k < 2; k++)
                W32(AT(A_VMSG) + 0x1C * k + 8, 0);
            W32(CNT_USED, 0);
            W32(CNT_FREE, 16);
            t = NewThread(AT(A_SIF), 33);
            K("StartThread", t, AT(A_ARG_SIF), 0, 0);
            t = NewThread(AT(A_POLL), 37);
            K("StartThread", t, AT(A_ARG_POLL), 0, 0);
            snd_memset(IopMem_Ptr(AT(A_QUEUE)), 0, 0x1000);
            sHead = sTail = 0;
            CseEng_Init(0x2000000);   /* the engine's reset: its last two calls are the effect requests of the two cores */
            K("StartHardTimer", id, 0, 0, 0);
        }
        break;
    }
    case 1:
    case 3: {   /* 0x2A9C: the request's first word says what it is (the function number is not looked at) */
        u32 op = R32(buff), r = 0;
        if ((op >= 0x10000000 && op <= 0x1000000D && op != 0x10000005)) {   /* a request for the engine: queued */
            K("CpuSuspendIntr", SCR(S_OUT), 0, 0, 0);
            if (sTail - sHead == QSLOTS) {   /* 0x2170: the ring is full: three error prints (Kprintf, no effect) and -1 */
                r = (u32)-1;
            } else {
                K("memcpy", AT(A_QUEUE) + SLOT * (sTail % QSLOTS), buff, SLOT, 0);
                sTail++;
                r = sTail - sHead;
            }
            K("CpuResumeIntr", R32(SCR(S_OUT)), 0, 0, 0);
        } else if (op == 0x30000000) {   /* an upload: its message to the SIF-transfer thread */
            r = UploadMsg(R32(buff + 8), R32(buff + 12), R32(buff + 16));
        } else if (op != 0) {
            Unobserved("a COMM request of another kind (the module prints an error)");
        }
        W32(AT(A_RBUF), r);
        break;
    }
    default:
        Unobserved("a COMM function number");
    }
    return AT(A_RBUF);
}

/* 0x2BEC: 0x20000000 the state of the uploads (the event flag's bits 5 and 4, the counters of used and free upload messages); the other
 * requests are the engine's (cse_eng.c). */
static u32 StatFn(u32 fno, u32 buff, u32 size, u32 unused)
{
    (void)size; (void)unused;
    u32 reply = AT(A_RBUF) - 0x40;
    if (fno != 2)
        Unobserved("a STAT function number");
    if (R32(buff) == 0x20000000) {
        u32 b8, b9;
        K("ReferEventFlagStatus", sEvf, SCR(S_REFER), 0, 0);
        b8 = (R32(SCR(S_REFER) + 12) >> 5) & 1;
        K("ReferEventFlagStatus", sEvf, SCR(S_REFER), 0, 0);
        b9 = (R32(SCR(S_REFER) + 12) >> 4) & 1;
        IopMem_Write8(reply + 8, (u8)b8);
        IopMem_Write8(reply + 9, (u8)b9);
        IopMem_Write8(reply + 0xA, (u8)R32(CNT_USED));
        IopMem_Write8(reply + 0xB, (u8)R32(CNT_FREE));
        W32(reply, 0);
        return reply;
    }
    return CseEng_Stat(buff, reply);
}

static u32 ThmnFn(u32 fno, u32 buff, u32 size, u32 unused)
{
    (void)fno; (void)buff; (void)size; (void)unused;
    Unobserved("the THMN server (the game never calls it)");
    return 0;
}

/* ------------------------------------------------------------------- the start */

/* The init thread. */
static u32 InitThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 t;
    (void)a0; (void)a1; (void)a2; (void)a3;
    t = NewThread(AT(A_THMN), 20);
    K("StartThread", t, AT(A_ARG_THMN), 0, 0);
    Sd(SD_INIT, 0, 0, 0, 0);
    {   /* OBSERVED OUTPUT, not code: the 64 bytes the first voice transfer (VoiceTrans mode 8, to the SPU at 0x5010) writes:
         * what the init thread's stack held there in the original game. Read from the SPU register writes (the DMA'd words) of the
         * village route, MH2_SND_LOG=4 at the first VoiceTrans; the SPU holds them, and the silent loop block of every released
         * voice plays from 0x5010 (the voice-retirement key-on), so they decide what the SPU outputs. 
         * The original's stream compares every register write and libsd call. */
        static const u16 blk[32] = {0x500, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x200, 0, 0, 0xFDC4, 0x16, 0xE6E0, 0x16, 0x14, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0xF00, 0x1FC0};
        for (u32 i = 0; i < 32; i++)
            IopMem_Write16(SCR(S_TMP) + 2 * i, blk[i]);
    }
    IopNative_Sd5(SD_VOICETRANS, 0, 8, SCR(S_TMP), 0x5010, 0x10);
    CseEng_SpuInit();   /* 0x5B30 */
    Sd(SD_VTSTATUS, 0, 0, 0, 0);
    for (u32 ch = 0; ch < 2; ch++) {
        Sd(SD_GETCOREATTR, 4 + ch, 0, 0, 0);
        SdW(SD_SETCOREATTR, 4 + ch, 0, 0, 0);
        Sd(SD_GETTRANSARG, ch, 0, 0, 0);
        Sd(SD_SETTRANSINTR, ch, AT(A_TRANS), 0, 0);
        SdW(SD_SETCOREATTR, 4 + ch, 1, 0, 0);
        if (ch == 0)
            Sd(SD_VTSTATUS, 1, 0, 0, 0);
    }
    W32(SCR(S_EPARAM), 2);
    W32(SCR(S_EPARAM) + 4, 0);
    W32(SCR(S_EPARAM) + 8, 4);
    sEvf = K("CreateEventFlag", SCR(S_EPARAM), 0, 0, 0);
    t = NewThread(AT(A_COMMTH), 32);
    K("StartThread", t, AT(A_ARG_COMM), 0, 0);
    t = NewThread(AT(A_STATTH), 31);
    K("StartThread", t, AT(A_ARG_STAT), 0, 0);
    return 0;
}

static u32 StartFn(u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 t;
    (void)a0; (void)a1; (void)a2; (void)a3;
    t = NewThread(AT(A_INIT), 30);
    K("StartThread", t, 0, 0, 0);
    return 0;
}

static u32 Setup(u32 base, u32 size)
{
    (void)size;
    sBase = base;
    sEvf = sHead = sTail = 0;
    sEffBusy = 0;
    sRounds = 0;
    for (u32 i = 0; i < sizeof(sData) / sizeof(sData[0]); i++)
        IopMem_Write32(AT(A_RBUF) + sData[i].off, sData[i].v);
    IopNative_Map(AT(A_START), StartFn);
    IopNative_Map(AT(A_INIT), InitThread);
    IopNative_Map(AT(A_THMN), ThmnThread);
    IopNative_Map(AT(A_COMMTH), CommThread);
    IopNative_Map(AT(A_STATTH), StatThread);
    IopNative_Map(AT(A_DRV), DriverThread);
    IopNative_Map(AT(A_TICK), TickThread);
    IopNative_Map(AT(A_EFF), EffThread);
    IopNative_Map(AT(A_VTR), VtrThread);
    IopNative_Map(AT(A_SIF), SifThread);
    IopNative_Map(AT(A_POLL), PollThread);
    IopNative_Map(AT(A_FCOMM), CommFn);
    IopNative_Map(AT(A_FSTAT), StatFn);
    IopNative_Map(AT(A_FTHMN), ThmnFn);
    IopNative_Map(AT(A_TIMER), TimerHandler);
    IopNative_Map(AT(A_TRANS), TransHandler);
    return A_START - OBS_BASE;
}

const IopNativeModule g_cse_iop = {"Capcom Sound Engine", Setup};
