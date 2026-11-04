/* The native CRI_ADX_Driver: the IOP side of CRI ADX's players (adx_iop.h).
 *
 * Specified at its interface from the original's recorder
 * on the village route, never from the module's code:
 * - the module takes the same IOP heap blocks as the original's (the image's size, the start thread's stack, the
 *   stacks of the three threads it creates) and the addresses of its objects are the original's, relative to the
 *   module's base (the RPC replies hold them, and the libsd calls name the staging buffer and the callback);
 * - RPC server 0x90000200 (api/adxt.c is the client): fno 2 (a status block: its EE and IOP buffers), 0x403, 0x40C
 *   (the output rate, 48000: libsd's transfer callback for core 1 and the looping BlockTrans of the staging buffer,
 *   the master volumes), 0x422 (a ring: its IOP buffer, the PCM from 0x100 in, 0x4000 bytes), 0x400 (the ring's
 *   proxy: its EE status), 0x40A, 0x408 (an output of rings);
 * - each VBlank, after the end of it (WaitVblankEnd), the two status blocks the EE sent to their IOP buffers:
 *   block 0 holds the EE's announcements of PCM put into rings (word 0 their count, entries of 16 bytes from 0x10:
 *   0x100, the ring's proxy, the data address, its bytes); the IOP's echo replaces them with its answers (0, the
 *   ring's EE status, the address, the bytes) of what the callbacks consumed since the last echo (one entry per
 *   contiguous range, a ring at a time); block 1 holds the outputs' commands (2 play, 4 rate, 5 level, 8 stop, 9
 *   pan, 13 balance; the echo has its count 0); both echoes have the last word (0x87C) one more, and go to the EE
 *   by sceSifSetDma;
 * - core 1's transfer callback (a half of the 0x1000-byte staging buffer has played: libsd's third argument is
 *   the half) fills that half with the next 512 samples of every playing output: each channel's ring gives
 *   0x400 bytes, the samples times the gains the output's level, the channel's pan and the balance give each side
 *   (Gain), >> 8, the sum clipped to +-32767; the half gets the mix of the callback before (the buffer is one callback late),
 *   laid out as two blocks of 256 samples left then 256 right; then wakes the poll
 *   thread, which asks libsd for core 1's BlockTransStatus.
 * What no observation has shown aborts with its name (SndLog_Fatal).
 */
#include "cse/adx_iop.h"

/* The original's layout, as its replies and libsd calls show it (the module's base there: 0x17ED00). */
#define OBS_BASE 0x17ED00u
#define AT(abs) (sBase + ((abs) - OBS_BASE))
enum {
    A_START = 0x17F27C, A_MAIN = 0x17ED88, A_DTX = 0x17F4D4, A_RPC = 0x17F420, A_PSM = 0x1830C0, A_CB = 0x183570,
    A_CBARG = 0x18B0C4, A_RPCBUF = 0x18C200, A_STAT = 0x18C300, A_OUT = 0x18C7E0, A_SD = 0x18C770,
    A_STAGE = 0x18DC80, A_SJ = 0x1922C0, A_SJX = 0x192E00,
    /* this file's: unobserved places in the module's image */
    A_QD = 0x18C740, A_TPARAM = 0x18C400, A_DMA = 0x18C440
};
#define STAT_SIZE 0x44u
#define OUT_SIZE 0x1Cu
#define SJ_SIZE 0x30u
#define SJX_SIZE 0x14u
#define BLK_SIZE 0x880u
#define STAGE_SIZE 0x1000u
#define RING_DATA 0x100u
#define RING_SIZE 0x4000u
#define CB_BYTES 0x400u         /* the bytes of each channel a callback consumes: 512 samples */
#define MAX_RING 6
#define MAX_OUT 8
#define MAX_RANGE 64

typedef struct Ring {
    int used;
    u32 sj, sjx, buf, ee_stat;
    u32 rp, held;                /* the read position (an IOP address in the data) and the bytes held */
} Ring;

typedef struct Out {
    int nch;
    int ring[2];
    u32 play, stop, level, rate;
    s32 bal;                     /* command 13's first word: a balance (-128: none) */
    s32 pan[2];
} Out;

typedef struct Range {
    int ring;
    u32 addr, n;
} Range;

static u32 sBase;
static struct {
    int used;
    u32 ee, iop;
} sStat[2];
static int sNStat;
static Ring sRing[MAX_RING];
static int sNRing;
static Out sOut[MAX_OUT];
static int sNOut;
static Range sCons[MAX_RANGE];
static int sNCons;
static int sPsm;                /* the poll thread's id, 0 before it exists */

