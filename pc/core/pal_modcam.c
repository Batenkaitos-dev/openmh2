/*
 * The modern camera's game side (include/pal.h: Pal_SmoothCamera, PC_CAM_*). With the setting on, the right stick turns the
 * follow camera at a speed that follows the push and tilts it smoothly between the game's own lowest and highest views (a
 * diagonal does both). An enhancement: off by default, and with it off the camera is the game's, to the bit; with it on
 * and the stick at rest it is the game's too. With it on the game's state differs from a run without it once the stick
 * moves, so a saved session made with it loads only with it.
 *
 * The state lives in the game's own memory, in bytes of its structures that nothing uses (include/camera.h: CamCtl
 * padCamTurn and padCamTilt, CamFollow tiltFrac), so a saved state and a RAM image carry it. The view is distNo plus
 * tiltFrac / 65,536; distNo is always the nearest of the game's views, so every check the camera makes on the goal
 * (the floor, the sweeps, the held eye) is the game's own, on the goal of a view in between.
 *
 * The core's --smooth-camera is how the host asks (the host's camera row). Pal_ModcamStart runs once, after the options
 * are read and before the game starts. The two numbers come from the store (Pal_CamStickSet); the development builds'
 * --smooth-camera-test feeds it from a schedule.
 */
#include "pal_core.h"

#include "pal_modcam.h"
#include "pal.h"
#include "camera.h"
#include "player.h"
#include "pad.h"
#include "host.h"

static s32 sOption;   /* --smooth-camera */
static s32 sOn;       /* in force */
static s32 sTurn;     /* the store */
static s32 sTilt;


static s32 StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}


s32 Pal_ModcamOption(s32 argc, char **argv, s32 i)
{
    if (StrEq(argv[i], "--smooth-camera")) {
        sOption = 1;
        return 1;
    }
    (void)argc;
    return 0;
}

void Pal_ModcamStart(void)
{
    sOn = sOption != 0;
    if (sOn) {
        Host_Log("smooth camera: on (the right stick turns and tilts the follow camera; an enhancement: it changes the game's state)");
    }
}

s32 Pal_SmoothCamera(void)
{
    return sOn;
}

void Pal_CamStickSet(s32 turn, s32 tilt)
{
    sTurn = turn < -127 ? -127 : turn > 127 ? 127 : turn;
    sTilt = tilt < -127 ? -127 : tilt > 127 ? 127 : tilt;
}

/* The VBlank's input step (the host's pad record, or the verification layer's feed): the numbers go to the store and to the
 * session file's camera lines. With the setting off nothing is stored or written. */
void Pal_CamStickInput(s32 turn, s32 tilt)
{
    if (!sOn) {
        return;
    }
    Pal_CamStickSet(turn, tilt);
}


/* ---- the arithmetic ---- */

s32 Modcam_TiltAdvance(s32 distNo, s32 frac, s32 stick, s32 sign, s32 *outFrac)
{
    s32 t = distNo * 65536 + frac + sign * stick * 128;
    s32 view;
    if (t < 0) {
        t = 0;
    }
    if (t > 3 * 65536) {
        t = 3 * 65536;
    }
    view = (t + 32768) >> 16;
    *outFrac = t - view * 65536;
    return view;
}

s32 Modcam_TurnStep(s32 step, s32 stick)
{
    s32 size = 23 * (stick < 0 ? -stick : stick);
    return step < 0 ? -size : size;
}

f32 Modcam_Blend(f32 a, f32 b, s32 k)
{
    return a + (b - a) * ((f32)k * 0.001953125f);   /* k / 512, exact in a float */
}

/* ---- the seven points (include/pal.h: PC_CAM_*) ---- */

/* Point 1, CamCtl_ReadPad: the stick as the camera reads it: the store's two numbers when the pad is allowed, else 0. */
void Pal_CamLatch(struct CamCtl *ctl, s32 allowed)
{
    if (!sOn) {
        return;
    }
    if (!allowed) {
        CAM_PAD_TURN(ctl) = 0;
        CAM_PAD_TILT(ctl) = 0;
        return;
    }
    CAM_PAD_TURN(ctl) = (s8)sTurn;
    CAM_PAD_TILT(ctl) = (s8)sTilt;
}

