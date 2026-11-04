/* The native Capcom Sound Engine, the voice engine: CSE.IRX's ThEffect.c functions written from the module's code
 * (CSE.IRX is Capcom's code, its disassembly is read in scratch and
 * never committed). The functions keep the module's order, named by their module offset, and the data keeps the module's
 * layout (offsets in the comments are module-relative, as the module's own addresses). Kernel and libsd calls are made in
 * the module's order through cse_iop.c; the IOP's time model charges nothing for the code in between (Details 7).
 *
 *  0x22E8 voice pool       0x2A6C voice object     0x2FC4 the voices' tick     0x3138 volume and pitch of a voice
 *  0x34F0 silence a voice  0x36D4 match            0x387C key-off, stop, set   0x3CEC set priority and ids
 *  0x3E20 allocate a voice 0x4364 count            0x442C .. 0x4658 channel volumes, LFOs, the sine table
 *  0x49AC key-on           0x4E98 modulate         0x5070 limit by id          0x5300 effect request
 *  0x53F4 .. 0x553C status 0x5B30 .. 0x6938 the SPU shadow and its synchronisation with libsd
 * Paths no recorded stream reaches say so in their comments. */
#include "cse/cse_eng.h"

#define R32 IopMem_Read32
#define W32 IopMem_Write32

/* ------------------------------------------------------------------------------------------ the SPU shadow (0xA75C) */

/* One SPU voice's registers as the module keeps them (0x2C bytes), with the bits of its dirty word (0xC): 0x1 VOLL, 0x2 VOLR,
 * 0x4 PITCH, 0x8 ADSR1, 0x10 ADSR2, 0x100 SSA, 0x200 LSAX. */
typedef struct SpuVoice {
    u16 core, voice;                       /* 0, 2 */
    u32 e;                                 /* 4: voice * 2 | core, libsd's register index */
    u32 mask;                              /* 8: 1 << voice */
    u32 dirty;                             /* 0xC */
    u16 volL, volR, pitch, adsr1, adsr2;   /* 0x10 .. 0x18 */
    u16 envx, r1c, r1e;                    /* 0x1A: the envelope read at each poll */
    u32 ssa, lsax, r28;                    /* 0x20, 0x24, 0x28 */
} SpuVoice;

/* One core (0x8B4 bytes, at 0xA760 and 0xB014): its dirty word (8) and what each bit writes (F5DB4): bit 0 MMIX (0x800) from 0x84C,
 * 1 MVOLL (0x980), 2 MVOLR (0xA80), 3 EVOLL (0xB80), 4 EVOLR (0xC80), 5 AVOLL (0xD80), 6 AVOLR (0xE80), 7 BVOLL (0xF80), 8 BVOLR (0x1080);
 * 11 VMIXL (switch 0x1300) from 0x864, 12 VMIXEL (0x1400) 0x868, 13 KON (0x1500) 0x86C, 14 KOFF (0x1600) 0x870, 15 (0x1700) 0x874,
 * 16 (0x1800) 0x878, 17 (0x1A00) 0x87C, 18 (0x1900) 0x880, 19 (0x1B00) 0x884; 21 EEA (addr 0x1D00) 0x88C, 23 (addr 0x1F00) 0x894;
 * 24 .. 27 the core attributes 2, 4, 6, 8 from 0x898 .. 0x89E; 28 reads ESA and EEA back into 0x888 and 0x88C. */
typedef struct SpuCore {
    u32 core, core1, dirty;                /* 0, 4, 8 */
    SpuVoice voice[24];                    /* 0xC */
    u8 unused[0x420];                      /* 0x42C: nothing in the module touches it */
    u16 mmix, mvoll, mvolr, evoll, evolr, avoll, avolr, bvoll, bvolr;   /* 0x84C .. 0x85C */
    u16 r85e, r860;                        /* 0x85E, 0x860 */
    u32 vmixl, vmixel, kon, koff, r874, sw1800, sw1a00, sw1900, sw1b00;   /* 0x864 .. 0x884 */
    u32 esa, eea, r890, r894;              /* 0x888 .. 0x894 */
    u16 attr2, attr4, attr6, attr8;        /* 0x898 .. 0x89E */
    u32 fxcore, fxmode;                    /* 0x8A0, 0x8A4 */
    u16 fxdl, fxdr;                        /* 0x8A8, 0x8AA */
    u32 fxdelay, fxfb;                     /* 0x8AC, 0x8B0 */
} SpuCore;

typedef struct SpuState {                  /* 0xA75C */
    u32 dirty;                             /* 0: bit 0 = SetCoreAttr(0xA) from coreattr */
    SpuCore core[2];                       /* 4, 0x8B8 */
    u16 coreattr, pad;                     /* 0x116C */
} SpuState;

static SpuState sSpu;

/* 0x64B0 */
static void SpuVoiceInit(SpuVoice *v, u32 core, u32 voice)
{
    v->core = (u16)core;
    v->voice = (u16)voice;
    v->e = (u32)v->voice << 1 | v->core;
    v->mask = 1u << v->voice;
    v->dirty = 0x7FF;
    v->volL = 0; v->volR = 0; v->pitch = 0x2000; v->adsr1 = 0x80FF; v->adsr2 = 0x5FC0;
    v->envx = 0; v->r1c = 0; v->r1e = 0;
    v->ssa = 0x5010; v->lsax = 0; v->r28 = 0;
}

/* 0x5C64 */
static void SpuCoreInit(SpuCore *c, u32 core)
{
    u32 i;
    c->core = core;
    c->core1 = core & 1;
    c->dirty = 0x1FFFFFFF;
    c->mmix = c->core == 0 ? 0xFC0 : 0xFCC;
    c->mvoll = 0x3FFF; c->mvolr = 0x3FFF; c->evoll = 0; c->evolr = 0; c->avoll = 0x7FFF; c->avolr = 0x7FFF;
    c->bvoll = 0; c->bvolr = 0; c->r85e = 0; c->r860 = 0;
    c->vmixl = 0; c->vmixel = 0; c->kon = 0xFFFFFF; c->koff = 0; c->r874 = 0;
    c->sw1800 = 0xFFFFFF; c->sw1900 = 0xFFFFFF; c->sw1a00 = 0xFFFFFF; c->sw1b00 = 0xFFFFFF;
    c->esa = 0x1FFFF0; c->eea = 0x1FFFFF; c->r890 = 0; c->r894 = 0;
    c->attr2 = 0; c->attr4 = 1; c->attr6 = 1; c->attr8 = 0;
    c->fxcore = c->core; c->fxmode = 0; c->fxdl = c->evoll; c->fxdr = c->evolr; c->fxdelay = 0; c->fxfb = 0;
    for (i = 0; i < 24; i++)
        SpuVoiceInit(&c->voice[i], c->core1, i);
}

/* 0x671C, 0x643C: the envelope of every voice and the mixer, read */
static void SpuVoicePoll(SpuVoice *v)
{
    v->envx = (u16)CseSd(SD_GETPARAM, (v->e | 0x500) & 0xFFFF, 0, 0, 0);
}

static void SpuCorePoll(SpuCore *c)
{
    u32 i;
    for (i = 0; i < 24; i++)
        SpuVoicePoll(&c->voice[i]);
    c->mmix = (u16)CseSd(SD_GETPARAM, (c->core1 | 0x800) & 0xFFFF, 0, 0, 0);
}

