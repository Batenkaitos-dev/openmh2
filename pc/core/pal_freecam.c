/*
 * The freecam's core side (include/pal.h: PC_FREECAM_FIELD, PC_FREECAM_BEGIN, PC_FREECAM_END).
 *
 * The game publishes its camera once a frame (Camera_Update, in the update) in a few objects and the draw reads them;
 * the freecam is a view that replaces those objects for the length of Frame_Draw and puts back what it replaced. The
 * game's update never sees it: the pad's direction, the sound's listener, the order of the transparent things and the
 * things placed around the camera keep the game's camera. What the draw reads follows the freecam: the geometry's view
 * and projection, the culling of the ground's tiles, the billboards, the sky's bands, the shadows and the lights.
 *
 * The pose is the host's (Pal_FreecamSetPose, from the IPC's mailbox block). It is composed with the game's camera
 * each frame (its roll and its fov when the pose names none) and turned into the same objects Camera_Update makes:
 * Mtx_LookAt's view, its inverse, the yaw matrix. The draw writes the projection itself (Camera_SetFov and
 * Camera_RestoreFov around the area's effects), so the end of the draw puts back an object only if it still holds the
 * value the beginning put there; what the draw wrote meanwhile stays, as the game's own update would see it.
 *
 * The layer rule: this file takes game memory only from the PalFreecamObjs the game hands over. It calls the game's
 * pure matrix functions on its own buffers and, through the pointer handed over, the game's own projection routine.
 *
 * The freecam is on only for a field scene's draw (the scene brackets its Frame_Draw with Pal_FreecamField): the menus,
 * the equipment preview and the results draw with cameras of their own.
 */
#include "pal_core.h"

#include <string.h>

#include "pal.h"
#include "pal_freecam.h"
#include "host.h"
#include "engine_math.h"
#include "vec_math.h"
#include "camera.h"
#include "kernel/state.h"
#include "kernel/kern.h"

#define SWAP_MAX 24
#define SWAP_BYTES 96

typedef struct Swap {
    void *p;
    u32 n;
    u8 saved[SWAP_BYTES];   /* what the object held before */
    u8 inst[SWAP_BYTES];    /* what the beginning put there */
} Swap;

static PalFreecamPose sPose;     /* the host's */
static s32 sField;               /* the draw is a field scene's */
static s32 sSwapped;             /* the beginning replaced something, the end puts it back */
static s32 sActive;
static u32 sFieldStep = 0xFFFFFFFFu;   /* the game step of the last field draw (Pal_FreezeStep's count) */
static u32 sStep;                /* the game steps Pal_FreezeStep has seen */
static u32 sHide;                /* the stages this field draw hides (bit n: PAL_STAGE_n) */
static Swap sSw[SWAP_MAX];
static s32 sNSw;
static s32 sHaveGame;
static f32 sGameEye[3], sGameAt[3], sGameFov;


void Pal_FreecamSetPose(const PalFreecamPose *pose)
{
    sPose = *pose;
}

s32 Pal_FreecamGameCamera(f32 eye[3], f32 at[3], f32 *fov)
{
    s32 i;
    if (!sHaveGame) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        eye[i] = sGameEye[i];
        at[i] = sGameAt[i];
    }
    *fov = sGameFov;
    return 1;
}

s32 Pal_FreecamActive(void)
{
    return sActive;
}

void Pal_FreecamField(s32 on)
{
    sField = on;
}

/* The scene's own stages: the fades and the loading letters stay in every picture. */
#define HIDE_SCENE ((1u << PAL_STAGE_FADE_B) | (1u << PAL_STAGE_FADE_A) | (1u << PAL_STAGE_LOADING))


s32 Pal_FreecamHide(s32 stage)
{
    u32 m = sHide;
    return (m >> stage) & 1;
}

/* Puts VAL into the object, remembering what it held. */
static void SwapIn(void *p, const void *val, u32 n)
{
    Swap *w = &sSw[sNSw++];
    w->p = p;
    w->n = n;
    memcpy(w->saved, p, n);
    memcpy(w->inst, val, n);
    memcpy(p, val, n);
}

