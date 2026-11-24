#ifndef PAL_MODCAM_H
#define PAL_MODCAM_H
/* The modern camera's setting and store (pal_modcam.c; include/pal.h has the game's side, PC_CAM_*). */
#include "pal_core.h"

/*
 * main's option loop: the number of arguments taken at argv[i]: 1 for --smooth-camera, 2 for the development builds'
 * --smooth-camera-test V:TURN:TILT[,V:TURN:TILT...] (a test schedule of the stick: from VBlank V on, the two numbers) and
 * --smooth-camera-drop-frac (1 argument: a loaded state loses the tilt's fraction) and --smooth-camera-trace (1 argument: a log
 * line for each camera update the stick or a view in between touches), else 0.
 */
s32 Pal_ModcamOption(s32 argc, char **argv, s32 i);

/* After the options are read, before the game starts: the setting in force. */
void Pal_ModcamStart(void);

/*
 * The store: the two numbers the platform hands the camera, each -127 to 127 (turn: positive turns the view right; tilt:
 * positive looks up). The core's input step writes them (the host's stick comes later); the
 * camera reads them when it latches the pad. Values beyond the range are clamped.
 */
void Pal_CamStickSet(s32 turn, s32 tilt);

/*
 * The VBlank's input step hands the stick here: the host's pad record (the ipc glue), or the verification layer's feed. With
 * the setting on it is stored (Pal_CamStickSet) and written to the session file; with it off
 * nothing happens.
 */
void Pal_CamStickInput(s32 turn, s32 tilt);


/*
 * The arithmetic the game's functions use, with no game state, so that a test can drive it.
 *  - Modcam_TiltAdvance: the view's place is distNo + frac / 65,536 (frac -32,768 to 32,767: half a view each way). It moves
 *    by sign * stick * 128, is held to 0 .. 3 views, and is given back as the nearest view (the return) and the rest (*outFrac).
 *  - Modcam_TurnStep: the turn's step (a signed 1,150 for the D-pad) made 23 times the stick's size, with its sign.
 *  - Modcam_Blend: a + (b - a) * (k / 512), k 1 to 256.
 */
s32 Modcam_TiltAdvance(s32 distNo, s32 frac, s32 stick, s32 sign, s32 *outFrac);
s32 Modcam_TurnStep(s32 step, s32 stick);
f32 Modcam_Blend(f32 a, f32 b, s32 k);

#endif