/* 0x5C24 */
static void SpuPoll(SpuState *s)
{
    s->coreattr = (u16)CseSd(SD_GETCOREATTR, 0xA, 0, 0, 0);
    SpuCorePoll(&s->core[0]);
    SpuCorePoll(&s->core[1]);
}

/* 0x6524: a voice's changed registers, lowest bit first */
static void SpuVoiceSync(SpuVoice *v)
{
    u32 bit = 1;
    while (v->dirty) {
        switch (v->dirty & bit) {
        case 0x1: CseSdW(SD_SETPARAM, v->e & 0xFFFF, v->volL, 0, 0); break;
        case 0x2: CseSdW(SD_SETPARAM, (v->e | 0x100) & 0xFFFF, v->volR, 0, 0); break;
        case 0x4: CseSdW(SD_SETPARAM, (v->e | 0x200) & 0xFFFF, v->pitch, 0, 0); break;
        case 0x8: CseSdW(SD_SETPARAM, (v->e | 0x300) & 0xFFFF, v->adsr1, 0, 0); break;
        case 0x10: CseSdW(SD_SETPARAM, (v->e | 0x400) & 0xFFFF, v->adsr2, 0, 0); break;
        case 0x100: CseSdW(SD_SETADDR, (v->e | 0x2040) & 0xFFFF, v->ssa, 0, 0); break;
        case 0x200: CseSdW(SD_SETADDR, (v->e | 0x2140) & 0xFFFF, v->lsax, 0, 0); break;
        default: break;
        }
        v->dirty &= ~bit;
        bit <<= 1;
    }
}

/* 0x5DB4: a core's voices, then its changed registers lowest bit first, then the key-on and key-off words cleared */
static void SpuCoreSync(SpuCore *c)
{
    u32 bit = 1, i, k = c->core1;
    for (i = 0; i < 24; i++)
        SpuVoiceSync(&c->voice[i]);
    while (c->dirty) {
        switch (c->dirty & bit) {
        case 0x1: CseSdW(SD_SETPARAM, (k | 0x800) & 0xFFFF, c->mmix, 0, 0); break;
        case 0x2: CseSdW(SD_SETPARAM, (k | 0x980) & 0xFFFF, c->mvoll, 0, 0); break;
        case 0x4: CseSdW(SD_SETPARAM, (k | 0xA80) & 0xFFFF, c->mvolr, 0, 0); break;
        case 0x8: CseSdW(SD_SETPARAM, (k | 0xB80) & 0xFFFF, c->evoll, 0, 0); break;
        case 0x10: CseSdW(SD_SETPARAM, (k | 0xC80) & 0xFFFF, c->evolr, 0, 0); break;
        case 0x20: CseSdW(SD_SETPARAM, (k | 0xD80) & 0xFFFF, c->avoll, 0, 0); break;
        case 0x40: CseSdW(SD_SETPARAM, (k | 0xE80) & 0xFFFF, c->avolr, 0, 0); break;
        case 0x80: CseSdW(SD_SETPARAM, (k | 0xF80) & 0xFFFF, c->bvoll, 0, 0); break;
        case 0x100: CseSdW(SD_SETPARAM, (k | 0x1080) & 0xFFFF, c->bvolr, 0, 0); break;
        case 0x800: CseSdW(SD_SETSWITCH, (k | 0x1300) & 0xFFFF, c->vmixl, 0, 0); break;
        case 0x1000: CseSdW(SD_SETSWITCH, (k | 0x1400) & 0xFFFF, c->vmixel, 0, 0); break;
        case 0x2000: CseSdW(SD_SETSWITCH, (k | 0x1500) & 0xFFFF, c->kon, 0, 0); CseK("DelayThread", 4, 0, 0, 0); break;
        case 0x4000: CseSdW(SD_SETSWITCH, (k | 0x1600) & 0xFFFF, c->koff, 0, 0); CseK("DelayThread", 4, 0, 0, 0); break;
        case 0x8000: CseSdW(SD_SETSWITCH, (k | 0x1700) & 0xFFFF, c->r874, 0, 0); break;
        case 0x10000: CseSdW(SD_SETSWITCH, (k | 0x1800) & 0xFFFF, c->sw1800, 0, 0); break;
        case 0x20000: CseSdW(SD_SETSWITCH, (k | 0x1A00) & 0xFFFF, c->sw1a00, 0, 0); break;
        case 0x40000: CseSdW(SD_SETSWITCH, (k | 0x1900) & 0xFFFF, c->sw1900, 0, 0); break;
        case 0x80000: CseSdW(SD_SETSWITCH, (k | 0x1B00) & 0xFFFF, c->sw1b00, 0, 0); break;
        case 0x200000: CseSdW(SD_SETADDR, (k | 0x1D00) & 0xFFFF, c->eea, 0, 0); break;
        case 0x800000: CseSdW(SD_SETADDR, (k | 0x1F00) & 0xFFFF, c->r894, 0, 0); break;
        case 0x1000000: CseSdW(SD_SETCOREATTR, (k | 2) & 0xFFFF, c->attr2, 0, 0); break;
        case 0x2000000: CseSdW(SD_SETCOREATTR, (k | 4) & 0xFFFF, c->attr4, 0, 0); break;
        case 0x4000000: CseSdW(SD_SETCOREATTR, (k | 6) & 0xFFFF, c->attr6, 0, 0); break;
        case 0x8000000: CseSdW(SD_SETCOREATTR, (k | 8) & 0xFFFF, c->attr8, 0, 0); break;
        case 0x10000000:   /* not bracketed by CpuSuspendIntr */
            c->esa = CseSd(SD_GETADDR, (k | 0x1C00) & 0xFFFF, 0, 0, 0);
            c->eea = CseSd(SD_GETADDR, (k | 0x1D00) & 0xFFFF, 0, 0, 0);
            break;
        default: break;
        }
        c->dirty &= ~bit;
        bit <<= 1;
    }
    c->kon = 0;
    c->koff = 0;
}

/* 0x5B80 */
static void SpuSync(SpuState *s)
{
    if (s->dirty & 1)
        CseSdW(SD_SETCOREATTR, 0xA, s->coreattr, 0, 0);
    s->dirty = 0;
    SpuCoreSync(&s->core[0]);
    SpuCoreSync(&s->core[1]);
}

/* 0x5B30 */
static void SpuInit(SpuState *s)
{
    s->coreattr = 0;
    s->dirty = 1;
    SpuCoreInit(&s->core[0], 0);
    SpuCoreInit(&s->core[1], 1);
    SpuSync(s);
}

/* 0x68F8, 0x6938: a core's shadow and a voice's */
static SpuCore *SpuCoreOf(u32 core) { return core < 2 ? &sSpu.core[core] : NULL; }
static SpuVoice *SpuVoiceOf(u32 core, u32 voice) { return core < 2 && voice < 24 ? &sSpu.core[core].voice[voice] : NULL; }

void CseEng_SpuInit(void) { SpuInit(&sSpu); }

void CseEng_Poll(void)
{
    CseK("GetSystemTimeLow", 0, 0, 0, 0);
    SpuPoll(&sSpu);
    CseK("GetSystemTimeLow", 0, 0, 0, 0);
}