/* The object now holds a value a call made: BEFORE is what it held before the call. */
static void SwapNote(void *p, u32 n, const void *before)
{
    Swap *w = &sSw[sNSw++];
    w->p = p;
    w->n = n;
    memcpy(w->saved, before, n);
    memcpy(w->inst, p, n);
}

static f32 Len3(const f32 *v)
{
    return Math_Sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}


/*
 * Decides whether this draw is the freecam's and, if it is, the eye, the target, the roll and the fov, and the form:
 * 2 the product's (every object the draw reads), 1 the test's view matrix alone.
 */
static s32 Pose(const PalFreecamObjs *o, f32 eye[3], f32 at[3], f32 *roll, f32 *fov, s32 *form, u32 *hide)
{
    f32 sy, cy, sp, cp;
    s32 i;
    for (i = 0; i < 3; i++) {
        eye[i] = o->eye[i];
        at[i] = o->at[i];
    }
    *roll = *o->roll;
    *fov = *o->fov;
    *form = 2;
    *hide = 0;
    if (!sPose.on) {
        return 0;
    }
    Math_GetSinCos(&sy, &cy, sPose.yaw);
    Math_GetSinCos(&sp, &cp, sPose.pitch);
    for (i = 0; i < 3; i++) {
        eye[i] = sPose.eye[i];
    }
    at[0] = eye[0] + sy * cp;
    at[1] = eye[1] + sp;
    at[2] = eye[2] + cy * cp;
    *roll = sPose.roll;
    *hide = sPose.hide & ~HIDE_SCENE;
    if (sPose.fov > 0.0f) {
        *fov = sPose.fov;
    }
    return 1;
}

void Pal_FreecamBegin(const PalFreecamObjs *o)
{
    f32 eye[3], at[3], up[3], roll, fov, ang;
    f32 view[16] __attribute__((aligned(16)));
    f32 inv[16] __attribute__((aligned(16)));
    f32 yawm[16] __attribute__((aligned(16)));
    s32 form, i, pr;
    u32 hide;
    sActive = 0;
    sHide = 0;
    if (!sField) {
        return;
    }
    for (i = 0; i < 3; i++) {
        sGameEye[i] = o->eye[i];
        sGameAt[i] = o->at[i];
    }
    sGameFov = *o->fov;
    sHaveGame = 1;
    sFieldStep = sStep;
    pr = Pose(o, eye, at, &roll, &fov, &form, &hide);
    if (!pr) {
        return;
    }
    sHide = hide;
    if (pr == 2) {   /* the test's hide-only segment */
        return;
    }
    {
        f32 d[3];
        for (i = 0; i < 3; i++) {
            d[i] = at[i] - eye[i];
        }
        if (Len3(d) < 1e-3f) {
            return;
        }
    }
    Camera_ComputeUp(up, eye, at, roll);
    Mtx_LookAt(view, eye, at, up);
    sNSw = 0;
    sSwapped = 1;
    sActive = 1;
    SwapIn(o->view, view, 64);
    if (form == 1) {
        return;
    }
    Mtx_Inverse(inv, view);
    ang = 6.2831855f * (s32)(u16)(Mtx_Heading(inv) + 65536 / 4) / 65536.0f;
    Mtx_Identity(yawm);
    Mtx_SetRotXYZ(yawm, 0.0f, ang, 0.0f);
    SwapIn(o->viewCopy, view, 64);
    SwapIn(o->camMatrix, inv, 64);
    SwapIn(o->camYaw, yawm, 64);
    SwapIn(o->eye, eye, 12);
    SwapIn(o->at, at, 12);
    SwapIn(o->up, up, 12);
    if (memcmp(&fov, o->fov, 4) != 0) {
        u8 before[5][SWAP_BYTES];
        for (i = 0; i < 5; i++) {
            memcpy(before[i], o->proj[i].p, o->proj[i].bytes);
        }
        SwapIn(o->fov, &fov, 4);
        SwapIn(o->fovPrev, &fov, 4);
        o->applyProjection();
        for (i = 0; i < 5; i++) {
            SwapNote(o->proj[i].p, o->proj[i].bytes, before[i]);
        }
    }
}

