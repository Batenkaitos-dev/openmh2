/* Sony's libsd (the IOP's sound library), natively.
 *
 * The native Capcom Sound Engine (pc/core/cse/cse_iop.c) and CRI's ADX driver (pc/core/cse/adx_iop.c) call these 20
 * functions (IopNative_Sd). The setting is cse (Libsd_Setting): an audio setting of lle, libsd or adx
 * runs as cse, and the interpreter that ran the modules for --audio lle, libsd and adx is not in the core, so those
 * are refused. A native call is one call point: the time model's delivery at a libsd call's return
 * (iop_kern.c, native_call_point) happens at its return, and nothing happens at positions inside it.
 *
 * Clean room: written from libsd's behaviour at its interface, as observed in the original game
 * game: each call's arguments and result, the
 * SPU2 and DMA registers it read and wrote, in order, and the kernel calls it made; never from libsd's code
 * or its data. ps2sdk's headers (AFL) gave the names and the export numbers only. The values it computes:
 * - sceSdNote2Pitch: two tables of equal temperament's steps (the semitones of an octave, the 128 fine
 *   steps of a semitone) multiplied, then shifted by the octave (at the function): it gives every one of
 *   554,880 results got by calling the original's function over its inputs (how: at the function).
 * - The effect modes: the registers sceSdSetEffectAttr wrote for the two modes the game sets (0 and 4), on
 *   both cores, on the original's boot route (sEffect below), and their areas' sizes from the same writes
 *   (ESA = the end of the area at EEA, less the size): 0x10 and 0x6FE0, mode 4's being the documented size
 *   of the SPU2's "studio C" reverb.
 * An input never observed (a mode, a channel, an attribute, a busy channel) stops the run with its name
 * (Pal_StubAbort), as does each of libsd's exports the game's modules import but never called.
 *
 * Memory: libsd's module image stays loaded at its address, and the native code uses
 * its memory where the real libsd does: the zero block sceSdClearEffectWorkArea's DMAs read, the words its
 * Init clears; the kernel calls' parameter and result words go where the real calls put them, below the
 * caller's stack pointer. The IOP's heap and every allocation are unchanged.
 *
 * MH2_SND_LOG=4 (trace) logs each call, register access and kernel call in the recorder's line format
 * ("snd sdn trace: " then the line). */
#include "snd/snd_iop.h"
#include "pal_core.h"
#include "kernel/kern.h"

/* ------------------------------------------------------------ the trace */

static int sTrace = -1;
static u32 sTid;   /* the trace's context: the calling thread's IopCpu, 1 the interrupt context */

static int Tracing(void)
{
    if (sTrace < 0) {
        sTrace = SndHost_LogLevel() >= SND_LOG_TRACE;
    }
    return sTrace;
}

#define TR(...)                                                                                                \
    do {                                                                                                       \
        if (Tracing())                                                                                         \
            SndHost_Log(SND_LOG_TRACE, "sdn", __VA_ARGS__);                                                    \
    } while (0)

static void Unobserved(const char *what)
{
    SndHost_Log(SND_LOG_ERR, "libsd", "%s: never observed in the original game", what);
    Pal_StubAbort(what);
}

/* ------------------------------------------------------------ the device */

#define SPU 0x1F900000u
#define REG_ATTR 0x19Au        /* + core * 0x400 */
#define REG_TSAH 0x1A8u
#define REG_TSAL 0x1AAu
#define REG_STD 0x1ACu
#define REG_ADMAS 0x1B0u
#define REG_STAT 0x344u
#define REG_ESAH 0x2E0u
#define REG_ESAL 0x2E2u
#define REG_EEA 0x33Cu
#define DMA4 0x1F8010C0u       /* core 0's channel: MADR, BCR (+4, +6), CHCR (+8) */
#define DMA7 0x1F801500u       /* core 1's */
#define SSBUS4 0x1F801014u
#define SSBUS7 0x1F801414u
#define CHCR_START 0x01000201u
#define ATTR_DMA 0x30u         /* the transfer mode's bits: 0x10 the halfword port, 0x20 DMA */

static u16 R16(u32 off)
{
    u16 v = IopMem_Read16(SPU + off);
    TR("R %x %x %x 2", sTid, SPU + off, v);
    return v;
}

static void W16(u32 off, u32 v)
{
    TR("W %x %x %x 2", sTid, SPU + off, v & 0xFFFFu);
    IopMem_Write16(SPU + off, (u16)v);
}

static u32 IoR32(u32 a)
{
    u32 v = IopMem_Read32(a);
    TR("R %x %x %x 4", sTid, a, v);
    return v;
}

static void IoW32(u32 a, u32 v)
{
    TR("W %x %x %x 4", sTid, a, v);
    IopMem_Write32(a, v);
}

static void IoW16(u32 a, u32 v)
{
    TR("W %x %x %x 2", sTid, a, v & 0xFFFFu);
    IopMem_Write16(a, (u16)v);
}

/* A kernel call, from the calling thread (c) or the interrupt context (c NULL). */
static u32 Kc(const IopCpu *c, const char *name, u32 a0, u32 a1, u32 a2, u32 a3)
{
    TR("K %x %s %x %x %x %x", sTid, name, a0, a1, a2, a3);
    u32 v = IopKern_Call(c, name, a0, a1, a2, a3);
    sTid = c ? (u32)(unsigned long)c : 1;   /* another context may have run inside it */
    TR("k %x %s %x", sTid, name, v);
    return v;
}

/* CpuSuspendIntr's word: below the caller's stack pointer, where the real call puts it. */
static u32 Suspend(const IopCpu *c, u32 below)
{
    u32 out = c->r[29] - below;
    Kc(c, "CpuSuspendIntr", out, 0, 0, 0);
    return out;
}