void CseEng_Drive(void)
{
    CseK("GetSystemTimeLow", 0, 0, 0, 0);
    SpuSync(&sSpu);
    CseK("GetSystemTimeLow", 0, 0, 0, 0);
}

/* ---------------------------------------------------------------------------------------------- the voice engine */

typedef struct Lfo { u32 phase; u16 rate, depth; } Lfo;   /* 8 bytes: 0x45D8 sets, 0x45E8 advances, 0x4600 reads */

/* A voice of the pool (0x44 bytes, 48 of them from 0x9950). state: 0 free, 1 keyed on, 2 playing, 3 key-off asked, 4/5 key-off due,
 * 6 key-off sent, 7 to retire, 8 retired. */
typedef struct Voice {
    u32 state;                  /* 0 */
    SpuVoice *hw;               /* 4 */
    u32 id;                     /* 8: the request's word 1 */
    u8 flags, flagsOld, phase, prio;   /* 0xC: CseParam.unk4 (bits 0, 1 cores allowed; 2, 3 effect send of core 0, 1; 6, 7 phase), the previous, +1 or -1, priority */
    u8 bank, key, id2, id3;     /* 0x10: bank | 0x80 when the flags ask for it, key, ids */
    u8 vpan, rpan;              /* 0x14: the CseVoice pan, the CseParam pan */
    u16 vvol, rvol;             /* 0x16: both volumes as 0..0x3FFF */
    s16 vpitch, bend, bendNow;  /* 0x1A: CseVoice pitch (cents), CseParam bend, the bend's pitch offset */
    u16 bendUp, bendDown;       /* 0x20, 0x22 */
    u8 vol1, vol2;              /* 0x24, 0x25: 7-bit */
    u8 pad[2];
    u32 rate;                   /* 0x28: the sample rate */
    u32 age;                    /* 0x2C: ticks since key-on */
    u32 life;                   /* 0x30: ticks to live (0: unlimited) */
    Lfo pitchLfo, volLfo;       /* 0x34, 0x3C */
} Voice;

typedef struct Node { s16 home, idx; Voice *voice; struct Node *prev, *next; } Node;   /* 16 bytes, from 0x9650; the lists' sentinels have the same shape */
typedef struct List { u32 core; u32 count; Node *head, *tail; } List;                    /* 0x95C0 active (2), 0x95D0 core 1, 0x95E0 core 0 */

typedef struct Chan { s16 vol, tgt; u16 frac; s16 fade; s32 step; s16 end; s16 pad; } Chan;   /* 16 bytes: 0xA624 + 16 n, the master at 0xA724 */

static Node *sFreeCur;          /* 0x95B8 */
static Node *sActCur;           /* 0x95BC */
static List sActive;            /* 0x95C0 */
static List sFree1;             /* 0x95D0 */
static List sFree0;             /* 0x95E0 */
static Node sSent[6];           /* 0x95F0: active head, tail; core 1 head, tail; core 0 head, tail */
static Node sNode[48];          /* 0x9650 */
static Voice sVoice[48];        /* 0x9950 */
static u32 sClockA610, sClockA614, sClockA618, sClockA61C;   /* 0xA610 .. 0xA61C: the engine's time statistics */
static u32 sMono;               /* 0xA620 */
static Chan sChan[17];          /* 0xA624: 16 channels, then the master */
#define MASTER (&sChan[16])

/* ---- the sine table (0x7340): 257 entries, sin(2 pi i / 1024) * 32767 ---- */
static const u16 kSin[257] = {
    0x0000, 0x00C9, 0x0192, 0x025B, 0x0324, 0x03ED, 0x04B6, 0x057E,
    0x0647, 0x0710, 0x07D9, 0x08A1, 0x096A, 0x0A32, 0x0AFB, 0x0BC3,
    0x0C8B, 0x0D53, 0x0E1B, 0x0EE3, 0x0FAB, 0x1072, 0x1139, 0x1200,
    0x12C7, 0x138E, 0x1455, 0x151B, 0x15E1, 0x16A7, 0x176D, 0x1833,
    0x18F8, 0x19BD, 0x1A82, 0x1B46, 0x1C0B, 0x1CCF, 0x1D93, 0x1E56,
    0x1F19, 0x1FDC, 0x209F, 0x2161, 0x2223, 0x22E4, 0x23A6, 0x2467,
    0x2527, 0x25E7, 0x26A7, 0x2767, 0x2826, 0x28E5, 0x29A3, 0x2A61,
    0x2B1E, 0x2BDB, 0x2C98, 0x2D54, 0x2E10, 0x2ECC, 0x2F86, 0x3041,
    0x30FB, 0x31B4, 0x326D, 0x3326, 0x33DE, 0x3496, 0x354D, 0x3603,
    0x36B9, 0x376F, 0x3824, 0x38D8, 0x398C, 0x3A3F, 0x3AF2, 0x3BA4,
    0x3C56, 0x3D07, 0x3DB7, 0x3E67, 0x3F16, 0x3FC5, 0x4073, 0x4120,
    0x41CD, 0x4279, 0x4325, 0x43D0, 0x447A, 0x4523, 0x45CC, 0x4674,
    0x471C, 0x47C3, 0x4869, 0x490E, 0x49B3, 0x4A57, 0x4AFA, 0x4B9D,
    0x4C3F, 0x4CE0, 0x4D80, 0x4E20, 0x4EBF, 0x4F5D, 0x4FFA, 0x5097,
    0x5133, 0x51CE, 0x5268, 0x5301, 0x539A, 0x5432, 0x54C9, 0x555F,
    0x55F4, 0x5689, 0x571D, 0x57B0, 0x5842, 0x58D3, 0x5963, 0x59F3,
    0x5A81, 0x5B0F, 0x5B9C, 0x5C28, 0x5CB3, 0x5D3D, 0x5DC6, 0x5E4F,
    0x5ED6, 0x5F5D, 0x5FE2, 0x6067, 0x60EB, 0x616E, 0x61F0, 0x6271,
    0x62F1, 0x6370, 0x63EE, 0x646B, 0x64E7, 0x6562, 0x65DD, 0x6656,
    0x66CE, 0x6745, 0x67BC, 0x6831, 0x68A5, 0x6919, 0x698B, 0x69FC,
    0x6A6C, 0x6ADB, 0x6B4A, 0x6BB7, 0x6C23, 0x6C8E, 0x6CF8, 0x6D61,
    0x6DC9, 0x6E30, 0x6E95, 0x6EFA, 0x6F5E, 0x6FC0, 0x7022, 0x7082,
    0x70E1, 0x7140, 0x719D, 0x71F9, 0x7254, 0x72AE, 0x7306, 0x735E,
    0x73B5, 0x740A, 0x745E, 0x74B1, 0x7503, 0x7554, 0x75A4, 0x75F3,
    0x7640, 0x768D, 0x76D8, 0x7722, 0x776B, 0x77B3, 0x77F9, 0x783F,
    0x7883, 0x78C6, 0x7908, 0x7949, 0x7989, 0x79C7, 0x7A04, 0x7A41,
    0x7A7C, 0x7AB5, 0x7AEE, 0x7B25, 0x7B5C, 0x7B91, 0x7BC4, 0x7BF7,
    0x7C29, 0x7C59, 0x7C88, 0x7CB6, 0x7CE2, 0x7D0E, 0x7D38, 0x7D61,
    0x7D89, 0x7DB0, 0x7DD5, 0x7DF9, 0x7E1C, 0x7E3E, 0x7E5E, 0x7E7E,
    0x7E9C, 0x7EB9, 0x7ED4, 0x7EEF, 0x7F08, 0x7F20, 0x7F37, 0x7F4C,
    0x7F61, 0x7F74, 0x7F86, 0x7F96, 0x7FA6, 0x7FB4, 0x7FC1, 0x7FCD,
    0x7FD7, 0x7FE0, 0x7FE8, 0x7FEF, 0x7FF5, 0x7FF9, 0x7FFC, 0x7FFE,
    0x7FFF,
};

