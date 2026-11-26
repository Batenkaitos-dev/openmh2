#ifndef PAL_CTL_H
#define PAL_CTL_H

/*
 * The Frontier controls preset's translator.
 *
 * The game is given Dos's own pad. With the preset on, the face buttons of the second layout (Triangle and Circle attack, Select
 * kicks, the right stick is the camera's) are turned into what Dos reads for the same move: the right stick pushed fully and
 * straight one way, or R3, with the face button itself withheld. The translation runs on the pad the platform gives the game
 * (the core's PadFromHost, before Io_PadSetState), so no reader of the game changes.
 *
 * What it needs from the game is three numbers, stored by Player_CopyPadInput under PLATFORM_PC into gPalCtlCtx and read here:
 * the hunter's weapon type, whether the weapon is drawn, and whether the field's input is locked, with a count that rises at each
 * store. A count that has not risen for PAL_CTL_STALE VBlanks is the context "none" (the village, the title, a load): the output is then the input.
 * With the preset off, or the context none or locked, the output is the input to the bit.
 */
#include "common.h"
#include "pal.h"

/* The pad as the platform layer holds it: the buttons in libpad2's bit order (active high), the sticks (0x80 at rest, small
 * values up and left) and the 12 pressures (right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2). */
typedef struct PalCtlPad {
    u16 buttons;
    u8 rx, ry, lx, ly;
    u8 pressure[12];
} PalCtlPad;

#define PAL_CTL_SELECT 0x0001
#define PAL_CTL_L3 0x0002
#define PAL_CTL_R3 0x0004
#define PAL_CTL_START 0x0008
#define PAL_CTL_UP 0x0010
#define PAL_CTL_RIGHT 0x0020
#define PAL_CTL_DOWN 0x0040
#define PAL_CTL_LEFT 0x0080
#define PAL_CTL_L2 0x0100
#define PAL_CTL_R2 0x0200
#define PAL_CTL_L1 0x0400
#define PAL_CTL_R1 0x0800
#define PAL_CTL_TRIANGLE 0x1000
#define PAL_CTL_CIRCLE 0x2000
#define PAL_CTL_CROSS 0x4000
#define PAL_CTL_SQUARE 0x8000

/* the pressure slots */
#define PAL_CTL_P_TRIANGLE 4
#define PAL_CTL_P_CIRCLE 5
#define PAL_CTL_P_SQUARE 7
#define PAL_CTL_P_L1 8
#define PAL_CTL_P_R1 9
#define PAL_CTL_P_L2 10
#define PAL_CTL_P_R2 11

/* What the game stores (Player_CopyPadInput, through PC_CTL_STORE): PalCtlCtx and gPalCtlCtx are in include/pal.h. */

#define PAL_CTL_TOWN 2           /* the locked number of the town's free input: only the run is the preset's there, every other button is Dos's own */
#define PAL_CTL_STALE 6          /* a store count that has not risen for this many VBlanks is the context none */
#define PAL_CTL_CHORD_WAIT 2     /* a lone press of Triangle or Circle waits this many VBlanks for the other (one game frame) */
#define PAL_CTL_L1_HOLD 10       /* L1 held this many VBlanks gives Dos's L2 (the item bar) */
#define PAL_CTL_CHARGE_HOLD 8    /* Triangle held this many VBlanks is the great sword's charge (a guess: the owner's play tunes it) */

/* What a button of the second layout does, in Dos's inputs. */
enum {
    PAL_CTL_A_NONE = 0,   /* nothing: the move is not Dos's */
    PAL_CTL_A_UP,         /* the right stick pushed fully up, while the button is held */
    PAL_CTL_A_DOWN,
    PAL_CTL_A_LEFT,
    PAL_CTL_A_RIGHT,
    PAL_CTL_A_R3          /* Dos's R3 */
};

/* One weapon type's row. The r1 fields are the actions with R1 held. */
typedef struct PalCtlRow {
    u8 tri, cir, chord, sel;
    u8 r1tri, r1cir, r1chord;
    u8 flags;
} PalCtlRow;

#define PAL_CTL_F_R1_GATE 0x01      /* R1 reaches the game only while Triangle and Circle are both held (the dual swords' demon mode) */
#define PAL_CTL_F_TRI_LEFT_ALT 0x02 /* Triangle with the left stick pushed left gives the right sweep (the horn) */
#define PAL_CTL_F_TRI_HOLD 0x04     /* Triangle held PAL_CTL_CHARGE_HOLD VBlanks gives the stick down for as long as it is held (the great sword's charge); a shorter press gives the row's action when it is let go */

#define PAL_CTL_TYPES 11

const PalCtlRow *PalCtl_Row(s32 type);

/* The hunting horn's play stances: the state (group in the high byte, state in the low byte, as the game stores it) is one in which the four notes
 * are read from the right stick. There Triangle, Square, Circle and both are the up, left, right and down notes. */
s32 PalCtl_HornStance(u32 state);
const PalCtlRow *PalCtl_StanceRow(void);

/* The preset on or off (off at start), and a reset of the translator's memory. */
void PalCtl_Set(s32 on);
s32 PalCtl_IsOn(void);
void PalCtl_Reset(void);

/* The translation at one VBlank. now is the VBlank count; the context is the store's (the count is looked at in each call). The pad is changed in place. */
void PalCtl_Run(const PalCtlCtx *ctx, u64 now, PalCtlPad *pad);

/* main's option loop: the number of arguments taken at argv[i] (1 for --frontier-controls, which turns the preset on), 0 if none. */
s32 PalCtl_Option(s32 argc, char **argv, s32 i);

#endif /* PAL_CTL_H */