static void Resume(const IopCpu *c, u32 out)
{
    Kc(c, "CpuResumeIntr", IopMem_Read32(out), 0, 0, 0);
}

/* ------------------------------------------------------------ the state */

#define LIBSD_ZERO 0x5090u     /* the zero block in libsd's image (sceSdClearEffectWorkArea's DMA source) */
#define LIBSD_CLEAR 0x55F0u    /* the words sceSdInit clears, and how many bytes */
#define LIBSD_CLEAR_N 0x28u

static u32 sEvf[2];            /* the transfer event flags (sceSdInit), per channel */
static u32 sGp;                /* the gp of sceSdInit's caller: the handlers' and their callbacks' */
static u32 sCbFunc[2], sCbArg[2];
static u32 sTransDone[2];      /* sceSdVoiceTransStatus */
static u32 sTransUser[2];      /* the running transfer is the caller's (its end calls the callback) */
static u32 sBlockBase[2], sBlockHalf[2], sBlockSide[2];
static u32 sEffMode[2];

static void Ret(IopCpu *c, u32 v)
{
    c->r[2] = v;
}

static u32 Arg(const IopCpu *c, int n)
{
    return n < 4 ? c->r[4 + n] : IopMem_Read32(c->r[29] + 0x10u + 4u * (u32)(n - 4));
}

/* The channel's DMA must be idle (every call observed found it so). */
static void CheckIdle(int ch, const char *fn)
{
    u32 attr = R16(REG_ATTR + 0x400u * (u32)ch);
    u32 chcr = IoR32((ch ? DMA7 : DMA4) + 8);
    u32 admas = R16(REG_ADMAS + 0x400u * (u32)ch);
    (void)attr;
    if ((chcr & 0x01000000u) || admas) {
        Unobserved(fn);
    }
}

static void NotInterrupt(const IopCpu *c, const char *fn)
{
    if (Kc(c, "QueryIntrContext", 0, 0, 0, 0)) {
        Unobserved(fn);
    }
}

/* A DMA transfer to SPU RAM on channel 0, as sceSdVoiceTrans's mode 0 and sceSdClearEffectWorkArea's
 * pieces make it; out: the CpuSuspendIntr word's distance below the caller's sp. */
static u32 DmaWrite(const IopCpu *c, u32 iop, u32 spu, u32 size, u32 out, int user)
{
    u32 blocks = (size + 63u) >> 6;
    u32 w, a;
    W16(REG_TSAL, spu >> 1);
    W16(REG_TSAH, spu >> 17);
    R16(REG_TSAH);
    NotInterrupt(c, "sceSdVoiceTrans (in an interrupt)");
    Kc(c, "ClearEventFlag", sEvf[0], ~1u, 0, 0);
    w = Suspend(c, out);
    a = R16(REG_ATTR);
    W16(REG_ATTR, (a & ~ATTR_DMA) | 0x20u);
    Resume(c, w);
    IoW32(SSBUS4, IoR32(SSBUS4));
    sTransDone[0] = 0;
    sTransUser[0] = (u32)user;
    IoW32(DMA4, iop);
    IoW16(DMA4 + 4, 0x10);
    IoW16(DMA4 + 6, blocks);
    IoW32(DMA4 + 8, CHCR_START);
    IoR32(DMA4 + 8);
    return blocks << 6;
}

/* ------------------------------------------------------------ the functions */