/* 0x4658: the sine of a phase of 1024 units a turn; negative phases index the module's table before its start, which no caller
 * produces (the callers' arguments are an unsigned shift and 256 + 2 pan) */
static s16 Sine(s32 x)
{
    s32 i = x & 0x3FF;
    if (x < 0 && i != 0)
        SndLog_Fatal("cse", "a negative phase of the sine function (0x4658)");
    if (i < 0x100)
        return (s16)kSin[i];
    if (i < 0x200)
        return (s16)kSin[0x200 - i];
    if (i < 0x300)
        return (s16)-(s16)kSin[i - 0x200];
    return (s16)-(s16)kSin[0x400 - i];
}

/* 0x4768 */
static s16 Cosine(s32 x) { return Sine(x + 0x100); }

/* 0x45D8, 0x45E8, 0x4600 */
static void LfoSet(Lfo *l, u32 rate, u32 depth) { l->phase = 0; l->rate = (u16)rate; l->depth = (u16)depth; }
static void LfoStep(Lfo *l) { l->phase += l->rate; }
static s32 LfoGet(const Lfo *l) { return ((s32)l->depth * Sine((s32)(l->phase >> 8))) / 0x8000; }

/* 0x447C, 0x442C: the channels and the master, reset */
static void ChanInit(Chan *c)
{
    c->vol = 0x3FFF; c->tgt = 0x3FFF; c->fade = 0x3FFF; c->frac = 0; c->step = 0; c->end = 0;
}

static void ChansInit(void)
{
    u32 i;
    ChanInit(MASTER);
    for (i = 0; i < 16; i++)
        ChanInit(&sChan[i]);
}

/* 0x4528: a channel's fade by one tick, then its volume */
static void ChanTick(Chan *c)
{
    if (c->step != 0) {
        u32 acc = (u32)c->frac | (u32)(u16)c->fade << 16;
        acc += (u32)c->step;
        c->frac = (u16)acc;
        c->fade = (s16)(acc >> 16);
        if (c->step < 0 ? !(c->end < c->fade) : !(c->fade < c->end)) {
            c->fade = c->end;
            c->frac = 0;
            c->step = 0;
        }
    }
    c->vol = (s16)((s32)c->tgt * c->fade / 0x3FFF);
}

/* 0x449C: the master, then every channel times the master */
static void ChansTick(void)
{
    u32 i;
    ChanTick(MASTER);
    for (i = 0; i < 16; i++) {
        ChanTick(&sChan[i]);
        sChan[i].vol = (s16)((s32)MASTER->vol * sChan[i].vol / 0x3FFF);
    }
}

/* ---- the voice pool and its lists ---- */

/* 0x2A6C */
static Voice *VoiceObj(u32 n) { return &sVoice[n % 48]; }

/* 0x299C: the voice objects, zeroed, each with its SPU voice */
static void VoicesInit(void)
{
    u32 core, v, n = 0;
    for (core = 0; core < 2; core++)
        for (v = 0; v < 24; v++, n++) {
            Voice *o = VoiceObj(n);
            snd_memset(o, 0, sizeof(*o));
            o->state = 0;
            o->hw = SpuVoiceOf(core, v);
        }
}

/* 0x22E8: the free lists of both cores (24 nodes each between two sentinels) and the empty active list */
static void PoolInit(void)
{
    Node *h0 = &sSent[4], *t0 = &sSent[5], *h1 = &sSent[2], *t1 = &sSent[3], *ah = &sSent[0], *at = &sSent[1];
    u32 i;
    for (i = 0; i < 24; i++) {
        Node *n = &sNode[i];
        n->home = 0; n->idx = (s16)i; n->prev = i == 0 ? h0 : &sNode[i - 1]; n->next = i == 23 ? t0 : &sNode[i + 1];
        n->voice = VoiceObj(i);
    }
    for (i = 24; i < 48; i++) {
        Node *n = &sNode[i];
        n->home = 1; n->idx = (s16)(i - 24); n->prev = i == 24 ? h1 : &sNode[i - 1]; n->next = i == 47 ? t1 : &sNode[i + 1];
        n->voice = VoiceObj(i);
    }
    h0->prev = NULL; h0->voice = NULL; t0->next = NULL; t0->voice = NULL;
    h1->prev = NULL; h1->voice = NULL; t1->next = NULL; t1->voice = NULL;
    ah->prev = NULL; ah->voice = NULL; at->next = NULL; at->voice = NULL;
    h0->home = h0->idx = t0->home = t0->idx = -1;
    h1->home = h1->idx = t1->home = t1->idx = -1;
    ah->home = ah->idx = at->home = at->idx = -1;
    h0->next = &sNode[0]; t0->prev = &sNode[23];
    h1->next = &sNode[24]; t1->prev = &sNode[47];
    ah->next = at; at->prev = ah;
    sActive.core = 2; sActive.count = 0; sActive.head = ah; sActive.tail = at;
    sFree1.core = 1; sFree1.count = 24; sFree1.head = h1; sFree1.tail = t1;
    sFree0.core = 0; sFree0.count = 24; sFree0.head = h0; sFree0.tail = t0;
}

static List *FreeList(u32 core) { return (core & 1) ? &sFree1 : &sFree0; }

/* 0x25E4, 0x2614, 0x2648: the free voices of a core, the busy ones, the active ones */
static u32 FreeCount(u32 core) { return FreeList(core)->count; }
static u32 BusyCount(u32 core) { return 24 - FreeList(core)->count; }
static u32 ActiveCount(void) { return sActive.count; }

/* 0x2658, 0x26B4: a core's free voices in turn */
static Voice *FreeFirst(u32 core)
{
    List *l = FreeList(core);
    Node *n;
    if (l->count == 0)
        return NULL;
    n = l->head->next;
    sFreeCur = n;
    return n->voice;
}

static Voice *FreeNext(void)
{
    Node *n = sFreeCur, *m;
    if (!n)
        return NULL;
    m = n->next;
    sFreeCur = m;
    if (m->next == NULL) {
        sFreeCur = NULL;
        return NULL;
    }
    return m->voice;
}

/* 0x270C, 0x2750: the active voices in turn (the cursor is the module's, the caller may release the voice it has) */
static Voice *ActFirst(void)
{
    Node *n;
    if (sActive.count == 0)
        return NULL;
    n = sActive.head->next;
    sActCur = n;
    return n->voice;
}

static Voice *ActNext(void)
{
    Node *n, *m;
    if (sActive.count == 0)
        return NULL;
    n = sActCur;
    if (!n)
        return NULL;
    m = n->next;
    sActCur = m;
    if (m->next == NULL) {
        sActCur = NULL;
        return NULL;
    }
    return m->voice;
}

