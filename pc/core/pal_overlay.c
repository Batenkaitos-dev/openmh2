/*
 * Overlay activation. A window is one memory, the RAM block's own
 * range, as on the PS2: every name of an overlay's .data, .rodata and .bss is
 * Pal_EeRam + its PS2 address (the data layout, pc/core/data_c.h), so
 * the overlays that share a window share its bytes, and a name's page offset
 * and alignment are the PS2's by construction.
 *
 * What the PS2 does at a load (as the original game's run shows):
 * Overlay_Relocate (src/main/main.c) copies the file's image
 * (text, .data, .rodata) to the header's load address, then the runtime's
 * start-up func_00128550 clears [load + image size, + the header's .bss size),
 * the first load included. Nothing else: the window's other bytes keep what
 * they held.
 *
 * On the PC main.c's copy has put the disc's image at PAL_RAM(load): the
 * matching build's bytes, so the PS2's everywhere but in the words that hold
 * an address. Pal_OverlayActivate then
 *   1. writes the overlay's address words (DataC_OverlayWords: PS2 address,
 *      the PC's value of the same target) over the image;
 *   2. clears [load + imageSize, + bssSize) in the RAM block (func_00128550's
 *      range); for sub_main, whose names are the resident block
 *      DataC_sub_main_Bss rather than the RAM block, that block's own PS2 .bss
 *      range too;
 *   3. makes it the window's active overlay (Pal_OverlayWindowActive: what a
 *      call by address reaches).
 *
 * MH2_OVERLAY_BSS (a test of this file, plan 4.5):
 *   clear  (default) as above: what the PS2 does;
 *   poison as clear, and first fill with 0xA5 the window's bytes past this
 *          load's image and .bss that an earlier load of the window wrote (its
 *          image or .bss): another overlay's leftovers, which the PS2 and the
 *          default keep. A run that differs from the default names a read of
 *          them.
 */
#include "pal_core.h"
#include "pal_ovcall.h"
#include "data_c.h"

#include "host.h"

typedef struct PalOverlay {
    int id;                       /* the OverlayId */
    const char *name;
    int group;                    /* the window: overlays of one group are never active together */
} PalOverlay;

/* The overlays with C (gen_stubs.py reads these OV(id, module, window) lines). */
#define OV(i, m, g) { i, #m, g }
static const PalOverlay sOverlays[] = {
    OV(1, sub_main, 0),
    OV(2, select, 1), OV(3, game, 1), OV(4, lobby, 1),          /* the scene window, 0x6AF200 */
    OV(5, nethttp, 2), OV(6, gm_sub, 2), OV(10, lbguild, 2),    /* 0x8FAE00 */
    OV(9, yn, 3),                                               /* 0xAC8000 */
    OV(11, plsel, 4),                                           /* 0xD60200 */
};
#define OVERLAY_COUNT ((int)(sizeof(sOverlays) / sizeof(sOverlays[0])))

/* The overlays with no C (the DNAS ones, in yn's window). */
static const PalOverlay sNoC[] = { { 7, "dnas_net", 3 }, { 8, "dnas_ins", 3 } };
#define NOC_COUNT ((int)(sizeof(sNoC) / sizeof(sNoC[0])))

/* sub_main's names are the resident block's (data_sub_main.c), not the RAM block's. */
extern unsigned char DataC_sub_main_BssLo[], DataC_sub_main_BssHi[];

/* The overlay active in each window (pal_ovcall.h): what a call by address reaches. */
int Pal_OverlayWindowActive[PAL_OVCALL_WINDOWS];

/* MH2_OVERLAY_BSS=poison: the end (a PS2 address) of what the window's loads have written so far. */
static unsigned int sWindowHigh[PAL_OVCALL_WINDOWS];

static int sPoison;
static int sInited;

static int StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void Fill(unsigned int lo, unsigned int hi, unsigned char v)
{
    if (hi > sizeof(Pal_EeRam)) {
        Pal_Fatal("Pal_OverlayActivate: an overlay's range runs past the RAM block");
    }
    if (lo < hi) {
        __builtin_memset(Pal_EeRam + lo, v, hi - lo);
    }
}

void Pal_OverlayInit(void)
{
    const char *v;

    if (sInited) {
        return;
    }
    sInited = 1;
    v = Host_Getenv("MH2_OVERLAY_BSS");
    if (v && StrEq(v, "poison")) {
        sPoison = 1;
    } else if (v && !StrEq(v, "clear")) {
        Pal_Fatal("MH2_OVERLAY_BSS must be clear or poison (keep is gone: a window is one memory)");
    }
    v = Host_Getenv("MH2_OVERLAY_VERIFY");
    if (v && v[0]) {
        Pal_Fatal("MH2_OVERLAY_VERIFY is gone: a window is one memory, as on the PS2 (pal_overlay.c)");
    }
}

