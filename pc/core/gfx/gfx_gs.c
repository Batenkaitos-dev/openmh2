/*
 * What the GS receives, and its privileged registers (from the earlier port's hardware model's GS
 * part and gs.c).
 *
 * The core has no GS: the stream goes to the
 * host (pc/core/ipc/core_ipc.h: Ipc_GsGif, Ipc_GsPrivWrite, Ipc_GsVSync;
 * headless, they drop it), or to a test's sink (Gfx_SetGsSink) in its place.
 * The GS's SIGNAL, FINISH and LABEL, which the game
 * waits for, are found in the stream here, as the original game does without a GS
 * backend (gs.c gs_off_events), so the game sees the same events on any host.
 * A GS-to-EE transfer gets the host's answer (Ipc_GsReadback), or zeros
 * (gs.c gs_read_fifo): headless, the zeros are the whole answer. In play
 * the game gets the host's pixels, whether or not anything captures them.
 * The RAM images that hold them (the screen capture's buffer) then agree
 * with what the host drew; without them (headless) the images are zeros.
 *
 * A test can supply the answer instead of the host (Gfx_SetReadback), as it
 * can take the stream (Gfx_SetGsSink). The sink is called for every record,
 * whatever its kind.
 */
#include "gfx_rt.h"
#include "host.h"
#include "pal_freecam.h"
#include "ipc/core_ipc.h"

typedef struct GsPriv {
    u64 pmode, smode1, smode2, srfsh, synch1, synch2, syncv;
    u64 dispfb1, display1, dispfb2, display2, extbuf, extdata, extwrite, bgcolor;
    u64 csr, imr, busdir, siglblid;
} GsPriv;

static GsPriv sGs;
static u32 sField;
static GfxGsSink sSink;
static GfxReadback sReadback;
static u32 sOddities, sOddityKey;
static const char *sOddityTag;


/* An anomaly (a case the game is not known to reach, which the original game logs
 * once): counted, and its first occurrence per tag and key logged as
 * "mh2-core: gfx anomaly: TAG 0xKEY" (the first 64 distinct ones). */
static struct {
    const char *tag;
    u32 key;
} sSeen[64];
static u32 sNSeen;