static void Unobserved(const char *what)
{
    SndHost_Log(SND_LOG_ERR, "adx", "%s: never observed in the original game", what);
    SndLog_Fatal("adx", what);
}

static u32 R32(u32 a) { return IopMem_Read32(a); }
static void W32(u32 a, u32 v) { IopMem_Write32(a, v); }

/* ---------------------------------------------------------------- the rings */

static u32 RingStart(const Ring *r) { return r->buf + RING_DATA; }

static int RingBySj(u32 sj)
{
    for (int i = 0; i < sNRing; i++)
        if (sRing[i].used && sRing[i].sj == sj)
            return i;
    Unobserved("a ring named that was not made");
    return -1;
}

static int RingBySjx(u32 sjx)
{
    for (int i = 0; i < sNRing; i++)
        if (sRing[i].used && sRing[i].sjx == sjx)
            return i;
    Unobserved("an announcement for a ring that has no proxy");
    return -1;
}

/* What the callbacks (and a stop's flush) took from a ring, for the next echo's answers. */
static void Consumed(int ring, u32 addr, u32 n)
{
    for (int i = sNCons - 1; i >= 0; i--) {
        if (sCons[i].ring != ring)
            continue;
        if (sCons[i].addr + sCons[i].n == addr) {
            sCons[i].n += n;
            return;
        }
        break;
    }
    if (sNCons == MAX_RANGE)
        Unobserved("more than 64 consumed ranges between two echoes");
    sCons[sNCons++] = (Range){ring, addr, n};
}

/* Takes n bytes from a ring's read position (n <= held), in pieces that do not cross the ring's end. */
static void Take(int ring, u32 n, u32 *pieces, u32 *np)
{
    Ring *r = &sRing[ring];
    u32 end = RingStart(r) + RING_SIZE;
    *np = 0;
    while (n) {
        u32 k = end - r->rp < n ? end - r->rp : n;
        pieces[(*np)++] = r->rp;
        pieces[(*np)++] = k;
        Consumed(ring, r->rp, k);
        r->rp += k;
        if (r->rp == end)
            r->rp = RingStart(r);
        r->held -= k;
        n -= k;
    }
}

/* A stop's flush: everything the ring holds is freed. */
static void Flush(const Out *o)
{
    for (int c = 0; c < o->nch; c++) {
        Ring *r = &sRing[o->ring[c]];
        u32 pieces[4], np;
        if (r->held)
            Take(o->ring[c], r->held, pieces, &np);
    }
}

/* ---------------------------------------------------------------- the status blocks */

static void SendBlock(int kind)
{
    W32(AT(A_DMA), sStat[kind].iop);
    W32(AT(A_DMA) + 4, sStat[kind].ee);
    W32(AT(A_DMA) + 8, BLK_SIZE);
    W32(AT(A_DMA) + 12, 0);
    IopNative_Call("sceSifSetDma", AT(A_DMA), 1, 0, 0);
}

static void Announce(const u32 *e)
{
    int ri = RingBySjx(e[1]);
    Ring *r = &sRing[ri];
    u32 addr = e[2], n = e[3], start = RingStart(r), end = start + RING_SIZE;
    if (addr < start || addr + n > end)
        Unobserved("an announcement outside its ring");
    if (!r->held) {
        r->rp = addr;
    } else {
        u32 w = r->rp + r->held;
        if (w >= end)
            w -= RING_SIZE;
        if (w != addr)
            Unobserved("an announcement that does not follow the ring's data");
    }
    r->held += n;
    if (r->held > RING_SIZE)
        Unobserved("a ring over full");
}

static void Command(const u32 *e)
{
    u32 cmd = e[0], oa = e[1], a = e[2], b = e[3];
    if (oa < AT(A_OUT) || (oa - AT(A_OUT)) % OUT_SIZE || (oa - AT(A_OUT)) / OUT_SIZE >= (u32)sNOut)
        Unobserved("a command for an output that was not made");
    Out *o = &sOut[(oa - AT(A_OUT)) / OUT_SIZE];
    switch (cmd) {
    case 2: o->play = a; break;
    case 4:
        o->rate = a;            /* no effect on the mix: the original game gave the same staging buffers at 44100,
                                 * 32000, 24000, 22050, 16000, 12000, 96000, 48001 and 0) */
        break;
    case 5: o->level = b; break;
    case 8:
        o->stop = a;
        if (a)
            Flush(o);
        break;
    case 9:
        if (a > 1)
            Unobserved("a pan for a third channel");
        o->pan[a] = (s32)b;
        break;
    case 13: o->bal = (s32)a; break;   /* its second word (0 or 15 in the game) changes nothing */
    default: Unobserved("an output command");
    }
}