/* 4: sceSdInit(flag): both cores reset and set up, the transfer flags made, the handlers registered. */
static void SdInit(IopCpu *c)
{
    u32 sp = c->r[29], w, v, core;
    int i;
    if (Arg(c, 0) != 0) {
        Unobserved("sceSdInit (flag not 0)");
    }
    sGp = c->r[28];
    IoW32(0x1F801404u, 0xBF900000u);
    IoW32(0x1F80140Cu, 0xBF900800u);
    w = Suspend(c, 0x48);
    IoW32(0x1F8010F0u, IoR32(0x1F8010F0u) | 0x80000u);
    IoW32(0x1F801570u, IoR32(0x1F801570u) | 8u);
    Resume(c, w);
    IoW32(SSBUS4, 0x200B31E1u);
    IoW32(SSBUS7, 0x200B31E1u);
    W16(0x7C6, 0x900);
    W16(0x7C8, 0x200);
    W16(0x7CA, 8);
    W16(0x7C0, 0);
    W16(0x7C0, 0x8000);
    W16(0x760, 0);
    W16(0x762, 0);
    W16(0x788, 0);
    W16(0x78A, 0);
    W16(0x1B0, 0);
    W16(0x5B0, 0);
    W16(0x19A, 0);
    W16(0x59A, 0);
    W16(0x19A, 0x8000);
    W16(0x59A, 0x8000);
    W16(0x760, 0);
    W16(0x762, 0);
    W16(0x788, 0);
    W16(0x78A, 0);
    R16(0x344);
    R16(0x744);
    W16(0x1A4, 0xFFFF);
    W16(0x1A6, 0xFF);
    W16(0x5A4, 0xFFFF);
    W16(0x5A6, 0xFF);
    W16(0x180, 0);
    W16(0x182, 0);
    W16(0x580, 0);
    W16(0x582, 0);
    W16(0x184, 0);
    W16(0x186, 0);
    W16(0x584, 0);
    W16(0x586, 0);
    Kc(c, "DisableIntr", 0x24, sp - 0x38, 0, 0);
    Kc(c, "DisableIntr", 0x28, sp - 0x38, 0, 0);
    Kc(c, "DisableIntr", 9, sp - 0x38, 0, 0);
    Kc(c, "ReleaseIntrHandler", 0x24, 0, 0, 0);
    Kc(c, "ReleaseIntrHandler", 0x28, 0, 0, 0);
    Kc(c, "ReleaseIntrHandler", 9, 0, 0, 0);
    Kc(c, "bzero", g_iop_libsd_lo + LIBSD_CLEAR, LIBSD_CLEAR_N, 0, 0);
    W16(0x2E0, 0xE);
    W16(0x2E2, 0xFFF8);
    W16(0x6E0, 0xF);
    W16(0x6E2, 0xFFF8);
    for (i = 0; i < 2; i++) {   /* the event flags: attr 2 (several waiters), pattern 1 */
        IopMem_Write32(sp - 0x48, 2);
        IopMem_Write32(sp - 0x44, 0);
        IopMem_Write32(sp - 0x40, 1);
        IopMem_Write32(sp - 0x3C, 0);
        sEvf[i] = Kc(c, "CreateEventFlag", sp - 0x48, 0, 0, 0);
    }
    /* sixteen bytes to SPU RAM 0x5000 through the halfword port: 0x0700, then zeros */
    W16(REG_ATTR, R16(REG_ATTR) & ~ATTR_DMA);
    W16(REG_TSAH, 0);
    W16(REG_TSAL, 0x2800);
    W16(REG_STD, 0x700);
    for (i = 0; i < 7; i++) {
        W16(REG_STD, 0);
    }
    W16(REG_ATTR, (R16(REG_ATTR) & ~ATTR_DMA) | 0x10u);
    R16(REG_STAT);
    W16(REG_ATTR, R16(REG_ATTR) & ~ATTR_DMA);
    /* each voice: volumes 0, pitch 0x3FFF, envelope 0, start address 0x5000; core 1 written, read back, then
     * core 0 */
    for (i = 0; i < 24; i++) {
        static const u16 sVoice[5] = {0, 0, 0x3FFF, 0, 0};
        u32 r;
        for (r = 0; r < 5; r++) {
            W16(0x400 + 0x10u * (u32)i + 2 * r, sVoice[r]);
            R16(0x400 + 0x10u * (u32)i + 2 * r);
            W16(0x10u * (u32)i + 2 * r, sVoice[r]);
        }
        for (r = 0; r < 2; r++) {
            v = r ? 0x2800u : 0u;
            W16(0x5C0 + 0xCu * (u32)i + 2 * r, v);
            R16(0x5C0 + 0xCu * (u32)i + 2 * r);
            W16(0x1C0 + 0xCu * (u32)i + 2 * r, v);
        }
    }
    for (v = 0; v < 4; v++) {   /* the key-off switches: all voices */
        W16(0x5A0 + 2 * v, (v & 1) ? 0xFF : 0xFFFF);
        R16(0x5A0 + 2 * v);
        W16(0x1A0 + 2 * v, (v & 1) ? 0xFF : 0xFFFF);
    }
    for (core = 0; core < 2; core++) {
        W16(0x342 + 0x400 * core, 0);
        R16(0x342 + 0x400 * core);
        W16(0x340 + 0x400 * core, 0);
    }
    W16(0x7C0, 0xC032);
    W16(0x19A, 0xC000);
    W16(0x59A, 0xC001);
    for (core = 0; core < 2; core++) {   /* the four mix switches: all voices */
        static const u16 sMix[4] = {0x188, 0x190, 0x18C, 0x194};
        for (v = 0; v < 4; v++) {
            W16(sMix[v] + 0x400 * core, 0xFFFF);
            W16(sMix[v] + 2 + 0x400 * core, 0xFF);
        }
    }
    W16(0x198, 0xFF0);
    W16(0x598, 0xFFC);
    W16(0x760, 0);
    W16(0x762, 0);
    W16(0x788, 0);
    W16(0x78A, 0);
    W16(0x764, 0);
    W16(0x766, 0);
    W16(0x78C, 0);
    W16(0x78E, 0);
    W16(0x33C, 0xE);
    W16(0x73C, 0xF);
    W16(0x768, 0);
    W16(0x76A, 0);
    W16(0x790, 0x7FFF);
    W16(0x792, 0x7FFF);
    W16(0x76C, 0);
    W16(0x76E, 0);
    W16(0x794, 0);
    W16(0x796, 0);
    Kc(c, "EnableIntr", 0x24, 0, 0, 0);
    Kc(c, "EnableIntr", 0x28, 0, 0, 0);
    Kc(c, "EnableIntr", 9, 0, 0, 0);
    Kc(c, "RegisterIntrHandler", 0x24, 1, IOP_NATIVE_INTR + 0x24, 0);
    Kc(c, "RegisterIntrHandler", 0x28, 1, IOP_NATIVE_INTR + 0x28, 1);
    Kc(c, "RegisterIntrHandler", 9, 1, IOP_NATIVE_INTR + 9, 0);
    for (i = 0; i < 2; i++) {
        sCbFunc[i] = sCbArg[i] = 0;
        sTransDone[i] = 1;
        sTransUser[i] = 0;
        sEffMode[i] = 0;
    }
    Ret(c, 0);
}

/* The register of a parameter entry: bit 0 the core, bits 1-5 the voice, bits 8 and up the parameter; bit 7
 * a core parameter. NO_REG for an entry never observed. */
#define NO_REG 0xFFFFFFFFu
static u32 ParamReg(u32 e)
{
    u32 core = e & 1, voice = (e >> 1) & 0x1F, p = e >> 8;
    if (!(e & 0x80) && p <= 5) {
        return 0x400 * core + 0x10 * voice + 2 * p;   /* VOLL VOLR PITCH ADSR1 ADSR2 ENVX */
    }
    if (!(e & 0x80) && p == 8 && !voice) {
        return 0x198 + 0x400 * core;
    }
    if ((e & 0x80) && p >= 9 && p <= 0x10 && !voice) {
        return 0x760 + 0x28 * core + 2 * (p - 9);       /* the volumes of the 0x760 block */
    }
    return NO_REG;
}