/* 0x27C4: the front free voice of a core goes to the end of the active list */
static Voice *VoiceTake(u32 core)
{
    List *l = FreeList(core);
    Node *n, *t;
    if (l->count == 0)
        return NULL;
    n = l->head->next;
    n->next->prev = n->prev;
    n->prev->next = n->next;
    l->count--;
    t = sActive.tail;
    n->prev = t->prev;
    n->next = t;
    n->prev->next = n;
    t->prev = n;
    sActive.count++;
    return n->voice;
}

/* 0x2898: an active voice goes to the end of its core's free list */
static int VoiceRelease(Voice *v)
{
    Node *n;
    if (sActive.count == 0)
        return -1;
    for (n = sActive.head->next; n->next; n = n->next) {
        if (n->voice == v) {
            List *l = n->home == 0 ? &sFree0 : &sFree1;
            Node *t = l->tail;
            n->prev->next = n->next;
            n->next->prev = n->prev;
            sActive.count--;
            n->prev = t->prev;
            n->next = t;
            n->prev->next = n;
            t->prev = n;
            l->count++;
            return 0;
        }
    }
    return -1;
}

/* ---- a voice's volumes and pitch ---- */

/* The IOP's address 0. A steal that finds no victim goes on with the NULL it got (0x3F14, 0x3F90: the module tests neither the victim
 * nor the voice it hands to 0x34F0), so the module reads and writes the IOP's low memory as a voice object, as that voice's SPU
 * shadow (a NULL pointer in it is address 0 again) and, once its state word is 7, as a core (0x68F8 answers NULL for core 7).
 * The original's low memory is zero there: the first such steal leaves core 0's KON dirty with an empty mask (the extra
 * SetSwitch(0x1500, 0) of the probe grids), the later ones touch nothing the engine reads. The block is that memory's first 0x8B4 bytes. */
static union LowMem { Voice v; SpuVoice h; SpuCore c; } sLow;

/* 0x34F0: silence a voice and key it on to the silent loop at 0x5010 */
static void VoiceKill(Voice *v)
{
    SpuVoice *hw = v->hw ? v->hw : &sLow.h;
    SpuCore *c = SpuCoreOf(hw->core);
    if (!c)
        c = &sLow.c;
    hw->volL = 0;
    hw->volR = 0;
    hw->adsr1 = 0x80FF;
    hw->adsr2 = 0x5FC0;
    hw->ssa = 0x5010;
    hw->dirty |= 0x11B;
    c->kon |= hw->mask;
    c->koff &= ~hw->mask;
    c->dirty |= 0x2000;
}

/* 0x3138: a voice's left and right volume and pitch from its parameters, the channel's volume, the pan law and the LFOs.
 * The stages divide by 0x3FFF and 127 one after the other, each truncating toward zero (the fits of the earlier sessions
 * missed that the volumes are 0..0x3FFF and the pan law divides by 0x7FFF, not 0x8000). */
static void VoiceUpdate(Voice *v)
{
    s32 x, pan, pan2, l, r, cents, note, fine, pitch;
    s32 chan = sChan[v->bank & 0xF].vol;
    x = chan * v->vvol / 0x3FFF;
    x = x * v->rvol / 0x3FFF;
    x = x * v->vol1 / 0x7F;
    x = x * v->vol2 / 0x7F;
    x += LfoGet(&v->volLfo);
    if (x < 0) x = 0;
    if (x >= 0x4000) x = 0x3FFF;
    pan = (s32)v->vpan + ((s32)v->rpan - 0x40);
    if (pan < 0) pan = 0;
    if (pan >= 0x80) pan = 0x7F;
    pan2 = pan << 1;
    l = x * Cosine(pan2) / 0x7FFF;
    r = x * Sine(pan2) / 0x7FFF;
    cents = (s32)v->vpitch + v->bend + v->bendNow;
    cents += LfoGet(&v->pitchLfo);
    if (cents >= 0x1771) cents = 0x1770;
    else if (cents < -0x1770) cents = -0x1770;
    note = cents / 100 + 0x3C;
    fine = cents % 100;
    pitch = (s32)(u16)CseSd(SD_NOTE2PITCH, 0x3C, 0, (u32)note & 0xFFFF, (u32)(s32)(s16)fine);
    pitch = (s32)v->rate * pitch / 48000;
    if (sMono == 1) {
        s32 s = l + r;
        r = s < 0 ? (s + 1) >> 1 : s >> 1;
        l = r;
    }
    if (pitch >= 0x4000) pitch = 0x3FFF;
    v->hw->pitch = (u16)pitch;
    v->hw->dirty |= 4;
    /* the phase of the channels (flags bits 6, 7): the sign of the multiplier changes when they change */
    if ((v->flagsOld & 0xC0) == 0) {
        if ((v->flags & 0xC0) == 0x80) v->phase = (u8)-(s8)v->phase;
    } else if ((v->flagsOld & 0xC0) == 0x80) {
        if ((v->flags & 0xC0) == 0) v->phase = (u8)-(s8)v->phase;
    }
    v->flagsOld = v->flags;
    l *= (s8)v->phase;
    r *= (s8)v->phase;
    if (v->flags & 0x80)
        l = -l;
    v->hw->volL = l < 0 ? (u16)((l + 0x3FFF) | 0x4000) : (u16)l;
    v->hw->volR = r < 0 ? (u16)((r + 0x3FFF) | 0x4000) : (u16)r;
    v->hw->dirty |= 3;
}

/* 0x35C4: retired voices (state 7) go back to their free lists */
static void VoicesReap(void)
{
    Voice *v = ActFirst(), *w;
    while (v) {
        if (v->state == 7) {
            v->state = 8;
            w = ActNext();
            VoiceRelease(v);
            v->state = 0;
            v = w;
        } else {
            v = ActNext();
        }
    }
}

/* 0x2FC4: every active voice, once a tick */
static void VoicesTick(void)
{
    Voice *v = ActFirst();
    while (v) {
        SpuCore *c = SpuCoreOf(v->hw->core);
        v->age++;
        LfoStep(&v->pitchLfo);
        LfoStep(&v->volLfo);
        if (v->state == 1)
            v->state = 2;
        if (v->state != 0 && v->state != 7)
            VoiceUpdate(v);
        if (v->life != 0 && v->life < v->age)
            v->state = 4;
        if (v->state == 3)
            v->state = 4;
        if (v->state == 4) {
            v->state = 5;
            c->koff |= v->hw->mask;
            c->dirty |= 0x4000;
            v->state = 6;
        }
        if (v->hw->envx == 0) {
            VoiceKill(v);
            v->state = 7;
        }
        v = ActNext();
    }
    VoicesReap();
}

/* ---- the requests ---- */

/* The CseParam of a request, at the request + 8 (include/cse.h): flags at 0, then bytes 4 unk4, 5 priority, 6 bank, 7 key, 8 id2, 9 id3,
 * s16 0xA volume, 0xC pan, 0xE bend, 0x10 bend ratio, u8 0x13, 0x14 the 7-bit volumes, u32 0x1C the id to match. */
#define PU32(p, o) (*(const u32 *)((const u8 *)(p) + (o)))
#define PS16(p, o) (*(const s16 *)((const u8 *)(p) + (o)))
#define PU16(p, o) (*(const u16 *)((const u8 *)(p) + (o)))
#define PU8(p, o) (*((const u8 *)(p) + (o)))