/* One VBlank's work: the blocks the EE put in the IOP's buffers, their echoes to the EE. */
static void Tick(void)
{
    u8 *b0 = NULL;
    u32 *w;
    if (sNStat < 2)
        return;
    b0 = IopMem_Ptr(sStat[0].iop);
    w = (u32 *)b0;
    /* block 0: the announcements, then this echo's answers in their place */
    u32 nann = w[0];
    if (4 + 4 * nann > BLK_SIZE / 4)
        Unobserved("more announcements than the block holds");
    u32 ann[64 * 4];
    if (nann > 64)
        Unobserved("more than 64 announcements");
    for (u32 i = 0; i < nann; i++)
        for (int k = 0; k < 4; k++)
            ann[4 * i + (u32)k] = w[4 + 4 * i + (u32)k];
    for (u32 i = 0; i < nann; i++) {
        if (ann[4 * i] != 0x100)
            Unobserved("a block 0 entry that is not an announcement");
        Announce(&ann[4 * i]);
    }
    u32 na = 0;
    for (int r = 0; r < sNRing; r++)
        for (int i = 0; i < sNCons; i++)
            if (sCons[i].ring == r) {
                if (4 + 4 * (na + 1) > BLK_SIZE / 4)
                    Unobserved("more answers than the block holds");
                w[4 + 4 * na] = 0;
                w[4 + 4 * na + 1] = sRing[r].ee_stat;
                w[4 + 4 * na + 2] = sCons[i].addr;
                w[4 + 4 * na + 3] = sCons[i].n;
                na++;
            }
    sNCons = 0;
    w[0] = na;
    w[BLK_SIZE / 4 - 1]++;
    /* block 1: the commands, then the echo with their count 0 */
    u32 *c = (u32 *)IopMem_Ptr(sStat[1].iop);
    u32 ncmd = c[0];
    if (ncmd > 64)
        Unobserved("more than 64 commands");
    for (u32 i = 0; i < ncmd; i++)
        Command(&c[4 + 4 * i]);
    c[0] = 0;
    c[BLK_SIZE / 4 - 1]++;
    SendBlock(0);
    SendBlock(1);
}

/* ---------------------------------------------------------------- the callback */

static s32 Sat(s32 v)
{
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;   /* symmetric: seen at -32767 on the original game */
}

static s32 sPrev[2][512];       /* the mix of the callback before: the staging buffer takes it one callback late */

/* A channel's gain to a side, in 256ths of its sample. Observed on the original game with a grid of levels, pans and
 * balance words :
 * - the pan (-15 left to 15 right, other values clamped) gives the left side the numerator sPanLeft[pan + 15] and
 *   the right side sPanLeft[15 - pan], over 255 (a constant-power law: 255 cos and 255 sin of 3 degrees per step,
 *   floor);
 * - command 13's first word a is a balance (-128: none): for a > 0 the left side is scaled by (15 - a)/15 and for
 *   a < 0 the right side by (15 + |a|)/15 (zero from 15 on); its second word (0 or 15 in the game) changes nothing;
 * - the gain is the level (a signed word, clamped to 0..255) times those, rounded once to the nearest, and 256
 *   where that is 255 (the level 255 at a full pan);
 * - the staging buffer takes (sample * gain) >> 8 for each channel and side, summed and clipped to +-32767. */
static const u8 sPanLeft[31] = {255, 254, 253, 251, 249, 246, 242, 238, 232, 227, 220, 213, 206, 198, 189, 180,
                                170, 160, 149, 138, 127, 115, 103, 91, 78, 65, 53, 39, 26, 13, 0};

static u32 Gain(const Out *o, int c, int side)
{
    s32 pan = o->pan[c];
    u32 n, num, den = 255;
    if (pan < -15)
        pan = -15;
    if (pan > 15)
        pan = 15;
    n = sPanLeft[side ? 15 - pan : pan + 15];
    s32 lv = (s32)o->level;
    lv = lv < 0 ? 0 : lv > 255 ? 255 : lv;
    num = (u32)lv * n;
    if (o->bal != -128 && (side ? o->bal < 0 : o->bal > 0)) {
        u32 m = o->bal < 0 ? 0u - (u32)o->bal : (u32)o->bal;
        num *= m >= 15 ? 0 : 15 - m;
        den *= 15;
    }
    n = (2 * num + den) / (2 * den);
    return n == 255 ? 256 : n;
}