/* 5: sceSdSetParam(entry, value): the register's KSEG1 address. */
static void SdSetParam(IopCpu *c)
{
    u32 reg = ParamReg(Arg(c, 0));
    if (reg == NO_REG || (Arg(c, 0) >> 8) == 5) {
        Unobserved("sceSdSetParam (this parameter)");
    }
    W16(reg, Arg(c, 1));
    Ret(c, 0xA0000000u | (SPU + reg));
}

/* 6: sceSdGetParam(entry): the register. */
static void SdGetParam(IopCpu *c)
{
    u32 reg = ParamReg(Arg(c, 0));
    if (reg == NO_REG) {
        Unobserved("sceSdGetParam (this parameter)");
    }
    Ret(c, R16(reg));
}

/* 7: sceSdSetSwitch(entry, mask): the 24 voices' bits, low then high half; the high register's address. */
static void SdSetSwitch(IopCpu *c)
{
    static const u16 sSwitch[9] = {0x180, 0x184, 0x1A0, 0x1A4, 0x340, 0x188, 0x18C, 0x190, 0x194};
    u32 e = Arg(c, 0), v = Arg(c, 1), p = e >> 8, reg;
    if (p < 0x13 || p > 0x1B || (e & 0xFE)) {
        Unobserved("sceSdSetSwitch (this switch)");
    }
    reg = sSwitch[p - 0x13] + 0x400 * (e & 1);
    W16(reg, v);
    W16(reg + 2, v >> 16);
    Ret(c, 0xA0000000u | (SPU + reg + 2));
}

/* 9: sceSdSetAddr(entry, address). */
static void SdSetAddr(IopCpu *c)
{
    u32 e = Arg(c, 0), a = Arg(c, 1), core = e & 1, voice = (e >> 1) & 0x1F, p = e >> 8, reg;
    if ((p == 0x20 || p == 0x21) && (e & 0x40) && !(e & 0x80)) {   /* a voice's start, its loop */
        reg = 0x1C0 + 0x400 * core + 0xC * voice + (p == 0x21 ? 4 : 0);
        W16(reg, a >> 17);
        W16(reg + 2, a >> 1);
        Ret(c, (a >> 1) & 0xFFFF);
    } else if (p == 0x1D && !(e & 0xFE)) {   /* the effect area's end */
        W16(REG_EEA + 0x400 * core, a >> 17);
        Ret(c, e & 0xFF00);
    } else if (p == 0x1F && !(e & 0xFE)) {   /* the interrupt address */
        W16(0x19C + 0x400 * core, a >> 17);
        W16(0x19E + 0x400 * core, a >> 1);
        Ret(c, (a >> 1) & 0xFFFF);
    } else {
        Unobserved("sceSdSetAddr (this address)");
    }
}

/* 10: sceSdGetAddr(entry). */
static void SdGetAddr(IopCpu *c)
{
    u32 e = Arg(c, 0), core = e & 1, p = e >> 8, hi, lo;
    if (p == 0x1C && !(e & 0xFE)) {   /* the effect area's start */
        hi = R16(REG_ESAH + 0x400 * core);
        lo = R16(REG_ESAL + 0x400 * core);
        Ret(c, (hi << 17) | (lo << 1));
    } else if (p == 0x1D && !(e & 0xFE)) {   /* its end */
        Ret(c, ((u32)R16(REG_EEA + 0x400 * core) << 17) | 0x1FFFF);
    } else {
        Unobserved("sceSdGetAddr (this address)");
    }
}

/* 11: sceSdSetCoreAttr(entry, value): bit 0 the core, the attribute above it. */
static void SdSetCoreAttr(IopCpu *c)
{
    u32 e = Arg(c, 0), v = Arg(c, 1), core = e & 1, a, w;
    if (e == 0xA) {   /* the S/PDIF output: only 0 observed, which leaves its registers as they were */
        u32 m, s;
        if (v != 0) {
            Unobserved("sceSdSetCoreAttr (S/PDIF mode not 0)");
        }
        m = R16(0x7C0);
        s = R16(0x7C6);
        W16(0x7C8, 0x200);
        W16(0x7C0, m);
        W16(0x7C6, s);
        Ret(c, 0);
        return;
    }
    switch (e >> 1) {
    case 1:   /* the effect: bit 7 */
    case 2:   /* the interrupt: bit 6 */
        if (v > 1) {
            Unobserved("sceSdSetCoreAttr (value not 0 or 1)");
        }
        break;
    case 3:   /* mute: only 1 observed, with bit 14 already set */
        if (v != 1) {
            Unobserved("sceSdSetCoreAttr (mute not 1)");
        }
        break;
    case 4:   /* the noise clock: only 0 observed, with bits 8-13 already clear */
        if (v != 0) {
            Unobserved("sceSdSetCoreAttr (noise clock not 0)");
        }
        break;
    default:
        Unobserved("sceSdSetCoreAttr (this attribute)");
    }
    w = Suspend(c, (e >> 1) == 4 ? 0x18 : 0x40);
    a = R16(REG_ATTR + 0x400 * core);
    switch (e >> 1) {
    case 1:
        a = v ? a | 0x80 : a & ~0x80u;
        break;
    case 2:
        a = v ? a | 0x40 : a & ~0x40u;
        break;
    case 3:
        if (!(a & 0x4000)) {
            Unobserved("sceSdSetCoreAttr (mute with bit 14 clear)");
        }
        break;
    default:
        if (a & 0x3F00) {
            Unobserved("sceSdSetCoreAttr (noise clock with bits 8-13 set)");
        }
        break;
    }
    W16(REG_ATTR + 0x400 * core, a);
    Resume(c, w);
    Ret(c, 0);
}

