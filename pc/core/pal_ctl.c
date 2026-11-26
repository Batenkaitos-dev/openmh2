/*
 * The Frontier controls preset's translator: see pal_ctl.h.
 *
 * One call a VBlank, on the pad the game is about to be given. The state kept between calls is small: the buttons of the last
 * call, the buttons that are Dos's own until let go, how long Triangle, Circle and L1 have been down, and which of the two face
 * buttons a chord has used.
 */
#include "pal_ctl.h"
#include "host.h"

PalCtlCtx gPalCtlCtx;

static u32 sSeq;      /* the store count at the last call that saw it change */
static u64 sSeqAt;    /* the VBlank of that call */
static s32 sSeqSeen;  /* sSeq is valid */
static s32 sWasField; /* the last call was the field's play */


#define CTL_NOLOCK(c) 0

/* The rows, in Dos's weapon type order: 0 great sword, 1 heavy bowgun, 2 hammer, 3 lance, 4 sword and shield, 5 light bowgun,
 * 6 dual swords, 7 long sword, 8 hunting horn, 9 gunlance, 10 bow. Each action is what Dos reads for the move the second
 * layout puts on the button. */
#define N PAL_CTL_A_NONE
#define U PAL_CTL_A_UP
#define D PAL_CTL_A_DOWN
#define L PAL_CTL_A_LEFT
#define R PAL_CTL_A_RIGHT
#define K PAL_CTL_A_R3

static const PalCtlRow sRows[PAL_CTL_TYPES] = {
    /*            tri cir chord sel  r1tri r1cir r1chord flags */
    /* 0  GS  */ {U, L, R, K, N, N, N, PAL_CTL_F_TRI_HOLD},
    /* 1  HBG */ {N, K, U, N, N, K, D, 0},
    /* 2  HAM */ {U, D, N, N, N, N, N, 0},
    /* 3  LAN */ {U, D, K, K, N, N, N, 0},
    /* 4  SNS */ {U, R, D, K, K, N, N, 0},
    /* 5  LBG */ {N, K, U, N, N, K, D, 0},
    /* 6  DS  */ {U, L, D, N, N, N, N, PAL_CTL_F_R1_GATE},
    /* 7  LS  */ {U, K, D, N, N, N, N, 0},
    /* 8  HRN */ {L, D, U, N, N, N, N, PAL_CTL_F_TRI_LEFT_ALT},
    /* 9  GL  */ {U, K, D, L, N, D, K, 0},
    /* 10 BOW */ {D, U, K, N, N, N, N, 0},
};

#undef N
#undef U
#undef D
#undef L
#undef R
#undef K

/* The horn in a play stance: the notes (Triangle up, Circle right, both down; Square, the left note, is read apart). */
static const PalCtlRow sHornStance = {PAL_CTL_A_UP, PAL_CTL_A_RIGHT, PAL_CTL_A_DOWN, PAL_CTL_A_NONE, PAL_CTL_A_NONE, PAL_CTL_A_NONE, PAL_CTL_A_NONE, 0};

/* The states of PL_GROUP_WEAPON (1) in which the horn reads its notes (HuntingHorn_PlayInput): the stances 0 to 7 (the blends and the
 * four notes) and the three that enter the down stance from a swing. */
s32 PalCtl_HornStance(u32 state)
{
    u32 s = state & 0xFF;
    if ((state >> 8) != 1) {
        return 0;
    }
    return s <= 0x07 || s == 0x16 || s == 0x24 || s == 0x27;
}

const PalCtlRow *PalCtl_StanceRow(void)
{
    return &sHornStance;
}

/* The buttons that are the translator's (the others pass through untouched). */
#define TRANSLATED                                                                                                          \
    (PAL_CTL_TRIANGLE | PAL_CTL_CIRCLE | PAL_CTL_SELECT | PAL_CTL_R3 | PAL_CTL_L1 | PAL_CTL_L2 | PAL_CTL_R1 | PAL_CTL_R2 | PAL_CTL_SQUARE)