static int SameTag(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void LogOddity(const char *tag, u32 key)
{
    static const char kPre[] = "mh2-core: gfx anomaly: ";
    char line[96];
    u32 n = 0, i;
    for (i = 0; i < sNSeen; i++)
        if (sSeen[i].key == key && SameTag(sSeen[i].tag, tag))
            return;
    if (sNSeen == sizeof(sSeen) / sizeof(sSeen[0]))
        return;
    sSeen[sNSeen].tag = tag;
    sSeen[sNSeen].key = key;
    sNSeen++;
    for (i = 0; kPre[i]; i++)
        line[n++] = kPre[i];
    for (i = 0; tag[i] && n < 80; i++)
        line[n++] = tag[i];
    line[n++] = ' ';
    line[n++] = '0';
    line[n++] = 'x';
    for (i = 0; i < 8; i++)
        line[n++] = "0123456789ABCDEF"[(key >> (28 - 4 * i)) & 0xF];
    line[n] = 0;
    Host_Log(line);
}

void Gfx_Oddity(const char *tag, u32 key)
{
    if (!sOddities++) {
        sOddityTag = tag;
        sOddityKey = key;
    }
    LogOddity(tag, key);
}

u32 Gfx_Oddities(const char **firstTag, u32 *firstKey)
{
    if (firstTag)
        *firstTag = sOddityTag;
    if (firstKey)
        *firstKey = sOddityKey;
    return sOddities;
}

void Gfx_SetGsSink(GfxGsSink sink) { sSink = sink; }
void Gfx_SetReadback(GfxReadback fn) { sReadback = fn; }

/* A record of the stream: the host takes the GIF and PRIV ones,
 * a test's sink (instead of the host) all of them. */
static void Record(u32 kind, u32 arg, const void *data, u32 bytes)
{
    u64 v;
    if (sSink) {
        sSink(kind, arg, data, bytes);
        return;
    }
    switch (kind) {
    case GFX_GS_GIF:
        Ipc_GsGif(arg, data, bytes / 16);
        break;
    case GFX_GS_PRIV:
        memcpy(&v, data, 8);
        Ipc_GsPrivWrite(arg, v);
        break;
    }
}

/* ---- SIGNAL, FINISH and LABEL (the original's hw_gs_event) ---- */

static void GsEvent(int event, u64 payload)
{
    u32 id = (u32)payload, mask = (u32)(payload >> 32);
    switch (event) {
    case 0: { /* SIGNAL: SIGLBLID.SIGID = ID under IDMSK */
        u32 cur = (u32)sGs.siglblid;
        sGs.siglblid = (sGs.siglblid & ~0xFFFFFFFFull) | ((cur & ~mask) | (id & mask));
        if (sGs.csr & 1)
            rt_log_once("gs", 0x10, "GS SIGNAL while the previous one is pending");
        sGs.csr |= 1;
        if (!(sGs.imr & 0x100))
            Kern_RaiseIntc(KERN_INTC_GS);
        break;
    }
    case 1: /* FINISH */
        sGs.csr |= 2;
        if (!(sGs.imr & 0x200))
            Kern_RaiseIntc(KERN_INTC_GS);
        break;
    case 2: { /* LABEL: SIGLBLID.LBLID = ID under IDMSK */
        u32 cur = (u32)(sGs.siglblid >> 32);
        sGs.siglblid = (sGs.siglblid & 0xFFFFFFFFull) | ((u64)((cur & ~mask) | (id & mask)) << 32);
        break;
    }
    }
}

/* The events in whole GIFtags: A+D writes of PACKED tags to SIGNAL (0x60),
 * FINISH (0x61) and LABEL (0x62); REGLIST descriptors reach registers 0x0-0xF
 * only (gs.c gs_off_events). */
static void FindEvents(const u8 *p, u32 qwords)
{
    u32 q = 0;
    while (q < qwords) {
        u64 t, regs;
        u32 nloop, flg, nreg, len, i;
        memcpy(&t, p + q * 16, 8);
        memcpy(&regs, p + q * 16 + 8, 8);
        nloop = (u32)(t & 0x7FFF);
        flg = (u32)(t >> 58) & 3;
        nreg = (u32)(t >> 60) & 15;
        if (!nreg)
            nreg = 16;
        len = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        q++;
        if (flg == 0) {
            for (i = 0; i < nloop * nreg && q + i < qwords; i++) {
                const u8 *d;
                u64 v;
                if (((regs >> ((i % nreg) * 4)) & 15) != 0xE)
                    continue;
                d = p + (q + i) * 16;
                memcpy(&v, d, 8);
                if (d[8] >= 0x60 && d[8] <= 0x62)
                    GsEvent(d[8] - 0x60, v);
            }
        }
        q += len;
    }
}

void gs_gif(int path, const void *data, u32 qwords)
{
    Record(GFX_GS_GIF, (u32)path, data, qwords * 16);
    FindEvents(data, qwords);
}

void gs_priv_write(u32 addr, u64 value)
{
    Record(GFX_GS_PRIV, addr, &value, 8);
}

int gs_read_fifo(void *dst, u32 qwords)
{
    if (sReadback) {
        sReadback(dst, qwords);
        return 1;
    }
    if (Ipc_GsReadback(dst, qwords))
        return 1;
    memset(dst, 0, qwords * 16);
    return 0;
}

/* ---- VBlank and the kernel library's calls ---- */

void Gfx_VBlankStart(u64 vblank)
{
    (void)vblank;
    sField ^= 1;
    sGs.csr |= 0x8; /* VSINT */
    if (!(sGs.imr & 0x800))
        Kern_RaiseIntc(KERN_INTC_GS);
}

/* The host's frame boundary (the original's gs_vblank_end(field, frame),
 * the original's hw_vblank): the field as VBlank start toggled it and the VBlank
 * number, to the host as a VSync, or to a test's sink. Not in the G line. */
__attribute__((weak)) u64 Pal_FreezeBias(void)   /* pal_freecam.c's: the pause's restores; the gfx tests run without it */
{
    return 0;
}

void Gfx_VBlankEnd(u64 vblank)
{
    if (sSink) {
        sSink(GFX_GS_VBLANK, (u32)vblank, &sField, 4);
        return;
    }
    Ipc_GsVSync(sField, vblank + Pal_FreezeBias());   /* the host sees VBlanks that go on through the pause's restores */
}

/* SMODE1 as the BIOS sets it for the video mode; the renderer needs CMOD
 * (NTSC/PAL) and LC (the original's hw_set_gs_crt). */
void Gfx_SetGsCrt(u32 interlace, u32 mode, u32 ffmd)
{
    switch (mode) {
    case 2: sGs.smode1 = 0x740834504ull; break; /* NTSC */
    case 3: sGs.smode1 = 0x740836504ull; break; /* PAL */
    default:
        rt_log_once("gs", 0x20 + mode, "SetGsCrt mode not supported; using NTSC");
        sGs.smode1 = 0x740834504ull;
        break;
    }
    sGs.smode2 = (interlace & 1) | ((ffmd & 1) << 1);
    gs_priv_write(0x12000010, sGs.smode1);
    gs_priv_write(0x12000020, sGs.smode2);
}

void Gfx_GsPutImr(u32 imr) { sGs.imr = imr; }
u32 Gfx_GsGetImr(void) { return (u32)sGs.imr; }
u64 Gfx_GsCsrForVSyncFlag(void) { return gs_read(0x12001000); }   /* the CSR as read: FIELD included */

/* SetVSyncFlag (syscall 0x73) as the kernel library's VSync wait (0x114040)
 * makes it: from then on, each VBlank start writes 1 to the flag and the CSR
 * word to the slot (the specification, 9, step 10; the original game never
 * clears the addresses). The kernel's step 10 does the writing
 * (Kern_SetVSyncFlag). The slots are the wait's own, here rather than on its
 * stack: Gfx_VSyncSlot reads the CSR
 * slot. */
static u32 sVsyncFlag;
static u64 sVsyncCsr;

void Gfx_SetVSyncFlag(void)
{
    sVsyncFlag = 0;
    Kern_SetVSyncFlag(&sVsyncFlag, &sVsyncCsr);
}

u32 Gfx_VSyncFlag(void)
{
    return sVsyncFlag;
}

u64 Gfx_VSyncSlot(void)
{
    return sVsyncCsr;
}

/* The kernel's start-up hook (kern.h), after its own Gfx_Init: the VU1 check
 * and the slots. The kernel calls Gfx_VBlankStart (step 8), the slots' writes
 * (step 10) and Gfx_VBlankEnd itself (kernel/vblank.c). */
void Kern_InitGfx(void)
{
    const char *check = Host_Getenv("MH2_VU1_CHECK");
    Gfx_Vu1Check(check && check[0] == '1');
    sVsyncFlag = 0;
    sVsyncCsr = 0;
}

/* ---- the privileged registers (the original's gs_reg, gs_write, gs_read) ---- */

static u64 *GsReg(u32 addr)
{
    switch (addr) {
    case 0x12000000: return &sGs.pmode;
    case 0x12000010: return &sGs.smode1;
    case 0x12000020: return &sGs.smode2;
    case 0x12000030: return &sGs.srfsh;
    case 0x12000040: return &sGs.synch1;
    case 0x12000050: return &sGs.synch2;
    case 0x12000060: return &sGs.syncv;
    case 0x12000070: return &sGs.dispfb1;
    case 0x12000080: return &sGs.display1;
    case 0x12000090: return &sGs.dispfb2;
    case 0x120000A0: return &sGs.display2;
    case 0x120000B0: return &sGs.extbuf;
    case 0x120000C0: return &sGs.extdata;
    case 0x120000D0: return &sGs.extwrite;
    case 0x120000E0: return &sGs.bgcolor;
    case 0x12001010: return &sGs.imr;
    case 0x12001040: return &sGs.busdir;
    case 0x12001080: return &sGs.siglblid;
    }
    return NULL;
}

/* A write of 64 bits (width64), or of the 32-bit half at addr. */
void gs_write(u32 addr, u64 v, int width64)
{
    u64 *r;
    if ((addr & ~7u) == 0x12001000) { /* CSR */
        if (addr & 4)
            return;
        if (v & 0x200) /* RESET */
            sGs.csr = 0;
        sGs.csr &= ~(v & 0x1F); /* a 1 clears SIGNAL, FINISH, HSINT, VSINT, EDWINT */
        return;
    }
    r = GsReg(addr & ~7u);
    if (!r) {
        rt_log_once("gs", addr, "write to an unknown GS register");
        return;
    }
    if (width64)
        *r = v;
    else if (addr & 4)
        *r = (*r & 0xFFFFFFFFull) | (v << 32);
    else
        *r = (*r & 0xFFFFFFFF00000000ull) | (v & 0xFFFFFFFFull);
    if ((addr & ~7u) <= 0x120000E0u) /* the display registers: the renderer needs them */
        gs_priv_write(addr & ~7u, *r);
}

u64 gs_read(u32 addr)
{
    u64 *r;
    if ((addr & ~7u) == 0x12001000) /* CSR: revision 0x1B, ID 0x55, FIFO empty, the field */
        return (0x55ull << 24) | (0x1Bull << 16) | (1ull << 14) | ((u64)sField << 13) | (sGs.csr & 0x1F);
    r = GsReg(addr & ~7u);
    return r ? *r : 0;
}

void gs_init(void)
{
    memset(&sGs, 0, sizeof(sGs));
    sGs.imr = 0x7F00;
    sField = 0;
}