/* 12: sceSdGetCoreAttr(entry). */
static void SdGetCoreAttr(IopCpu *c)
{
    u32 e = Arg(c, 0);
    if ((e >> 1) == 2) {   /* the interrupt's bit */
        Ret(c, (R16(REG_ATTR + 0x400 * (e & 1)) >> 6) & 1);
    } else if (e == 0xA) {   /* the S/PDIF mode: 0, with ATTR read */
        R16(REG_ATTR);
        Ret(c, 0);
    } else {
        Unobserved("sceSdGetCoreAttr (this attribute)");
    }
}

/* 2^x: the integer part by halving or doubling, the rest by exp's series (x ln 2 below 1). */
static double Exp2(double x)
{
    double r = 1.0, s = 1.0, u = 1.0, y;
    int n;
    while (x >= 1.0) {
        r *= 2.0;
        x -= 1.0;
    }
    while (x < 0.0) {
        r *= 0.5;
        x += 1.0;
    }
    y = x * 0.6931471805599453;
    for (n = 1; n < 30; n++) {
        u *= y / n;
        s += u;
    }
    return r * s;
}

/* 13: sceSdNote2Pitch(centre note, centre fine, note, fine): equal temperament in steps of 1/128 of a
 * semitone, 0x1000 at the centre. The steps from the centre: d = (note - centre) * 128 + fine + centre fine
 * (the centre fine is added, as an unsigned 16-bit value). With d = s * 128 + j (0 <= j < 128) and
 * s = o * 12 + k (0 <= k < 12), the pitch is P = (A[k] * B[j]) >> 16, A[k] = floor(2^15 * 2^(k/12)) and
 * B[j] = floor(2^15 * 2^(j/1536)); P as it is from two octaves above the centre up, else P shifted right
 * by 2 - o, rounded (half up).
 * Made from the original game : a scratch patch of its HLE dispatch called the
 * library's sceSdNote2Pitch on the interpreter over 560,232 inputs (notes and centres 0 to 127, fines -127
 * to 127, eleven centre fines from -200 to 200 as 16-bit values) and this computation gives every one of
 * their results that lies in the domain below; it also gives the game's own 81 distinct calls on area1.
 * Outside it (a note or centre past 127, a fine past one semitone either way: the original's results
 * there follow other rules) the call aborts.
 * The derivation was checked against the original game's results (the tables computed to 60 digits instead of doubles). */
static u16 g_n2p_a[12], g_n2p_b[128];

static void SdNote2Pitch(IopCpu *c)
{
    u32 cn = Arg(c, 0), cf = Arg(c, 1), n = Arg(c, 2), f = Arg(c, 3);
    s32 d, s, j, o, k, sh;
    u32 p;
    if (cn > 127 || n > 127 || (s32)f < -127 || (s32)f > 127 || (cf > 0xFFFF && cf < 0xFFFF8000u)) {
        SND_LOGE("libsd", "sceSdNote2Pitch(0x%X, 0x%X, 0x%X, 0x%X)", cn, cf, n, f);
        Unobserved("sceSdNote2Pitch (a note or centre past 127, or a fine past one semitone)");
    }
    if (!g_n2p_a[0]) {
        for (k = 0; k < 12; k++)
            g_n2p_a[k] = (u16)(32768.0 * Exp2((double)k / 12.0));
        for (j = 0; j < 128; j++)
            g_n2p_b[j] = (u16)(32768.0 * Exp2((double)j / 1536.0));
    }
    d = ((s32)n - (s32)cn) * 128 + (s32)f + (s32)(cf & 0xFFFF);
    s = d >= 0 ? d / 128 : -((127 - d) / 128);
    j = d - s * 128;
    o = s >= 0 ? s / 12 : -((11 - s) / 12);
    k = s - o * 12;
    p = ((u32)g_n2p_a[k] * g_n2p_b[j]) >> 16;
    sh = 2 - o;
    if (sh > 0)
        p = (p + (1u << (sh - 1))) >> sh;
    Ret(c, p);
}

/* 17: sceSdVoiceTrans(channel, mode, iop, spu, size): the bytes it moves (size rounded up to 64). */
static void SdVoiceTrans(IopCpu *c)
{
    u32 ch = Arg(c, 0), mode = Arg(c, 1), iop = Arg(c, 2), spu = Arg(c, 3), size = Arg(c, 4);
    u32 n, i, w, a;
    if (ch != 0 || (mode != 0 && mode != 8)) {
        Unobserved("sceSdVoiceTrans (channel 1, or a mode other than 0 and 8)");
    }
    CheckIdle(0, "sceSdVoiceTrans (channel busy)");
    if (mode == 0) {   /* DMA, ended by interrupt 0x24 */
        Ret(c, DmaWrite(c, iop, spu, size, 0x64, 1));
        return;
    }
    /* mode 8: through the halfword port, at once */
    n = (size + 63u) & ~63u;
    W16(REG_TSAL, spu >> 1);
    W16(REG_TSAH, spu >> 17);
    R16(REG_TSAH);
    NotInterrupt(c, "sceSdVoiceTrans (in an interrupt)");
    Kc(c, "ClearEventFlag", sEvf[0], ~1u, 0, 0);
    for (i = 0; i < n; i += 2) {
        W16(REG_STD, IopMem_Read16(iop + i));
    }
    w = Suspend(c, 0x64);
    a = R16(REG_ATTR);
    W16(REG_ATTR, (a & ~ATTR_DMA) | 0x10u);
    Resume(c, w);
    R16(REG_STAT);
    w = Suspend(c, 0x64);
    a = R16(REG_ATTR);
    W16(REG_ATTR, a & ~ATTR_DMA);
    Resume(c, w);
    sTransDone[0] = 1;
    Ret(c, n);
}