#define PUSH 37   /* Dos reads a stick as pushed from 37 (Pad_StickDirBits) */

static s32 sOn;

static struct {
    u16 prev;        /* the physical buttons at the last call */
    u16 latch;       /* buttons held from before the context became play, or first pressed while Circle was Dos's own: Dos's own until let go */
    u8 age[2];       /* calls held: Triangle, Circle (stops at 255) */
    u8 used[2];      /* a chord took this button: no attack from it until it is let go */
    u8 fired[2];     /* the lone attack has been given at least once in this press */
    u8 l1age;
    u8 last;         /* the action given at the last call (stick or R3), and for how many calls in a row (stops at 255) */
    u8 run;
    u16 drop;        /* buttons the preset was translating when the context went: ignored, and withheld, until let go */
} st;

const PalCtlRow *PalCtl_Row(s32 type)
{
    if (type < 0 || type >= PAL_CTL_TYPES) {
        return 0;
    }
    return &sRows[type];
}

void PalCtl_Set(s32 on)
{
    sOn = on != 0;
    PalCtl_Reset();
}

s32 PalCtl_IsOn(void)
{
    return sOn;
}

static s32 StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

s32 PalCtl_Option(s32 argc, char **argv, s32 i)
{
    if (i < argc && StrEq(argv[i], "--frontier-controls")) {
        PalCtl_Set(1);
        return 1;
    }
    return 0;
}

void PalCtl_Reset(void)
{
    sWasField = 0;
    u32 i;
    u8 *p = (u8 *)&st;
    for (i = 0; i < sizeof(st); i++) {
        p[i] = 0;
    }
}

static void Stick(PalCtlPad *pad, s32 act)
{
    switch (act) {
    case PAL_CTL_A_UP:
        pad->rx = 0x80;
        pad->ry = 0x00;
        break;
    case PAL_CTL_A_DOWN:
        pad->rx = 0x80;
        pad->ry = 0xFF;
        break;
    case PAL_CTL_A_LEFT:
        pad->rx = 0x00;
        pad->ry = 0x80;
        break;
    case PAL_CTL_A_RIGHT:
        pad->rx = 0xFF;
        pad->ry = 0x80;
        break;
    default:
        break;
    }
}

static void Give(PalCtlPad *pad, u16 bit, s32 slot)
{
    pad->buttons |= bit;
    if (slot >= 0) {
        pad->pressure[slot] = 255;
    }
}

static void Withhold(PalCtlPad *pad, u16 bit, s32 slot)
{
    pad->buttons &= (u16) ~bit;
    if (slot >= 0) {
        pad->pressure[slot] = 0;
    }
}

/* the buttons a press of which the preset translates (they are withheld from the game, apart from the latch) */
#define DROPSET (PAL_CTL_TRIANGLE | PAL_CTL_CIRCLE | PAL_CTL_SELECT | PAL_CTL_R3 | PAL_CTL_L2 | PAL_CTL_R2)

static void DropWithhold(PalCtlPad *pad)
{
    if (st.drop & PAL_CTL_TRIANGLE) {
        Withhold(pad, PAL_CTL_TRIANGLE, PAL_CTL_P_TRIANGLE);
    }
    if (st.drop & PAL_CTL_CIRCLE) {
        Withhold(pad, PAL_CTL_CIRCLE, PAL_CTL_P_CIRCLE);
    }
    if (st.drop & PAL_CTL_SELECT) {
        Withhold(pad, PAL_CTL_SELECT, -1);
    }
    if (st.drop & PAL_CTL_R3) {
        Withhold(pad, PAL_CTL_R3, -1);
    }
    if (st.drop & PAL_CTL_L2) {
        Withhold(pad, PAL_CTL_L2, PAL_CTL_P_L2);
    }
    if (st.drop & PAL_CTL_R2) {
        Withhold(pad, PAL_CTL_R2, PAL_CTL_P_R2);
    }
    /* a held remap goes on: L2 is Dos's Triangle (the chat) and R3 is Dos's Select (the map's zoom) until let go, whatever the context */
    if (st.drop & PAL_CTL_L2) {
        Give(pad, PAL_CTL_TRIANGLE, PAL_CTL_P_TRIANGLE);
    }
    if (st.drop & PAL_CTL_R3) {
        Give(pad, PAL_CTL_SELECT, -1);
    }
}