/*
 * Point 2, CamFollow_GetKeys, where the camera has the whole pad. The turn: a push with the D-pad's LEFT and RIGHT not held
 * and L1 and R1 not newly pressed is the D-pad's key for the camera. The tilt: a new press of UP or DOWN puts the view
 * on the nearest view (the D-pad then steps as in the game); else a push moves the view.
 */

static void KeysBody(struct CamCtl *ctl, struct CamFollow *work, s32 camControl);

void Pal_CamKeys(struct CamCtl *ctl, struct CamFollow *work, s32 camControl)
{
    KeysBody(ctl, work, camControl);
}

static void KeysBody(struct CamCtl *ctl, struct CamFollow *work, s32 camControl)
{
    s32 stick;
    s32 frac;
    if (!sOn) {
        return;
    }
    stick = CAM_PAD_TURN(ctl);
    if (stick != 0 && (ctl->padHeld & (PAD_LEFT | PAD_RIGHT)) == 0 && (ctl->padPressed & (PAD_L1 | PAD_R1)) == 0) {
        work->held |= stick > 0 ? PAD_RIGHT : PAD_LEFT;
    }
    if (ctl->padPressed & (PAD_UP | PAD_DOWN)) {
        CAM_TILT_FRAC(work) = 0;
        return;
    }
    stick = CAM_PAD_TILT(ctl);
    if (stick != 0 && work->blockedTimer == 0) {
        work->distNo = (u8)Modcam_TiltAdvance(work->distNo, CAM_TILT_FRAC(work), stick, camControl == 0 ? 1 : -1, &frac);
        CAM_TILT_FRAC(work) = (s16)frac;
        work->unk76 = 8;
    }
}

/* Point 3, CamFollow_Update's turn: the step that follows the push (its sign is the game's: REVERSE 2 reverses the stick). */
s32 Pal_CamTurnStep(struct CamCtl *ctl, s32 step)
{
    if (sOn && CAM_PAD_TURN(ctl) != 0 && (ctl->padHeld & (PAD_LEFT | PAD_RIGHT)) == 0) {
        s32 out = Modcam_TurnStep(step, CAM_PAD_TURN(ctl));
        return out;
    }
    return step;
}

/*
 * Point 4, CamFollow_Update, the end of the eye's offset: the view in between. The orbit camera's end has just chosen a
 * view (tiltFrac 0). Then, for the views the pad steps through, the eye's height, the eye's distance and the look point's
 * height are the nearest view's and the next one's blended by the fraction (a 512th each step). work->dist stays the nearest
 * view's: the floor gap and the two distance limits are its.
 */
void Pal_CamBlend(struct CamCtl *ctl, struct CamFollow *work, struct Player *pl, u16 distKind, f32 *offY, f32 *offZ)
{
    s32 frac;
    s32 k;
    s32 other;
    CamDist *a;
    CamDist *b;
    if (!sOn) {
        return;
    }
    if (ctl->prevAreaMode == CAM_MODE_ORBIT && work->unk78 == 0) {
        CAM_TILT_FRAC(work) = 0;
    }
    frac = CAM_TILT_FRAC(work);
    k = (frac < 0 ? -frac : frac) >> 7;
    if (distKind > CAM_DIST_CROUCH || k == 0) {
        return;
    }
    other = (s32)work->distNo + (frac > 0 ? 1 : -1);
    if (other < 0 || other > CAM_DIST_MAX) {
        return;
    }
    if (other == CAM_DIST_DEFAULT && pl->action == 1) {
        other = CAM_DIST_CROUCH;
    }
    a = work->dist;
    b = &work->set->dist[other];
    *offY = Modcam_Blend(a->eyeHeight, b->eyeHeight, k);
    *offZ = Modcam_Blend(a->eyeBack, b->eyeBack, k);
    work->atGoal[1] = pl->pos[1] + Modcam_Blend(a->atY, b->atY, k);
}

/* Points 5 to 7, where the game sets distNo itself: the view is the game's. */
void Pal_CamTiltReset(struct CamFollow *work)
{
    if (sOn) {
        CAM_TILT_FRAC(work) = 0;
    }
}