/* 18: sceSdBlockTrans(channel, mode, iop, size): the ring's auto-DMA to core 1's sound input, half by half
 * (interrupt 0x28 starts each next half); half the size. */
static void SdBlockTrans(IopCpu *c)
{
    u32 ch = Arg(c, 0), mode = Arg(c, 1), iop = Arg(c, 2), size = Arg(c, 3), w, a;
    if (ch != 1 || mode != 0x10 || (size & 0x7F)) {
        Unobserved("sceSdBlockTrans (not channel 1's looping input)");
    }
    CheckIdle(1, "sceSdBlockTrans (channel busy)");
    w = Suspend(c, 0x6C);
    a = R16(REG_ATTR + 0x400);
    W16(REG_ATTR + 0x400, a & ~ATTR_DMA);
    Resume(c, w);
    W16(REG_TSAH + 0x400, 0);
    W16(REG_TSAL + 0x400, 0);
    W16(REG_ADMAS + 0x400, 2);
    IoW32(SSBUS7, IoR32(SSBUS7));
    sBlockBase[1] = iop;
    sBlockHalf[1] = size / 2;
    sBlockSide[1] = 0;
    IoW32(DMA7, iop);
    IoW16(DMA7 + 4, 0x10);
    IoW16(DMA7 + 6, sBlockHalf[1] >> 6);
    IoW32(DMA7 + 8, CHCR_START);
    IoR32(DMA7 + 8);
    Ret(c, sBlockHalf[1]);
}

/* 19: sceSdVoiceTransStatus(channel, flag): 1 once the transfer is done. */
static void SdVoiceTransStatus(IopCpu *c)
{
    if (Arg(c, 0) > 1 || Arg(c, 1) != 0) {
        Unobserved("sceSdVoiceTransStatus (waiting)");
    }
    if (!sTransDone[Arg(c, 0)]) {
        Unobserved("sceSdVoiceTransStatus (a transfer still running)");
    }
    Ret(c, 1);
}

/* 20: sceSdBlockTransStatus(channel, flag): the DMA's address, and in bit 24 the half it plays. */
static void SdBlockTransStatus(IopCpu *c)
{
    u32 m;
    if (Arg(c, 0) != 1 || Arg(c, 1) != 0) {
        Unobserved("sceSdBlockTransStatus (channel 0, or flag not 0)");
    }
    IoR32(DMA7);
    m = IoR32(DMA7);
    R16(REG_ADMAS + 0x400);
    Ret(c, (sBlockSide[1] << 24) | m);
}

/* The effect modes the game sets (observed on the original game: the registers sceSdSetEffectAttr wrote). */
typedef struct EffectMode {
    u32 size;        /* the work area's bytes */
    u16 fb[2];       /* the two feedback addresses (0x2E4, 0x2E8; high halves 0) */
    u16 coef[8];     /* 0x774-0x782 (+ core * 0x28) */
    u16 addr[20];    /* 0x2EC-0x338 (high halves 0) */
    u16 in[2];       /* 0x784, 0x786 */
} EffectMode;

static const EffectMode sEffect0 = {0x10, {0, 0}, {0}, {4, 4, 0, 0, 0, 0, 0, 0, 4, 4}, {0, 0}};
static const EffectMode sEffect4 = {
    0x6FE0, {0x38C, 0x2A4}, {0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0},
    {0x37EC, 0x2D60, 0x3424, 0x28F0, 0x2F64, 0x25CC, 0x2D64, 0x2368, 0x2364, 0x17A4,
     0x1FB0, 0x12C0, 0x1BBC, 0x0F48, 0x17A8, 0x0C74, 0x0C70, 0x08E0, 0x0550, 0x02A8},
    {0x8000, 0x8000}};

static const EffectMode *Effect(u32 mode)
{
    return mode == 0 ? &sEffect0 : mode == 4 ? &sEffect4 : NULL;
}

/* The work area's start for a mode: it ends where EEA's block ends. */
static u32 EffectStart(u32 core, const EffectMode *m)
{
    u32 end = ((u32)R16(REG_EEA + 0x400 * core) << 17) | 0x1FFFF;
    return end + 1 - m->size;
}

/* 23: sceSdSetEffectAttr(core, attr): attr is {core, mode, depth L, depth R (s16), delay, feedback}. */
static void SdSetEffectAttr(IopCpu *c)
{
    u32 core = Arg(c, 0), at = Arg(c, 1), mode = IopMem_Read32(at + 4), esa, i, cr = 0x400 * core;
    u32 dl = IopMem_Read16(at + 8), dr = IopMem_Read16(at + 10);
    const EffectMode *m = Effect(mode);
    if (core > 1 || !m || dl || dr || IopMem_Read32(at + 12)) {
        Unobserved("sceSdSetEffectAttr (a mode other than 0 and 4, a depth or a delay)");
    }
    esa = EffectStart(core, m);
    R16(REG_ATTR + cr);
    W16(0x764 + 0x28 * core, dl);
    W16(0x766 + 0x28 * core, dr);
    for (i = 0; i < 2; i++) {
        W16(0x2E4 + cr + 4 * i, 0);
        W16(0x2E6 + cr + 4 * i, m->fb[i]);
    }
    for (i = 0; i < 8; i++) {
        W16(0x774 + 0x28 * core + 2 * i, m->coef[i]);
    }
    for (i = 0; i < 20; i++) {
        W16(0x2EC + cr + 4 * i, 0);
        W16(0x2EE + cr + 4 * i, m->addr[i]);
    }
    W16(0x784 + 0x28 * core, m->in[0]);
    W16(0x786 + 0x28 * core, m->in[1]);
    W16(REG_ESAH + cr, esa >> 17);
    W16(REG_ESAL + cr, esa >> 1);
    R16(REG_ESAL + cr);
    sEffMode[core] = mode;
    Ret(c, 0);
}