void Pal_OverlayActivate(int id, unsigned int load, int imageSize, int bssSize)
{
    const PalOverlay *o = 0;
    const DataCWord *w;
    unsigned int i, count, end;

    Pal_OverlayInit();
    for (i = 0; i < (unsigned int)OVERLAY_COUNT; i++) {
        if (sOverlays[i].id == id) {
            o = &sOverlays[i];
        }
    }
    for (i = 0; !o && i < (unsigned int)NOC_COUNT; i++) {
        if (sNoC[i].id == id) {
            o = &sNoC[i];
        }
    }
    if (imageSize < 0 || bssSize < 0) {
        Pal_Fatal("Pal_OverlayActivate: a negative size");
    }
    end = load + (unsigned int)imageSize + (unsigned int)bssSize;
    if (end < load || end > sizeof(Pal_EeRam)) {
        Pal_Fatal("Pal_OverlayActivate: an overlay's range runs past the RAM block");
    }

    /* MH2_OVERLAY_BSS=poison: the window's leftovers past this load. */
    if (o && sPoison && o->group != 0) {
        Fill(end, sWindowHigh[o->group], 0xA5);
    }
    if (o && end > sWindowHigh[o->group]) {
        sWindowHigh[o->group] = end;
    }

    /* 1. The address words over the disc's image. */
    w = DataC_OverlayWords(id, &count);
    for (i = 0; i < count; i++) {
        if (w[i].ps2 < load || w[i].ps2 + 4 > load + (unsigned int)imageSize || (w[i].ps2 & 3)) {
            Pal_Fatal("Pal_OverlayActivate: an address word outside the overlay's image");
        }
        *(unsigned int *)(Pal_EeRam + w[i].ps2) = w[i].value;
    }

    /* 2. The .bss, as func_00128550 clears it. */
    Fill(load + (unsigned int)imageSize, end, 0);
    if (id == 1) {
        __builtin_memset(DataC_sub_main_BssLo, 0, (unsigned int)(DataC_sub_main_BssHi - DataC_sub_main_BssLo));
    }

    /* 3. The window's active overlay (an OverlayId this file doesn't list, a test's: no window). */
    if (o) {
        Pal_OverlayWindowActive[o->group] = id;
    }
}

/* ---- calls by address (pal_ovcall.h) ---- */

static char *Append(char *p, char *end, const char *s)
{
    while (*s && p < end) {
        *p++ = *s++;
    }
    return p;
}

static char *AppendHex(char *p, char *end, unsigned int v)
{
    char b[9];
    int i;

    for (i = 7; i >= 0; i--) {
        b[i] = "0123456789ABCDEF"[v & 15];
        v >>= 4;
    }
    b[8] = 0;
    return Append(p, end, b);
}

static const char *OverlayName(int id)
{
    int i;

    for (i = 0; i < OVERLAY_COUNT; i++) {
        if (sOverlays[i].id == id) {
            return sOverlays[i].name;
        }
    }
    for (i = 0; i < NOC_COUNT; i++) {
        if (sNoC[i].id == id) {
            return sNoC[i].name;
        }
    }
    return "an overlay with no C";
}

void Pal_OverlayCallMissing(const char *stub, unsigned int addr, int window, const char *known)
{
    static char msg[512];
    char *p = msg, *end = msg + sizeof(msg) - 1;
    int i, first = 1, active;

    active = window >= 0 && window < PAL_OVCALL_WINDOWS ? Pal_OverlayWindowActive[window] : 0;
    p = Append(p, end, stub);
    p = Append(p, end, ": a call to 0x");
    p = AppendHex(p, end, addr);
    p = Append(p, end, " in the window of ");
    for (i = 0; i < OVERLAY_COUNT; i++) {
        if (sOverlays[i].group == window) {
            p = Append(p, end, first ? "" : ", ");
            p = Append(p, end, sOverlays[i].name);
            first = 0;
        }
    }
    if (active == 0) {
        p = Append(p, end, ", where no overlay is loaded");
    } else {
        p = Append(p, end, ", where ");
        p = Append(p, end, OverlayName(active));
        p = Append(p, end, " is loaded and has no PC function there");
    }
    p = Append(p, end, " (");
    p = Append(p, end, known);
    p = Append(p, end, ")");
    *p = 0;
    Pal_Fatal(msg);
}