static void Fill(u32 half)
{
    s32 acc[2][512];
    snd_memset(acc, 0, sizeof(acc));
    for (int j = 0; j < sNOut; j++) {
        Out *o = &sOut[j];
        if (!o->play || o->stop)
            continue;
        int dry = 0;
        for (int c = 0; c < o->nch; c++)
            if (sRing[o->ring[c]].held < CB_BYTES) {
                if (sRing[o->ring[c]].held)
                    Unobserved("a ring that holds part of what a callback takes");
                dry++;
            }
        if (dry == o->nch)
            continue;           /* every ring of the output empty: silence, nothing consumed (seen with the
                                 * announcements of both rings held back for 8 and 20 echoes) */
        if (dry)
            Unobserved("one ring of an output empty while another holds data");
        for (int c = 0; c < o->nch; c++) {
            u32 pieces[4], np, at = 0;
            u32 g[2] = {Gain(o, c, 0), Gain(o, c, 1)};
            Take(o->ring[c], CB_BYTES, pieces, &np);
            for (u32 p = 0; p < np; p += 2)
                for (u32 k = 0; k < pieces[p + 1]; k += 2, at++) {
                    s32 x = (s16)IopMem_Read16(pieces[p] + k);
                    acc[0][at] += (x * (s32)g[0]) >> 8;
                    acc[1][at] += (x * (s32)g[1]) >> 8;
                }
        }
    }
    u8 *stage = IopMem_Ptr(AT(A_STAGE) + half * (STAGE_SIZE / 2));
    for (u32 n = 0; n < 512; n++) {
        u32 blk = n / 256, pos = n % 256;
        s16 l = (s16)Sat(sPrev[0][n]), rr = (s16)Sat(sPrev[1][n]);
        snd_memcpy(stage + blk * 0x400 + pos * 2, &l, 2);
        snd_memcpy(stage + blk * 0x400 + 0x200 + pos * 2, &rr, 2);
    }
    snd_memcpy(sPrev, acc, sizeof(acc));
}

static u32 Callback(u32 ch, u32 arg, u32 a2, u32 half)
{
    (void)arg;
    (void)a2;
    if (ch != 1 || half > 1)
        Unobserved("a transfer callback for another channel or half");
    Fill(half);
    if (SndHost_LogLevel() >= SND_LOG_DEBUG) {   /* the staging buffer's FNV-1a 64, for comparisons */
        const u8 *b = IopMem_Ptr(AT(A_STAGE));
        u64 h = 0xCBF29CE484222325ull;
        for (u32 i = 0; i < STAGE_SIZE; i++)
            h = (h ^ b[i]) * 0x100000001B3ull;
        SND_LOGD("adx", "B %u %08X%08X", half, (u32)(h >> 32), (u32)h);
        if (SndHost_LogLevel() >= SND_LOG_TRACE) {
            for (u32 i = 0; i < 8; i++) {
                u32 w0, w1;
                snd_memcpy(&w0, b + 0x200 * i, 4);
                snd_memcpy(&w1, b + 0x200 * i + 4, 4);
                SND_LOGT("adx", "  chunk %u: %08X %08X", i, w0, w1);
            }
        }
    }
    if (sPsm)
        IopKern_Call(NULL, "iWakeupThread", (u32)sPsm, 0, 0, 0);
    return 1;
}

/* ---------------------------------------------------------------- the RPC server */

static u32 NewThread(u32 entry, u32 prio)
{
    u32 p = AT(A_TPARAM);
    W32(p, 0x02000000);
    W32(p + 4, 0);
    W32(p + 8, entry);
    W32(p + 12, 0x800);
    W32(p + 16, prio);
    return IopNative_Call("CreateThread", p, 0, 0, 0);
}