/* 24: sceSdGetEffectAttr(core, attr): the mode last set and the effect volumes; the right one. */
static void SdGetEffectAttr(IopCpu *c)
{
    u32 core = Arg(c, 0), at = Arg(c, 1), l, r;
    if (core > 1) {
        Unobserved("sceSdGetEffectAttr (core)");
    }
    l = R16(0x764 + 0x28 * core);
    r = R16(0x766 + 0x28 * core);
    IopMem_Write32(at, core);
    IopMem_Write32(at + 4, sEffMode[core]);
    IopMem_Write16(at + 8, (u16)l);
    IopMem_Write16(at + 10, (u16)r);
    IopMem_Write32(at + 12, 0);
    Ret(c, r);
}

/* 25: sceSdClearEffectWorkArea(core, channel, mode): the mode's work area zeroed by DMA from libsd's zero
 * block, a 64-byte block for the size's odd part first, then 1 KiB at a time, each waited for. */
static void SdClearEffectWorkArea(IopCpu *c)
{
    u32 core = Arg(c, 0), ch = Arg(c, 1), mode = Arg(c, 2), a, left, n;
    const EffectMode *m;
    if (mode == 0 || mode == 0x100) {
        Ret(c, 0);
        return;
    }
    m = mode == 4 ? &sEffect4 : NULL;
    if (core > 1 || ch != 0 || !m) {
        Unobserved("sceSdClearEffectWorkArea (channel 1, or a mode other than 0 and 4)");
    }
    CheckIdle(0, "sceSdClearEffectWorkArea (channel busy)");
    NotInterrupt(c, "sceSdClearEffectWorkArea (in an interrupt)");
    R16(REG_EEA + 0x400 * core);
    a = EffectStart(core, m);
    left = m->size;
    while (left) {
        if (left & 0x3F) {
            n = left & 0x3F;
            CheckIdle(0, "sceSdClearEffectWorkArea (channel busy)");
            DmaWrite(c, g_iop_libsd_lo + LIBSD_ZERO, a, 0x40, 0xA4, 0);
        } else {
            n = left < 0x400 ? left : 0x400;
            CheckIdle(0, "sceSdClearEffectWorkArea (channel busy)");
            DmaWrite(c, g_iop_libsd_lo + LIBSD_ZERO, a, n, 0xA4, 0);
        }
        NotInterrupt(c, "sceSdClearEffectWorkArea (in an interrupt)");
        Kc(c, "WaitEventFlag", sEvf[0], 1, 0, c->r[29] - 0x58);
        a += n;
        left -= n;
    }
    Ret(c, 0);
}

/* 26: sceSdSetTransIntrHandler(channel, func, arg): the handler before. */
static void SdSetTransIntrHandler(IopCpu *c)
{
    u32 ch = Arg(c, 0);
    if (ch > 1) {
        Unobserved("sceSdSetTransIntrHandler (channel)");
    }
    Ret(c, sCbFunc[ch]);
    sCbFunc[ch] = Arg(c, 1);
    sCbArg[ch] = Arg(c, 2);
}

/* 28: sceSdGetTransIntrHandlerArgument(channel). */
static void SdGetTransIntrHandlerArgument(IopCpu *c)
{
    u32 ch = Arg(c, 0);
    if (ch > 1) {
        Unobserved("sceSdGetTransIntrHandlerArgument (channel)");
    }
    Ret(c, sCbArg[ch]);
}

/* ------------------------------------------------------------ the interrupts */

/* Interrupt 0x24, core 0's DMA: the transfer's end; the caller's transfers call the channel's callback. */
static u32 Intr24(void)
{
    u32 a;
    R16(REG_STAT);
    a = R16(REG_ATTR);
    W16(REG_ATTR, a & ~ATTR_DMA);
    R16(REG_ATTR);
    Kc(NULL, "iSetEventFlag", sEvf[0], 1, 0, 0);
    sTransDone[0] = 1;
    if (sTransUser[0] && sCbFunc[0]) {
        TR("B %x %x %x %x 0 0 %x", sTid, sCbFunc[0], 0, sCbArg[0], sGp);
        u32 v = IopNative_CallbackIntr(sCbFunc[0], 0, sCbArg[0], sGp);
        TR("b %x %x %x", sTid, sCbFunc[0], v);
    }
    return 1;
}

/* Interrupt 0x28, core 1's DMA: a half of the ring played; the other half started, then the callback. */
static u32 Intr28(void)
{
    sBlockSide[1] ^= 1;
    IoW16(DMA7 + 6, sBlockHalf[1] >> 6);
    IoW32(DMA7, sBlockBase[1] + sBlockSide[1] * sBlockHalf[1]);
    IoW32(DMA7 + 8, CHCR_START);
    IoR32(DMA7 + 8);
    if (sCbFunc[1]) {
        TR("B %x %x %x %x 0 0 %x", sTid, sCbFunc[1], 1, sCbArg[1], sGp);
        u32 v = IopNative_IsMapped(sCbFunc[1]) ? IopNative_CallIntr(sCbFunc[1], 1, sCbArg[1], 4, sBlockSide[1])
                                            : IopNative_CallbackIntr(sCbFunc[1], 1, sCbArg[1], sGp);
        TR("b %x %x %x", sTid, sCbFunc[1], v);
    }
    return 1;
}

static u32 Libsd_Intr(u32 func, u32 arg)
{
    u32 irq = func - IOP_NATIVE_INTR, v = 0, tid = sTid;
    sTid = 1;
    TR("I %x %x %x %x", sTid, irq, func, arg);
    if (irq == 0x24) {
        v = Intr24();
    } else if (irq == 0x28) {
        v = Intr28();
    } else {
        Unobserved("libsd's SPU2 interrupt handler");
    }
    TR("i %x %x %x", sTid, irq, v);
    sTid = tid;
    return v;
}