static s32 LeftStickLeft(const PalCtlPad *pad)
{
    s32 dx = (s32)pad->lx - 128;
    s32 dy = (s32)pad->ly - 128;
    if (dy < 0) {
        dy = -dy;
    }
    return dx <= -PUSH && -dx >= dy;
}

static void RunInner(const PalCtlCtx *ctx, u64 now, PalCtlPad *pad)
{
    const PalCtlRow *row;
    u16 held = pad->buttons;
    u16 newly;
    s32 play;
    s32 drawn;
    s32 tri, cir, sel, r3, l1, l2, r1;
    s32 r1on;
    s32 stance, sq;
    s32 act = PAL_CTL_A_NONE;      /* the attack's action this call */
    s32 selAct = PAL_CTL_A_NONE;
    u8 pa[2], pf[2], pu[2];   /* the last call's age, lone attack given, chord used */
    s32 triAct, cirAct, chordAct, chordExists;
    s32 i;

    if (!sOn) {
        return;
    }
    if (ctx != 0 && (!sSeqSeen || ctx->seq != sSeq)) {
        sSeq = ctx->seq;
        sSeqAt = now;
        sSeqSeen = 1;
    }
    play = ctx != 0 && ctx->set && (ctx->locked == 0 || ctx->locked == PAL_CTL_TOWN || CTL_NOLOCK(ctx)) && ctx->type < PAL_CTL_TYPES && now >= sSeqAt && now - sSeqAt <= PAL_CTL_STALE;
    if (!play) {
        /* what is held now is Dos's own until it is let go, except a press the preset was translating a call ago (the attack's button, L2 for the chat,
         * R3 and Select): the game saw that button withheld, so it would see a fresh press of it (it could confirm a window that the press opened) */
        u16 drop = (u16)(st.drop & held);
        if (sWasField) {
            drop |= (u16)(held & st.prev & ~st.latch & DROPSET);
        }
        PalCtl_Reset();
        sWasField = 0;
        st.drop = drop;
        st.latch = (u16)(held & TRANSLATED & ~drop);
        st.prev = held;
        DropWithhold(pad);
        return;
    }
    if (ctx->locked == PAL_CTL_TOWN) {
        sWasField = 0;
        /* the village and the town: Triangle is the chat and every other button is Dos's own; the run is on R1 as in the field */
        st.latch &= held;
        if ((held & PAL_CTL_R1) && !(st.latch & PAL_CTL_R1) && ctx->drawn == 0) {
            Withhold(pad, PAL_CTL_R1, PAL_CTL_P_R1);
            Give(pad, PAL_CTL_R2, PAL_CTL_P_R2);
        }
        st.prev = held;
        return;
    }
    sWasField = 1;
    st.drop &= held;
    held &= (u16)~st.drop;   /* a press the preset was translating when the context went: ignored until let go */
    row = &sRows[ctx->type];
    drawn = ctx->drawn != 0;
    stance = ctx->type == 8 && drawn && PalCtl_HornStance(ctx->state);   /* the horn reads its notes: the stance's row */
    if (stance) {
        row = &sHornStance;
    }
    st.latch &= held;
    newly = (u16)(held & ~st.prev);
    if ((newly & PAL_CTL_CIRCLE) && !drawn) {
        st.latch |= PAL_CTL_CIRCLE;   /* Dos's own Circle (gather, talk, carve): not an attack when the weapon comes out */
    }

    if ((newly & PAL_CTL_SQUARE) && !stance) {
        st.latch |= PAL_CTL_SQUARE;   /* Dos's own Square (the sheathe): not a note when a stance comes while it is held */
    }
    sq = stance && (held & PAL_CTL_SQUARE) && !(st.latch & PAL_CTL_SQUARE);
    tri = (held & PAL_CTL_TRIANGLE) && !(st.latch & PAL_CTL_TRIANGLE);
    cir = (held & PAL_CTL_CIRCLE) && !(st.latch & PAL_CTL_CIRCLE) && drawn;
    sel = (held & PAL_CTL_SELECT) && !(st.latch & PAL_CTL_SELECT);
    r3 = (held & PAL_CTL_R3) && !(st.latch & PAL_CTL_R3);
    l1 = (held & PAL_CTL_L1) && !(st.latch & PAL_CTL_L1);
    l2 = (held & PAL_CTL_L2) && !(st.latch & PAL_CTL_L2);
    r1 = (held & PAL_CTL_R1) && !(st.latch & PAL_CTL_R1);
    r1on = r1 && drawn;

    /* ages and the chord's bookkeeping */
    for (i = 0; i < 2; i++) {
        pa[i] = st.age[i];
        pf[i] = st.fired[i];
        pu[i] = st.used[i];
        s32 down = i == 0 ? tri : cir;
        if (down) {
            if (st.age[i] < 255) {
                st.age[i]++;
            }
        } else {
            st.age[i] = 0;
            st.used[i] = 0;
            st.fired[i] = 0;
        }
    }

    triAct = r1on ? row->r1tri : row->tri;
    if (!drawn && triAct == PAL_CTL_A_NONE && (ctx->type == 1 || ctx->type == 5)) {
        triAct = PAL_CTL_A_UP;   /* a sheathed bowgun is drawn by a flick of the right stick: Triangle draws it (once drawn Triangle does nothing) */
    }
    cirAct = r1on ? row->r1cir : row->cir;
    chordAct = r1on ? row->r1chord : row->chord;
    chordExists = chordAct != PAL_CTL_A_NONE || (r1on && (row->flags & PAL_CTL_F_R1_GATE));

    if (tri && cir) {
        /* both down: the chord's action, even when it is none; neither attacks alone until it is let go */
        st.used[0] = st.used[1] = 1;
        act = chordAct;
    } else if (tri || cir) {
        s32 b = tri ? 0 : 1;
        if (!st.used[b]) {
            if (b == 0 && (row->flags & PAL_CTL_F_TRI_HOLD) && !r1on) {
                /* the great sword: Triangle held is the charge (the stick down while held); a press let go sooner is the overhead */
                if (st.age[0] >= PAL_CTL_CHARGE_HOLD) {
                    act = PAL_CTL_A_DOWN;
                    st.fired[0] = 1;
                }
            } else {
                s32 go = !chordExists || st.age[b] >= PAL_CTL_CHORD_WAIT;
                if (go) {
                    act = tri ? triAct : cirAct;
                    st.fired[b] = 1;
                }
            }
        }
    }
    /* a lone press let go inside the wait is still an attack: one call of it */
    for (i = 0; i < 2; i++) {
        s32 down = i == 0 ? tri : cir;
        u16 bit = i == 0 ? PAL_CTL_TRIANGLE : PAL_CTL_CIRCLE;
        if (!down && (st.prev & bit) && !(st.latch & bit) && pa[i] >= 1 && !pf[i] && !pu[i] && chordExists &&
            act == PAL_CTL_A_NONE && (i == 0 || drawn)) {
            act = i == 0 ? triAct : cirAct;
        }
    }

    /* Triangle with the left stick pushed left: the horn's other sweep */
    if ((row->flags & PAL_CTL_F_TRI_LEFT_ALT) && act == PAL_CTL_A_LEFT && tri && !cir && LeftStickLeft(pad)) {
        act = PAL_CTL_A_RIGHT;
    }

    /* in a stance Square is the left note */
    if (sq && act == PAL_CTL_A_NONE) {
        act = PAL_CTL_A_LEFT;
    }

    /* Select: the kick or the type's R3 move */
    if (sel) {
        selAct = row->sel;
    }

    /* The game reads the pad once a frame, every second VBlank, and sees an edge only if a read falls on it: an action given for one VBlank is seen
     * on half of the frames (a short tap of Triangle on the great sword, a press let go inside the chord's wait). An action that would be given for
     * one call only is given for two. */
    {
        s32 cur = act != PAL_CTL_A_NONE ? act : selAct;
        if (cur != PAL_CTL_A_NONE) {
            if (cur == st.last) {
                if (st.run < 255) {
                    st.run++;
                }
            } else {
                st.last = (u8)cur;
                st.run = 1;
            }
        } else if (st.run == 1 && st.last != PAL_CTL_A_NONE) {
            act = st.last;
            selAct = PAL_CTL_A_NONE;
            st.run = 2;
        } else {
            st.last = PAL_CTL_A_NONE;
            st.run = 0;
        }
    }

    /* the output */
    pad->rx = 0x80;   /* the right stick is the camera's: the game is given it at rest, or the attack's push */
    pad->ry = 0x80;
    if (act >= PAL_CTL_A_UP && act <= PAL_CTL_A_RIGHT) {
        Stick(pad, act);
    } else if (selAct >= PAL_CTL_A_UP && selAct <= PAL_CTL_A_RIGHT) {
        Stick(pad, selAct);
    }
    /* Triangle and Circle are the attack's: withheld; Triangle is given for L2 (the chat) */
    if (!(st.latch & PAL_CTL_TRIANGLE)) {
        Withhold(pad, PAL_CTL_TRIANGLE, PAL_CTL_P_TRIANGLE);
    }
    if (drawn && !(st.latch & PAL_CTL_CIRCLE)) {
        Withhold(pad, PAL_CTL_CIRCLE, PAL_CTL_P_CIRCLE);
    }
    if (!(st.latch & PAL_CTL_SELECT)) {
        Withhold(pad, PAL_CTL_SELECT, -1);
    }
    if (stance && !(st.latch & PAL_CTL_SQUARE)) {
        Withhold(pad, PAL_CTL_SQUARE, PAL_CTL_P_SQUARE);
    }
    if (!(st.latch & PAL_CTL_R3)) {
        Withhold(pad, PAL_CTL_R3, -1);
    }
    if (!(st.latch & PAL_CTL_L2)) {
        Withhold(pad, PAL_CTL_L2, PAL_CTL_P_L2);
    }
    if (!(st.latch & PAL_CTL_R2)) {
        Withhold(pad, PAL_CTL_R2, PAL_CTL_P_R2);
    }
    if (act == PAL_CTL_A_R3 || selAct == PAL_CTL_A_R3) {
        Give(pad, PAL_CTL_R3, -1);
    }
    if (r3) {
        Give(pad, PAL_CTL_SELECT, -1);   /* the map's zoom */
    }
    if (l2) {
        Give(pad, PAL_CTL_TRIANGLE, PAL_CTL_P_TRIANGLE);   /* the chat */
    }
    if (l1) {
        if (st.l1age < 255) {
            st.l1age++;
        }
        if (st.l1age >= PAL_CTL_L1_HOLD) {
            Give(pad, PAL_CTL_L2, PAL_CTL_P_L2);   /* the item bar */
        }
    } else {
        st.l1age = 0;
    }
    if (r1) {
        if (!drawn) {
            Withhold(pad, PAL_CTL_R1, PAL_CTL_P_R1);
            Give(pad, PAL_CTL_R2, PAL_CTL_P_R2);   /* the run */
        } else if ((row->flags & PAL_CTL_F_R1_GATE) && !(tri && cir)) {
            Withhold(pad, PAL_CTL_R1, PAL_CTL_P_R1);
        }
    }
    DropWithhold(pad);
    st.prev = held;
}

void PalCtl_Run(const PalCtlCtx *ctx, u64 now, PalCtlPad *pad)
{
    RunInner(ctx, now, pad);
}