static u32 RpcFn(u32 fno, u32 buff, u32 size, u32 unused)
{
    (void)size;
    (void)unused;
    switch (fno) {
    case 2: {
        u32 kind = R32(buff);
        if (kind > 1 || sNStat > 1)
            Unobserved("a status block of another kind");
        sStat[kind].ee = R32(buff + 4);
        sStat[kind].iop = R32(buff + 8);
        if (R32(buff + 12) != BLK_SIZE)
            Unobserved("a status block of another size");
        W32(buff, AT(A_STAT) + STAT_SIZE * (u32)sNStat);
        sNStat++;
        break;
    }
    case 0x403:
    case 0x40A:
        break;
    case 0x40C: {
        if (R32(buff) != 48000)
            Unobserved("an output rate other than 48000");
        sPsm = (int)NewThread(AT(A_PSM), 39);
        IopNative_Sd(28, 1, 0, 0, 0);
        IopNative_Sd(26, 1, AT(A_CB), AT(A_CBARG), 0);
        IopNative_Sd(18, 1, 0x10, AT(A_STAGE), STAGE_SIZE);
        IopNative_Sd(5, 0xF81, 0x7FFF, 0, 0);
        IopNative_Sd(5, 0x1081, 0x7FFF, 0, 0);
        IopNative_Call("StartThread", (u32)sPsm, 0, 0, 0);
        W32(buff, 1);
        break;
    }
    case 0x422: {
        if (R32(buff) != 1 || R32(buff + 8) != RING_DATA || sNRing == MAX_RING)
            Unobserved("a ring of another kind");
        Ring *r = &sRing[sNRing];
        r->used = 1;
        r->buf = R32(buff + 4);
        r->sj = AT(A_SJ) + SJ_SIZE * (u32)sNRing;
        sNRing++;
        W32(buff, r->sj);
        break;
    }
    case 0x400: {
        Ring *r = &sRing[RingBySj(R32(buff + 4))];
        if (R32(buff + 8) != 1)
            Unobserved("a ring proxy of another kind");
        r->ee_stat = R32(buff + 12);
        r->sjx = AT(A_SJX) + SJX_SIZE * (u32)(r - sRing);
        W32(buff, r->sjx);
        break;
    }
    case 0x408: {
        Out *o;
        u32 nch = R32(buff);
        if (nch < 1 || nch > 2 || sNOut == MAX_OUT || R32(buff + 4) != 0)
            Unobserved("an output of another kind");
        o = &sOut[sNOut];
        snd_memset(o, 0, sizeof(*o));
        o->nch = (int)nch;
        for (u32 c = 0; c < nch; c++)
            o->ring[c] = RingBySj(R32(buff + 8 + 4 * c));
        W32(buff, AT(A_OUT) + OUT_SIZE * (u32)sNOut);
        sNOut++;
        break;
    }
    default:
        Unobserved("an RPC function number");
    }
    return buff;
}

/* ---------------------------------------------------------------- the threads and the start */

static u32 PsmThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    for (;;) {
        IopNative_Call("SleepThread", 0, 0, 0, 0);
        IopNative_Sd(20, 1, 0, 0, 0);
    }
    return 0;
}

static u32 DtxThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    IopNative_Call("sceSifRpcLoop", AT(A_QD), 0, 0, 0);
    return 0;
}

static u32 MainThread(u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 dtx;
    (void)a0; (void)a1; (void)a2; (void)a3;
    dtx = NewThread(AT(A_DTX), 48);
    IopNative_Call("StartThread", dtx, 0, 0, 0);
    IopNative_Call("sceSifInitRpc", 0, 0, 0, 0);
    IopNative_Call("sceSifSetRpcQueue", AT(A_QD), dtx, 0, 0);
    /* sceSifRegisterRpc(sd, sid, func, buff, cfunc, cbuff, qd): the last three on the stack */
    {
        u32 args[7] = {AT(A_SD), 0x90000200u, AT(A_RPC), AT(A_RPCBUF), 0, 0, AT(A_QD)};
        IopNative_CallN("sceSifRegisterRpc", args, 7);
    }
    for (;;) {
        IopNative_Call("WaitVblankEnd", 0, 0, 0, 0);
        Tick();
    }
    return 0;
}

static u32 StartFn(u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 main_th;
    (void)a0; (void)a1; (void)a2; (void)a3;
    IopNative_Sd(5, 0x980, 0x3FFF, 0, 0);
    IopNative_Sd(5, 0xA80, 0x3FFF, 0, 0);
    IopNative_Sd(5, 0x981, 0x3FFF, 0, 0);
    IopNative_Sd(5, 0xA81, 0x3FFF, 0, 0);
    main_th = NewThread(AT(A_MAIN), 39);
    IopNative_Call("StartThread", main_th, 0, 0, 0);
    return 2;   /* the module stays resident */
}

static u32 Setup(u32 base, u32 size)
{
    (void)size;
    sBase = base;
    sNStat = sNRing = sNOut = sNCons = sPsm = 0;
    snd_memset(sPrev, 0, sizeof(sPrev));
    IopNative_Map(AT(A_START), StartFn);
    IopNative_Map(AT(A_MAIN), MainThread);
    IopNative_Map(AT(A_DTX), DtxThread);
    IopNative_Map(AT(A_RPC), RpcFn);
    IopNative_Map(AT(A_PSM), PsmThread);
    IopNative_Map(AT(A_CB), Callback);
    return A_START - OBS_BASE;
}

const IopNativeModule g_adx_iop = {"CRI_ADX_Driver", Setup};
