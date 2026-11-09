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

static PalFreecamPose sPose KERN_HOSTBSS;   /* the host's: the pause's restores do not take it back; a pose moved while paused would be put back with the pages and never reach a frozen frame (shown: with it in the snapshot the paused picture stays at the pose of the pause, with it here the picture is that of a pose set before the pause, pixel for pixel) */
static s32 sField;               /* the draw is a field scene's */
static s32 sSwapped;             /* the beginning replaced something, the end puts it back */
static s32 sActive;
static u32 sFieldStep = 0xFFFFFFFFu;   /* the game step of the last field draw (Pal_FreezeStep's count) */
static u32 sStep;                /* the game steps Pal_FreezeStep has seen */
static u32 sHide;                /* the stages this field draw hides (bit n: PAL_STAGE_n) */
static u32 sDraws KERN_HOSTBSS;  /* the field draws so far (Pal_FreecamDraws): the host's count, a restore does not take it back */
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

u32 Pal_FreecamDraws(void)
{
    return sDraws;
}

u32 Pal_FreecamSteps(void)
{
    return sStep;
}

void Pal_FreecamField(s32 on)
{
    sField = on;
}

/* The scene's own stages: the fades and the loading letters stay in every picture. */
#define HIDE_SCENE ((1u << PAL_STAGE_FADE_B) | (1u << PAL_STAGE_FADE_A) | (1u << PAL_STAGE_LOADING))


void Pal_FreecamStage(s32 stage)
{
    u32 m = sHide;
    Pal_HudHide(stage >= 0 && stage < PAL_STAGE_COUNT ? (s32)((m >> stage) & 1) : 0);
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
    sDraws++;
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
    Pal_HudHide(0);
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
 * The pause: a state loop.
 *
 * Paused, the game does not stand still: it runs the same step again and again. At a step's start (Pal_FreezeStep, the first
 * thing a step of the game's main loop does) the kernel keeps a snapshot of the whole writable image (kernel/state.c,
 * Kern_StateMemSave: the pages, the guest clock, the fibers, the libraries' and the sound engine's state) and of the renderer;
 * at the next step's start it puts the pages back and goes on in the context that took it, so the step runs again, with the
 * pose of the moment, the game's own draw and the game's own flip. When the pause ends the snapshot is put back for the last
 * time, the renderer's block with it, and the step runs for real: the game goes on as one that was never paused, and no
 * VBlank of the loop's passed. The VBlanks that did pass are counted into an outward bias (Pal_FreezeBias), which the pacer, the
 * scanout's record and the host's status add, so that the host sees VBlanks that go on.
 *
 * The words of the loop are the process's (KERN_HOSTBSS): a restore does not touch them.
 */
extern s32 Mc_RequestPending(void);   /* api/libmc.c: a memory card request is in flight (not yet synced) */
extern s32 gSysCurBuf;                /* src/main/display.c: the frame buffer shown; the flip at a VBlank toggles it */

static s32 sHold KERN_HOSTBSS;                /* the host's request */
static u32 sGrant KERN_HOSTBSS, sTaken KERN_HOSTBSS;   /* steps the host granted, and the core ran, while paused */
static s32 sLoop KERN_HOSTBSS;                /* a snapshot is held and the step is run again from it */
static s32 sWhy KERN_HOSTBSS;
static u64 sBias KERN_HOSTBSS;                /* the VBlanks the restores have put back */
static u64 sBaseVb KERN_HOSTBSS;              /* the VBlank count when the snapshot was taken */
static s32 sAgain KERN_HOSTBSS;               /* the step is being put back to run again (not to run for real after the pause) */
static u32 sRuns KERN_HOSTBSS;                /* the times the step has been put back since the snapshot: the parity of the frame buffers */
static u64 sHigh KERN_HOSTBSS;                /* the highest VBlank the loop passed before a restore: the development layer's step has seen up to it */



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
    return sLoop ? PAL_FREEZE_FROZEN : sHold ? PAL_FREEZE_WAITING : PAL_FREEZE_RUNNING;
}

u64 Pal_FreezeBias(void)
{
    return sBias;
}

/* The development layer's step of a VBlank runs once for each VBlank number: the loop passes the same numbers again, and the
 * last pass, the real one, repeats what the first one saw. */
s32 Pal_FreezeSkipVfy(u64 n)
{
    return sHigh != 0 && n <= sHigh;
}

void Pal_FreezePadAgain(u64 n)
{
    (void)n;
}

/* A state file was loaded, or the run starts over: the snapshot belongs to a game that is gone. */
void Pal_FreezeCancel(void)
{
    sLoop = 0;
    sHigh = 0;
    sWhy = 0;
    Kern_StateMemDrop();
}

/* Why a pause cannot take hold at this step (0: it can). */
static s32 Why(s32 field)
{
    if (!field) {   /* the step before was not a field scene's draw: a loop of this step would draw no field scene */
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


/* The first thing a step of the game's main loop does. Returns 0, and does not return at all when the pause puts the
 * snapshot back: the call that took it returns again, in the restored context, and the step goes on from there. */
s32 Pal_FreezeStep(void)
{
    s32 hold = sHold;
    u32 cur = ++sStep;
    u64 vb = Kern_VblankCount();
    s32 field = sFieldStep == cur - 1;   /* the step before drew a field scene's frame */
    s32 why = 0, r;
    if (sLoop) {   /* the step has run since the snapshot */
        s32 end = !hold;
        if (!end) {
            why = Why(field);
            end = why != 0;
        }
        if (end) {   /* the pause is over: the game as it was at the snapshot, the renderer too, and the step runs for real */
            sLoop = 0;
            sWhy = why;
            if (vb > sHigh) {
                sHigh = vb;
            }
            sBias += vb - sBaseVb;
            sAgain = 0;
            Kern_StateMemRestore(1);   /* does not return when it has the snapshot */
            Kern_StateMemDrop();
            return 0;
        }
        if (sGrant == sTaken) {   /* paused: the step again */
            if (vb > sHigh) {
                sHigh = vb;
            }
            sBias += vb - sBaseVb;
            sAgain = 1;
            Kern_StateMemRestore(0);
            sLoop = 0;   /* no snapshot: the pause is over */
            sWhy = PAL_FREEZE_WHY_SCENE;
            return 0;
        }
        sTaken++;   /* a step: the step that ran stays, and the loop goes on from here */
    } else {
        if (!hold) {
            sWhy = 0;
            return 0;
        }
        why = Why(field);
        if (why) {
            sWhy = why;
            return 0;
        }
        sTaken = sGrant;   /* grants made before the pause are not used */
    }
    r = Kern_StateMemSave();
    if (r == 1) {   /* the restored context: the step runs again */
        /* The chain a step sends is drawn into the buffer its flip leaves off screen, and the next flip shows it. The snapshot has
         * the buffer of its own step, so a step put back would show the picture the step before it drew, every time: the pose of
         * the moment would be drawn and never shown. Each run takes the other buffer, as one step after another does. */
        if (sAgain) {   /* (the end of the pause comes back here too, and the step then runs as it would have) */
            sAgain = 0;
            sRuns++;
            gSysCurBuf ^= (s32)(sRuns & 1);
        }
        return 0;
    }
    sRuns = 0;
    if (r < 0) {
        sLoop = 0;
        sWhy = PAL_FREEZE_WHY_SCENE;
        return 0;
    }
    sLoop = 1;
    sWhy = 0;
    sBaseVb = Kern_VblankCount();
    return 0;
}