/* 0x3640: the CseParam's flags turned into the match mask of 0x36D4 */
static u32 ParamMask(const u8 *p)
{
    u32 in = PU32(p, 0), out = 0;
    if (in & 1) out |= 1;
    if (in & 2) out |= 2;
    else if (in & 8) out |= 4;
    if (in & 0x10) out |= 8;
    if (in & 0x20) out |= 0x10;
    if (in & 0x40) out |= 0x20;
    if (in & 0x80) out |= 0x40;
    if (in & 4) out |= 0x80;
    return out;
}

/* 0x36D4: does a voice match the param under the mask, lowest mask bit first (bit 4 falls into bit 0x80's test) */
static int VoiceMatch(const Voice *v, const u8 *p, u32 m)
{
    u32 bit = 1;
    while (m) {
        switch (m & bit) {
        case 0x40: if (v->id != PU32(p, 0x1C)) return 0; break;
        case 0x20: if (v->id3 != PU8(p, 9)) return 0; break;
        case 0x10: if (v->id2 != PU8(p, 8)) return 0; break;
        case 0x08: if (v->key != PU8(p, 7)) return 0; break;
        case 0x04: if (v->bank & 0x80) return 0;
            /* fall through */
        case 0x80: if ((v->bank & 0xF) != (PU8(p, 6) & 0xF)) return 0; break;
        case 0x02: if (!(v->bank & 0x80)) return 0; break;
        default: break;
        }
        m &= ~bit;
        bit <<= 1;
    }
    return 1;
}

/* 0x4364: the voices that match (mode 1: only the ones keyed on or playing) */
static u32 VoiceCount(const u8 *p, u32 mode)
{
    u32 mask = ParamMask(p), n = 0;
    Voice *v;
    for (v = ActFirst(); v; v = ActNext())
        if (VoiceMatch(v, p, mask) == 1 && (mode == 0 || (mode == 1 && (v->state == 1 || v->state == 2))))
            n++;
    return n;
}

static s32 Vol7(s32 v) { return v > 0 ? ((v << 14) - v) / 0x7F : 0; }   /* 0..127 as 0..0x3FFF */

/* 0x387C: the commands that act on the voices that match: 0x10000001 key-off, 2 stop, 3 set-param. Returns 0 if a match had a
 * higher priority than the request's. */
static u32 VoicesAct(u8 *p, u32 mask, u32 cmd)
{
    u32 r = 1;
    Voice *v;
    for (v = ActFirst(); v; v = ActNext()) {
        u32 fl;
        if (v->state == 0 || !VoiceMatch(v, p, mask))
            continue;
        if (PU8(p, 5) < v->prio) {
            r = 0;
            continue;
        }
        if (cmd == 0x10000002) {
            VoiceKill(v);
            v->state = 7;
            continue;
        }
        if (cmd == 0x10000001) {
            if (v->state == 1) {
                VoiceKill(v);
                v->state = 7;
            } else {
                v->state = 3;
            }
            continue;
        }
        if (cmd != 0x10000003)
            continue;
        /* set-param (no recorded stream sends it): each field the flags name */
        fl = PU32(p, 0);
        if (fl & 0x100) {
            s32 vol = PS16(p, 0xA);
            if (vol >= 0 && vol < 0x80)
                v->rvol = (u16)Vol7(vol);
        } else if (fl & 0x200) {
            s32 d = Vol7(PS16(p, 0xA)), t = (s32)v->rvol + d;
            v->rvol = t >= 0x4000 ? 0x3FFF : t < 0 ? 0 : (u16)t;
        } else if (fl & 0x400) {
            s32 t = (s32)v->rvol * PS16(p, 0xA) / 100;
            v->rvol = t >= 0x4000 ? 0x3FFF : t < 0 ? 0 : (u16)t;
        }
        if (fl & 0x800) {
            s32 pan = PS16(p, 0xC);
            if (pan >= 0 && pan < 0x80) {
                v->rpan = (u8)pan;
                v->flags = (u8)((v->flags | PU8(p, 4)) & 0x3F);
            }
        } else if (fl & 0x1000) {
            s32 t = (s32)v->rpan + PS16(p, 0xC);
            v->rpan = t >= 0x80 ? 0x7F : t < 0 ? 0 : (u8)t;
            v->flags = (u8)((v->flags | PU8(p, 4)) & 0x3F);
        }
        if (fl & 0x2000) v->bend = PS16(p, 0xE);
        else if (fl & 0x4000) v->bend = (s16)(v->bend + PS16(p, 0xE));
        if (fl & 0x8000) {
            s32 b = PS16(p, 0x10);
            if (b >= 0x2000) b = 0x1FFF;
            else if (b < -0x2000) b = -0x2000;
            if (b >= 0) v->bendNow = (s16)((s32)v->bendUp * b / 0x1FFF);
            else v->bendNow = (s16)(((s32)v->bendDown * b) / 0x2000);
        }
        if (fl & 0x10000) v->vol1 = PU8(p, 0x13) & 0x7F;
        if (fl & 0x20000) v->vol2 = PU8(p, 0x14) & 0x7F;
    }
    return r;
}

/* 0x3CEC: priority and ids of the matching voices (request 0x10000006; no recorded stream sends it) */
static u32 VoicesSetIds(u8 *p, u32 mask, const u8 *q)
{
    Voice *v;
    for (v = ActFirst(); v; v = ActNext()) {
        u16 fl;
        if (v->state == 0 || !VoiceMatch(v, p, mask))
            continue;
        fl = PU16(q, 0);
        if (fl & 0x100) v->prio = PU8(q, 2);
        else if (fl & 0x200) {
            s32 t = (s32)v->prio + PU8(q, 2);
            v->prio = t >= 0x100 ? 0xFF : (u8)t;
        }
        if (PU16(q, 0) & 0x400) v->id2 = PU8(q, 3);
        if (PU16(q, 0) & 0x800) v->id3 = PU8(q, 4);
    }
    return 0;
}

/* 0x400C: the voice to take from: kind 1 the lowest priority of the playing voices (state 1, 2), 2 of the releasing ones (3 to 6) on an
 * allowed core, 3 of any state on an allowed core; ties go to the oldest. */
static Voice *VoiceVictim(const u8 *p, u32 kind)
{
    u32 mask = ParamMask(p);
    Voice *best = NULL, *v;
    for (v = ActFirst(); v; v = ActNext()) {
        if (kind == 1) {
            if (v->state != 1 && v->state != 2) continue;
            if (!VoiceMatch(v, p, mask)) continue;
        } else if (kind == 2) {
            if (v->state < 3 || v->state > 6) continue;
            if (!((1u << v->hw->core) & PU8(p, 4) & 3)) continue;
            if (!VoiceMatch(v, p, mask)) continue;
        } else if (kind == 3) {
            if (!((1u << v->hw->core) & PU8(p, 4) & 3)) continue;
        } else {
            return NULL;
        }
        if (!best || v->prio < best->prio || (v->prio == best->prio && best->age < v->age))
            best = v;
    }
    return best;
}

/* 0x3E94: a free voice on an allowed core (both: the one with the fewer busy voices, core 0 on a tie) */
static Voice *VoiceFree(const u8 *p)
{
    u32 b = PU8(p, 4), core;
    if ((b & 3) == 3)
        core = BusyCount(1) < BusyCount(0) ? 1 : 0;
    else
        core = (b & 1) ? 0 : 1;
    return VoiceTake(core);
}