/* ------------------------------------------------------------ the calls */

typedef void (*SdFn)(IopCpu *c);
typedef struct SdExport {
    SdFn fn;
    const char *name;
} SdExport;

/* libsd's exports by number (ps2sdk's export table): the 20 the modules call, and the names of the rest. */
static const SdExport sExports[] = {
    {0, "libsd #0"}, {0, "libsd #1"}, {0, "libsd #2"}, {0, "libsd #3"},
    {SdInit, "sceSdInit"},
    {SdSetParam, "sceSdSetParam"},
    {SdGetParam, "sceSdGetParam"},
    {SdSetSwitch, "sceSdSetSwitch"},
    {0, "sceSdGetSwitch"},
    {SdSetAddr, "sceSdSetAddr"},
    {SdGetAddr, "sceSdGetAddr"},
    {SdSetCoreAttr, "sceSdSetCoreAttr"},
    {SdGetCoreAttr, "sceSdGetCoreAttr"},
    {SdNote2Pitch, "sceSdNote2Pitch"},
    {0, "sceSdPitch2Note"},
    {0, "sceSdProcBatch"},
    {0, "sceSdProcBatchEx"},
    {SdVoiceTrans, "sceSdVoiceTrans"},
    {SdBlockTrans, "sceSdBlockTrans"},
    {SdVoiceTransStatus, "sceSdVoiceTransStatus"},
    {SdBlockTransStatus, "sceSdBlockTransStatus"},
    {0, "sceSdSetTransCallback"},
    {0, "sceSdSetIRQCallback"},
    {SdSetEffectAttr, "sceSdSetEffectAttr"},
    {SdGetEffectAttr, "sceSdGetEffectAttr"},
    {SdClearEffectWorkArea, "sceSdClearEffectWorkArea"},
    {SdSetTransIntrHandler, "sceSdSetTransIntrHandler"},
    {0, "sceSdSetSpu2IntrHandler"},
    {SdGetTransIntrHandlerArgument, "sceSdGetTransIntrHandlerArgument"},
    {0, "sceSdGetSpu2IntrHandlerArgument"},
    {0, "sceSdStopTrans"},
    {0, "sceSdCleanEffectWorkArea"},
    {0, "sceSdSetEffectMode"},
    {0, "sceSdSetEffectModeParams"},
};

static void Libsd_Call(IopCpu *c, u32 index)
{
    const SdExport *x;
    if (index >= sizeof(sExports) / sizeof(sExports[0])) {
        Unobserved("libsd: an export past the table");
    }
    x = &sExports[index];
    sTid = (u32)(unsigned long)c;
    if (Tracing()) {   /* the VBlank, as the recorder's V lines */
        static u64 sVb = ~0ull;
        u64 vb = Kern_VblankCount();
        if (vb != sVb) {
            sVb = vb;
            TR("V %x 0", (unsigned)vb);
        }
    }
    if (!x->fn) {
        Unobserved(x->name);
    }
    TR("C %x %x %x %x %x %x %x %x", sTid, index, c->r[4], c->r[5], c->r[6], c->r[7],
       index == 17 ? IopMem_Read32(c->r[29] + 0x10) : 0, c->r[29]);
    x->fn(c);
    sTid = (u32)(unsigned long)c;
    TR("= %x %x %x", sTid, index, c->r[2]);
}

static const IopNativeLib sNative = {Libsd_Call, Libsd_Intr};

/* ------------------------------------------------------------ the setting */

static const char *sAudioOpt;   /* --audio cse */

static int Eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

extern const IopNativeModule g_adx_iop;   /* cse/adx_iop.c */
extern const IopNativeModule g_cse_iop;   /* cse/cse_iop.c */

/* main.c: --audio cse. The modes lle, libsd and adx ran the sound modules on the R3000A interpreter, which only the original game
 * build has (step 6b): they are refused, with the mode that does their work. */
void Libsd_Option(int argc, char **argv, int i)
{
    if (Eq(argv[i], "--audio") && i + 1 < argc) {
        sAudioOpt = argv[i + 1];
        if (Eq(sAudioOpt, "lle") || Eq(sAudioOpt, "libsd") || Eq(sAudioOpt, "adx")) {
            SndLog_Fatal("libsd", "--audio lle, libsd and adx need the IOP interpreter, which is not in mh2-core: the sound is cse (--audio cse)");
        }
        if (!Eq(sAudioOpt, "cse")) {
            SndLog_Fatal("libsd", "--audio takes cse");
        }
    }
}

/* Kern_InitSnd: the setting: --audio when it is given, which the log says; otherwise cse. */
const char *Libsd_Setting(void)
{
    const char *want = sAudioOpt ? sAudioOpt : "cse";
    const char *use = want;
    if (sAudioOpt && !Eq(use, sAudioOpt)) {
        SND_LOGW("libsd", "--audio %s overrides the audio setting %s", sAudioOpt, use);
        use = sAudioOpt;
    } else if (!sAudioOpt && (Eq(use, "lle") || Eq(use, "libsd") || Eq(use, "adx"))) {
        SND_LOGD("libsd", "the audio setting %s runs as cse", use);
        use = "cse";
    }
    if (Eq(use, "cse")) {   /* the native libsd, CRI_ADX_Driver and Capcom Sound Engine (pc/core/cse) */
        g_iop_native_libsd = &sNative;
        g_iop_native_adx = &g_adx_iop;
        g_iop_native_cse = &g_cse_iop;
        SND_LOGW("libsd", "audio cse: libsd, the CRI_ADX_Driver and the Capcom Sound Engine modules are native");
        return "cse";
    }
    return NULL;
}