void Pal_FreecamEnd(void)
{
    s32 i;
    sHide = 0;
    if (!sSwapped) {
        return;
    }
    sSwapped = 0;
    for (i = sNSw - 1; i >= 0; i--) {
        Swap *w = &sSw[i];
        if (!memcmp(w->p, w->inst, w->n)) {
            memcpy(w->p, w->saved, w->n);   /* what the draw wrote meanwhile stays */
        }
    }
    sNSw = 0;
}

/*
 * The frozen frame. Frozen, a step of the game's main loop is only the last field frame's draw again, with the freecam's
 * pose of the moment, and the game's own flip: no pad read, no update, none of the frame's lists made again. The flip
 * waits for the VBlank, so a frozen frame is a VBlank: the VBlank-driven state (the VSync counter, the timers, the sound
 * and the IOP) goes on, and only the game's tasks stand still.
 */
extern s32 Mc_RequestPending(void);   /* api/libmc.c: a memory card request is in flight (not yet synced) */

static s32 sHold;                /* the host's request */
static u32 sGrant, sTaken;       /* real frames the host granted, and the core ran, while frozen */
static s32 sFrozen;
static s32 sWasFrozen;           /* the step before was a frozen frame */
static s32 sWhy;
static u64 sStartVb;             /* the VBlank the freeze began at */



void Pal_FreezeSet(s32 hold, u32 grant)
{
    sHold = hold != 0;
    sGrant = grant;
}

s32 Pal_FreezeState(s32 *why, u32 *taken)
{
    if (why) {
        *why = sWhy;
    }
    if (taken) {
        *taken = sTaken;
    }
    return sFrozen ? PAL_FREEZE_FROZEN : sHold ? PAL_FREEZE_WAITING : PAL_FREEZE_RUNNING;
}

/* A real step: 2 when it follows frozen frames (the flip timing of the game's frames is put back), else 0. */
static s32 Real(void)
{
    s32 r = sWasFrozen ? 2 : 0;
    sWasFrozen = 0;
    return r;
}

/* Why a freeze cannot take hold at this step (0: it can). */
static s32 Why(s32 field)
{
    if (!field) {   /* the step before was not a field scene's draw: a frozen frame has nothing to draw again */
        return PAL_FREEZE_WHY_SCENE;
    }
    if (Kern_StateBlocked()) {
        return PAL_FREEZE_WHY_MOVIE;
    }
    if (Mc_RequestPending()) {
        return PAL_FREEZE_WHY_CARD;
    }
    return PAL_FREEZE_WHY_NONE;
}


s32 Pal_FreezeStep(void)
{
    s32 hold = sHold;
    u32 cur = ++sStep;
    u64 vb = Kern_VblankCount();
    s32 field = sFieldStep == cur - 1;   /* the step before drew a field scene's frame */
    s32 why;
    if (sFrozen && !hold && ((vb - sStartVb) & 1)) {
        hold = 1;   /* the game goes on at a VBlank of the parity it stopped at: its frames keep their fields */
    }
    if (!hold) {
        if (sFrozen) {
            sFrozen = 0;
        }
        sWhy = 0;
        return Real();
    }
    if (!sFrozen) {
        why = Why(field);
        if (why) {
            sWhy = why;
            return Real();
        }
        sFrozen = 1;
        sWhy = 0;
        sStartVb = vb;
        sTaken = sGrant;   /* grants made before the freeze are not used */
    } else if (!field) {   /* a real step of the freeze's was not a field scene's frame: the freeze ends */
        sFrozen = 0;
        sWhy = PAL_FREEZE_WHY_SCENE;
        return Real();
    }
    if (sGrant != sTaken) {   /* a step: one real frame, then frozen again */
        sTaken++;
        return Real();
    }
    sWasFrozen = 1;
    return 1;
}