/* 0x3F14, 0x3F90: take a voice from the releasing ones (kind 2) or any (kind 3) of equal or lower priority, then a free one */
static Voice *VoiceSteal(const u8 *p, u32 kind)
{
    Voice *v = VoiceVictim(p, kind);
    if (!v)
        v = &sLow.v;   /* no victim: the module goes on with NULL (see sLow) */
    if (PU8(p, 5) < v->prio)
        return NULL;
    VoiceKill(v);
    v->state = 7;
    VoicesReap();
    return VoiceFree(p);
}

/* 0x3E20: a voice for a key-on: how 0 a free one, 2 and 3 stolen */
static Voice *VoiceAlloc(const u8 *p, u32 how)
{
    switch (how) {
    case 0: return VoiceFree(p);
    case 2: return VoiceSteal(p, 2);
    case 3: return VoiceSteal(p, 3);
    default: return NULL;
    }
}

/* 0x5070: may the key-on go ahead: the key-offs of the request's own flags and its limit on voices with the same ids */
static u32 KeyOnAllowed(u8 *p)
{
    u32 r, count = VoiceCount(p, 1), limit = PU8(p, 0x12);
    if (limit == 0) {
        if (PU32(p, 0) & 1)
            r = VoicesAct(p, ParamMask(p), 0x10000001);
        else
            r = 1;
    } else if (count < limit) {
        r = 1;
    } else {
        u32 i, n = count + 1 - limit;
        r = 0;
        for (i = 0; i < n; i++) {
            Voice *v = VoiceVictim(p, 1);
            if (!v)
                continue;
            if ((PU32(p, 0) & 1) && PU8(p, 5) < v->prio) {
                r = 0;
                break;
            }
            v->state = 3;
            r = 1;
        }
    }
    VoicesReap();
    return r;
}

/* 0x49AC: key-on. The request is the 64 bytes at w (word 0 the command, 1 the id, 2 to 9 the CseParam, 10 to 14 the CseVoice). */
static void KeyOn(u32 *w)
{
    u8 *p = (u8 *)&w[2], *rq = (u8 *)w;
    Voice *v;
    SpuCore *c;
    s32 b;
    if (!KeyOnAllowed(p))
        return;
    v = VoiceAlloc(p, 0);
    if (!v) v = VoiceAlloc(p, 2);
    if (!v) v = VoiceAlloc(p, 3);
    if (!v)
        return;
    c = SpuCoreOf(v->hw->core);
    v->life = PU32(p, 0x18);
    v->age = 0;
    v->id = w[1];
    v->flags = PU8(p, 4);
    v->flagsOld = PU8(p, 4);
    v->phase = 1;
    v->prio = PU8(p, 5);
    v->bank = (u8)(PU8(p, 6) | ((PU32(p, 0) & 2) ? 0x80 : 0));
    v->key = PU8(p, 7);
    v->id2 = (PU32(p, 0) & 0x20) ? PU8(p, 8) : 0xFF;
    v->id3 = (PU32(p, 0) & 0x40) ? PU8(p, 9) : 0xFF;
    v->rate = w[13];
    v->vpan = PU8(rq, 0x29);
    v->rpan = (u8)PS16(p, 0xC);
    v->vvol = (u16)Vol7(PU8(rq, 0x28));
    v->rvol = (u16)Vol7(PS16(p, 0xA));
    v->vpitch = *(const s16 *)(rq + 0x2A);
    v->bend = PS16(p, 0xE);
    v->bendDown = *(const u16 *)(rq + 0x2C);
    v->bendUp = *(const u16 *)(rq + 0x2E);
    v->bendNow = 0;
    b = PS16(p, 0x10);
    if (b >= 0x2000) { b = 0x1FFF; }
    else if (b < -0x2000) { b = -0x2000; }
    /* the clamp is written back to the request (0x4BF0, 0x4C04); only the request's own copy sees it */
    *(s16 *)(p + 0x10) = (s16)b;
    if (b >= 0) v->bendNow = (s16)((s32)v->bendUp * b / 0x1FFF);
    else v->bendNow = (s16)(((s32)v->bendDown * b) / 0x2000);
    v->vol1 = PU8(p, 0x13) & 0x7F;
    v->vol2 = PU8(p, 0x14) & 0x7F;
    LfoSet(&v->pitchLfo, 0x6000, 0);
    LfoSet(&v->volLfo, 0x418, 0);
    v->hw->adsr1 = *(const u16 *)(rq + 0x30);
    v->hw->adsr2 = *(const u16 *)(rq + 0x32);
    v->hw->ssa = w[14];
    v->hw->dirty |= 0x118;
    VoiceUpdate(v);
    /* the effect sends: the voice's mask is set in the core's words 0x880 and 0x884 when its flags (bit 2 core 0, bit 3 core 1) ask */
    if (v->hw->core == 0) {
        if (v->flags & 4) { c->sw1900 |= v->hw->mask; c->sw1b00 |= v->hw->mask; }
        else { c->sw1900 &= ~v->hw->mask; c->sw1b00 &= ~v->hw->mask; }
    } else if (v->hw->core == 1) {
        if (v->flags & 8) { c->sw1900 |= v->hw->mask; c->sw1b00 |= v->hw->mask; }
        else { c->sw1900 &= ~v->hw->mask; c->sw1b00 &= ~v->hw->mask; }
    }
    c->dirty |= 0xC0000;
    c->kon |= v->hw->mask;
    c->koff &= ~v->hw->mask;
    c->dirty |= 0x2000;
    v->state = 1;
}

/* 0x5194, 0x51F8, 0x525C: key-off, stop and set-param of the matching voices. The request's priority is raised to 0xFF (it wins every
 * comparison) unless its flags bit 0 asks for the priority test. */
static u32 CmdVoices(u32 *w, u32 cmd)
{
    u8 *p = (u8 *)&w[2];
    u32 mask = ParamMask(p), r;
    if (!(PU32(p, 0) & 1)) {
        p[5] = 0xFF;
        mask |= 1;
    }
    r = VoicesAct(p, mask, cmd);
    if (cmd != 0x10000003)   /* set-param (0x525C) does not retire voices */
        VoicesReap();
    return r;
}

/* 0x4E98: modulate (the LFO of the matching voices; no recorded stream sends it) */
static void CmdModulate(u32 *w)
{
    u8 *p = (u8 *)&w[2], *rq = (u8 *)w;
    u32 mask = ParamMask(p);
    Voice *v;
    if (!(PU32(p, 0) & 1)) {
        p[5] = 0xFF;
        mask |= 1;
    }
    for (v = ActFirst(); v; v = ActNext()) {
        if (v->state == 0 || v->state == 7 || !VoiceMatch(v, p, mask))
            continue;
        if (PU8(p, 5) < v->prio)
            continue;
        v->pitchLfo.phase = 0;
        v->pitchLfo.rate = *(const u16 *)(rq + 0x28);
        v->pitchLfo.depth = *(const u16 *)(rq + 0x2A);
        v->volLfo.phase = 0;
        v->volLfo.rate = *(const u16 *)(rq + 0x2C);
        v->volLfo.depth = *(const u16 *)(rq + 0x2E);
    }
}

