/*
 * The 2D picture's shape under the widescreen enhancement (include/pal.h: PC_HUD_2D, PC_HUD_PACKET; level 2).
 *
 * With widescreen on, the game's 640 x 448 picture is shown at 16:9, 4/3 as wide as it is drawn. The camera takes
 * care of the 3D picture (Pal_CameraAspect); the 2D elements (the gauges, the menus, the text) would be 4/3 too wide.
 * So each 2D primitive's x coordinates are moved to 3/4 of their distance from the picture's centre, which is
 * what the display then stretches back: the element has its own shape, in the middle 480 columns of the frame.
 *
 * The primitives are rewritten in the packet the game's 2D emitters (src/main/prim.c) built, right before it is queued:
 * one place for the ten emitters and everything that reaches them (277 direct sprite calls, the sprite queue, the
 * menus' and the fonts' glyphs). The x of a vertex is the GS's XYZ2 register field, 12.4 fixed point, so
 * x' = c + (x - c) * 3 / 4 is integer arithmetic (C's division, toward zero, so that the left and right of a symmetric
 * element stay symmetric); c is the GS's coordinate centre (the middle of the frame) or, for an element placed from a
 * 3D point (Pal_HudPivot), that point's x. A primitive that spans the frame's whole width (a fade, a background, a
 * letterbox bar) is left as it is: it covers the stretched picture's edges too. Outside a pivot, a vertex that is at
 * the frame's left or right edge (or past it) stays there, so a band or a window that touches the screen's edge in
 * the original still reaches it; every other vertex follows the 3/4 map, so pieces that share a vertex stay joined.
 * The frame is 512 pixels wide in the
 * game's usual display mode (the display stretches it to 640) and 640 in the other; the emitters take 640-wide
 * coordinates and scale them to it, so the pivot, which is in those coordinates, is scaled the same way.
 */
#include "pal_core.h"

#include "pal.h"

#define GS_CENTRE (2048 << 4)          /* the GS's coordinate space is 4096 pixels wide; the frame is centred in it */
#define GIF_REGLIST 1
#define REG_XYZ2 5
#define REG_XYZ3 0xD

static s32 sHiding;        /* the stage in progress is one the freecam's pose hides: its 2D packets draw nothing */
static s32 sHideThis;      /* the packet Pal_HudPacket saw is to be hidden once it is queued (Pal_HudQueued) */
static u32 sHidden, sUnparsed;   /* packets hidden, and packets of a shape the hide can't read (left as they were): counts */
static s32 sMarked;        /* the packet to send is a 2D emitter's */
static s32 sPivot;         /* the pivot's x in the 640-wide coordinates the emitters take, when sPivoted */
static s32 sPivoted;

void Pal_HudMark(void)
{
    sMarked = 1;
}

void Pal_HudHide(s32 on)
{
    sHiding = on;
}

void Pal_HudHideStats(u32 *hidden, u32 *unparsed)
{
    *hidden = sHidden;
    *unparsed = sUnparsed;
}

void Pal_HudPivot(s32 x)
{
    sPivot = x;
    sPivoted = 1;
}

void Pal_HudPivotEnd(void)
{
    sPivoted = 0;
}

static s32 Shrink(s32 x16, s32 centre)
{
    return centre + ((x16 - centre) * 3) / 4;
}

/* The packet's vertex registers: the GIF tag at +16 (NLOOP, 15 bits; FLG, bits 58-59; NREG, bits 60-63) and the register
 * list at +24, the data from +32 with one 64-bit word for each register. Returns how many XYZ2/XYZ3 words (their addresses
 * in xyz), or -1 for a packet of another shape (not one register list of one loop). */
static s32 Verts(u32 *packet, u32 *xyz[16])
{
    s32 nreg, i, count = 0;

    if ((packet[4] & 0x7FFF) != 1 || ((packet[5] >> 26) & 3) != GIF_REGLIST) {
        return -1;
    }
    nreg = (s32)(packet[5] >> 28);
    if (nreg == 0) {
        nreg = 16;
    }
    for (i = 0; i < nreg; i++) {
        u32 reg = (packet[6 + (i >> 3)] >> ((i & 7) * 4)) & 0xF;
        if (reg == REG_XYZ2 || reg == REG_XYZ3) {
            xyz[count++] = &packet[8 + i * 2];
        }
    }
    return count;
}

/* The packet as it was queued (the copy the DMA chain holds, or the glyph's own place in the font's buffer), after
 * Pal_HudPacket: when the stage is hidden, all its vertices go to x = 0, which is outside the scissor (the frame is drawn
 * about x = 2048), so a sprite, a triangle and a line each cover no pixel. The packet is built, queued and sent as the game
 * has it: its place in the buffers, the queue's words and the emitter's scratchpad are the same. */
void Pal_HudQueued(u32 *queued)
{
    s32 i, count;
    u32 *xyz[16];

    if (!sHideThis) {
        return;
    }
    sHideThis = 0;
    count = Verts(queued, xyz);
    if (count < 0) {
        sUnparsed++;
        return;
    }
    for (i = 0; i < count; i++) {
        *xyz[i] &= 0xFFFF0000u;
    }
    sHidden++;
}

void Pal_HudPacket(u32 *packet, s32 frameWidth)
{
    s32 marked = sMarked;
    s32 i, count;
    s32 first = 0, last = 0;
    s32 left, right, centre;
    u32 *xyz[16];

    sMarked = 0;
    sHideThis = marked && sHiding;
    if (sHideThis) {
        return;   /* the packet is left as the emitter built it (the scratchpad keeps what it keeps); its queued copy is hidden */
    }
    if (!marked || Pal_WidescreenLevel() < 2 || frameWidth < 64 || frameWidth > 1024) {
        return;
    }
    count = Verts(packet, xyz);
    for (i = 0; i < count; i++) {
        s32 x = (s32)(*xyz[i] & 0xFFFF);
        if (i == 0 || x < first) {
            first = x;
        }
        if (i == 0 || x > last) {
            last = x;
        }
    }
    left = GS_CENTRE - (frameWidth / 2 << 4);
    right = GS_CENTRE + (frameWidth / 2 << 4);
    if (count == 0 || (first <= left && last >= right)) {
        return;
    }
    centre = sPivoted ? left + (sPivot * frameWidth) / 40 : GS_CENTRE;
    for (i = 0; i < count; i++) {
        s32 vx = (s32)(*xyz[i] & 0xFFFF);
        s32 x = (!sPivoted && (vx <= left || vx >= right)) ? vx : Shrink(vx, centre);
        *xyz[i] = (*xyz[i] & 0xFFFF0000u) | ((u32)x & 0xFFFFu);
    }
}