/* 0x52B0 (request 0x10000006, no recorded stream sends it) */
static void CmdSetIds(u32 *w)
{
    u8 *p = (u8 *)&w[2];
    u32 mask = ParamMask(p);
    if (!(PU32(p, 0) & 1)) {
        p[5] = 0xFF;
        mask |= 1;
    }
    VoicesSetIds(p, mask, (const u8 *)&w[10]);
}

/* 0x5300: an effect request (core in the CseParam's first halfword): the core's shadow and a message to the effect thread */
static void CmdEffect(u32 *w)
{
    const u8 *p = (const u8 *)&w[2];
    u32 core = PU16(p, 0) & 1;
    SpuCore *c = SpuCoreOf(core);
    c->fxcore = core;
    c->fxmode = PU16(p, 2);
    c->fxdl = (u16)PS16(p, 4);
    c->fxdr = (u16)PS16(p, 4);
    c->fxdelay = PU8(p, 6);
    c->fxfb = PU8(p, 7);
    c->dirty |= 0x10000000;
    c->eea = PU32(p, 8);
    c->dirty |= 0x200000;
    CseEffMsg(core, PU16(p, 2), PS16(p, 4), PU8(p, 6), PU8(p, 7), PU32(p, 8));
}

/* 0x4794, 0x4800, 0x4910: the channel volumes (channel 0xFF is the master) */
static Chan *ChanOf(u32 ch)
{
    if (ch == 0xFF) return MASTER;
    if (ch > 16)
        SndLog_Fatal("cse", "a channel number past the table (0x4794)");
    return &sChan[ch];
}

/* 0x4FB8: the engine's reset: the voices, the pool, the request ring (cse_iop.c), the SPU shadow, the channels, and an effect request for each core.
 * The effect requests' end address is the init function's stack word that nothing wrote (effEnd). */
void CseEng_Init(u32 effEnd)
{
    u32 core;
    VoicesInit();
    PoolInit();
    SpuInit(&sSpu);
    ChansInit();
    sMono = 0;
    for (core = 0; core < 2; core++) {
        u32 w[16] = {0};
        w[2] = core;                       /* the CseParam's first halfword: the core; mode, depth, delay and feedback 0 */
        w[4] = effEnd;
        w[3] = 0;
        CmdEffect(w);
    }
}

/* 0x2D6C: one tick (the poller calls it every 4 ms with its round counter) */
u32 CseEng_Callback(u32 n)
{
    u32 i, t0;
    t0 = CseK("GetSystemTimeLow", 0, 0, 0, 0);
    sClockA61C = t0;
    if (n % 250 == 0) {
        sClockA614 = 0;
        sClockA610 = 0;
    }
    ChansTick();
    VoicesTick();
    for (i = 0; i < 64; i++) {
        u32 w[16];
        if (CseDeq(w) == -1)
            break;
        switch (w[0] - 0x10000000u) {
        case 0: KeyOn(w); break;
        case 1: CmdVoices(w, 0x10000001); break;
        case 2: CmdVoices(w, 0x10000002); break;
        case 3: CmdVoices(w, 0x10000003); break;
        case 4: CmdModulate(w); break;
        case 6: CmdSetIds(w); break;
        case 7: CmdEffect(w); break;
        case 8: {   /* mixer: the core's MMIX */
            SpuCore *c = SpuCoreOf(*(u16 *)&w[2]);
            if (c) { c->mmix = *((u16 *)&w[2] + 1); c->dirty |= 1; }
            break;
        }
        case 9: sMono = ((u8 *)w)[8] & 1; break;
        case 10: {
            Chan *c = ChanOf(((u8 *)w)[8]);
            c->tgt = (s16)Vol7(((u8 *)w)[9]);
            break;
        }
        case 11: {
            Chan *c = ChanOf(((u8 *)w)[8]);
            s32 target = Vol7(((u8 *)w)[0x10]);
            c->step = target < c->fade ? -(s32)w[3] : (s32)w[3];
            c->end = (s16)target;
            break;
        }
        case 12: {
            Chan *c = ChanOf(((u8 *)w)[8]);
            c->fade = 0x3FFF; c->frac = 0; c->step = 0; c->end = 0;
            break;
        }
        case 13: CseEng_Init(0x2000000); break;
        default: SndLog_Fatal("cse", "an unknown request in the queue (0x2D6C)"); break;
        }
    }
    sClockA618 = CseK("GetSystemTimeLow", 0, 0, 0, 0);
    sClockA610 += sClockA618 - sClockA61C;
    if (n % 250 == 249)
        CseSysClock(sClockA610 + sClockA614);   /* the module passes the address of its statistics, SysClock2USec only converts */
    return sClockA618 - sClockA61C;
}

/* 0x2BEC: the STAT server's requests other than 0x20000000 (the uploads, cse_iop.c): 0x20000001 the voices' masks and counts,
 * 0x20000002 the count of voices that match, 0x20000004 an SPU voice's shadow. No recorded stream sends them. */
u32 CseEng_Stat(u32 req, u32 reply)
{
    u32 cmd = R32(req), r = 0;
    if (cmd == 0x20000001) {
        u32 free0 = 0, free1 = 0, act0 = 0, act1 = 0;
        Voice *v;
        for (v = FreeFirst(0); v; v = FreeNext()) free0 |= v->hw->mask;
        for (v = FreeFirst(1); v; v = FreeNext()) free1 |= v->hw->mask;
        for (v = ActFirst(); v; v = ActNext()) {
            if (v->hw->core == 0) act0 |= v->hw->mask;
            else act1 |= v->hw->mask;
        }
        W32(reply + 8, free0);
        W32(reply + 0xC, act0);
        W32(reply + 0x10, free1);
        W32(reply + 0x14, act1);
        IopMem_Write8(reply + 0x18, (u8)FreeCount(0));
        IopMem_Write8(reply + 0x1B, (u8)BusyCount(0));
        IopMem_Write8(reply + 0x19, (u8)FreeCount(1));
        IopMem_Write8(reply + 0x1A, (u8)BusyCount(1));
        IopMem_Write8(reply + 0x1C, (u8)ActiveCount());
    } else if (cmd == 0x20000002) {
        u32 w[16], i;
        for (i = 0; i < 16; i++) w[i] = R32(req + 4 * i);
        IopMem_Write8(reply + 0x1C, (u8)VoiceCount((const u8 *)&w[2], 0));
    } else if (cmd == 0x20000004) {
        SpuVoice *v = SpuVoiceOf(R32(req + 8) & 0xFF, (R32(req + 8) >> 8) & 0xFF);
        if (!v)
            SndLog_Fatal("cse", "STAT 0x20000004 of a voice that does not exist");
        IopMem_Write16(reply + 8, v->volL); IopMem_Write16(reply + 0xA, v->volR); IopMem_Write16(reply + 0xC, v->pitch);
        IopMem_Write16(reply + 0xE, v->adsr1); IopMem_Write16(reply + 0x10, v->adsr2); IopMem_Write16(reply + 0x12, v->envx);
        IopMem_Write16(reply + 0x14, v->r1c); IopMem_Write16(reply + 0x16, v->r1e);
        W32(reply + 0x18, v->ssa); W32(reply + 0x1C, v->r28);
    } else {
        SndLog_Fatal("cse", "a STAT request of an unknown kind (0x2BEC)");
    }
    W32(reply, r);
    return reply;
}
