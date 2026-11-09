/*
 * The core's glue between the host (the IPC, core_ipc.h) and the kernel and
 * io modules:
 *  - Kern_InitIpc, the kernel's start-up hook: attaches to the host (Ipc_Init;
 *    headless when MH2_IPC is unset), points io's Io_PadRumble at the host's
 *    motors, and sets the VBlank's steps 2 (KERN_VB_PACE) and 7
 *    (KERN_VB_INPUT), and the VBlank end's KERN_VB_END.
 *  - At each VBlank's start, step 2: real-time pacing (Ipc_Pace: it only
 *    waits, and only when the host asks for it); step 7,
 *    before io's Io_PadVBlank: the pad from the host handed to io
 *    (Io_PadSetState), which then writes it to the game's buffers.
 *    Only port 0 is handed over (see below).
 *  - The glue keeps no state of its own: the pad and the key events are
 *    read from the host at each step and handed on.
 *  - The same step 7 hands the host's key events to io's keyboard report
 *    (Io_KbVBlank), when a USB keyboard is connected (see below).
 *  - The movies' step first at the VBlank's end (KERN_VB_END: ipc_movie.c's
 *    Movie_VBlankEnd), headless too.
 *
 * The pads as the original game has them (the original's
 * platform_read_pad): port 0 is always connected (headless, or before the
 * host has written its first state: no buttons, the sticks centred, no
 * pressures); port 1 is never connected, so it is not handed over.
 *
 * The keyboard: by default the original game has none (its USB keyboard service answers
 * zeros, so sceUsbKbInit gives -1), and io's service does the same. The host's
 * keyboard as a pad (Ipc_ReadKeyboard) is therefore not handed to the game: doing so
 * would change what the game computes. A USB keyboard is connected only when the
 * host asks for one (MH2IPC_HOST_USB_KEYBOARD, Io_KbHostWants); then the host's key events
 * (Ipc_KeyPeek) make the report of each VBlank (Io_KbVBlank, with the pads).
 * Otherwise the game sees no keyboard at all. The events a report used are taken
 * from the host's queue (Ipc_KeyTake).
 *
 * Platform-layer code (the game's flags): it touches no game memory.
 */
#include "common.h"
#include <string.h>
#include "io/io.h"
#include "io/io_pad.h"
#include "ipc/core_ipc.h"
#include "kernel/kern.h"
#include "kernel/state.h"
#include "pal.h"
#include "pal_freecam.h"
#include "ipc/mh2ipc.h"

static void Rumble(int port, u8 small, u8 large)
{
    Ipc_SetRumble((u32)port, small, large);
}

static void PadFromHost(void)
{
    IpcPad in;
    IoPadState st;
    u32 i;
    Ipc_ReadPad(0, &in);
    st.connected = 1;
    if (in.connected) {
        st.buttons = in.buttons;
        st.rx = in.rx;
        st.ry = in.ry;
        st.lx = in.lx;
        st.ly = in.ly;
        for (i = 0; i < 12; i++)
            st.pressure[i] = in.pressure[i];
    } else {
        st.buttons = 0;
        st.rx = st.ry = st.lx = st.ly = 0x80;
        for (i = 0; i < 12; i++)
            st.pressure[i] = 0;
    }
    Io_PadSetState(0, &st);
}


static s32 KeyPeek(u32 *event)
{
    return Ipc_KeyPeek((unsigned int *)event);
}

static void KeyTake(void)
{
    Ipc_KeyTake();
}

static s32 KeyboardWanted(void)
{
    return Ipc_KeyboardWanted();
}


/* ipc_movie.c: the movie's step, first at each VBlank's end (KERN_VB_END). */
void Movie_VBlankEnd(u64 vblank);

/* The game's loading screen opens and closes (Loading_Open, Loading_Close: PC_LOADING_SCREEN in include/pal.h): fast loading's signal. */
void Pal_LoadingScreen(s32 on)
{
    Ipc_LoadingScreen(on);
}

/* The camera of Model_Draw (PC_GS_CAMERA in include/pal.h): the host's renderer gets it when it changes. */
void Pal_GsCamera(const f32 *view, const f32 *proj)
{
    static float last[32];
    static int have;
    float m[32];
    memcpy(m, view, 16 * sizeof(float));
    memcpy(m + 16, proj, 16 * sizeof(float));
    if (have && !memcmp(last, m, sizeof(m)))
        return;
    memcpy(last, m, sizeof(m));
    have = 1;
    Ipc_GsCamera(m);
}

static void VblankPace(u64 n)
{
    Ipc_PaceBias(Pal_FreezeBias());
    Ipc_PaceMovie(Kern_StateBlocked());   /* a movie is open: it is never sped up */
    Ipc_Pace(n);
}

/* The freecam's mailbox (Mh2IpcFreecam): the host's pose to the seam before this VBlank's frame draws, and the game's camera of the
 * last draw back to the host. When the pose is refused is pal_freecam.c's rule. */
/* The host's HUD mask (MH2IPC_FC_HUD) is every stage but the scene's own: the two fades and the loading letters. */
_Static_assert(MH2IPC_FC_HUD == (((1u << PAL_STAGE_COUNT) - 1u) & ~((1u << PAL_STAGE_FADE_B) | (1u << PAL_STAGE_FADE_A) | (1u << PAL_STAGE_LOADING))),
               "MH2IPC_FC_HUD names the stages of include/pal.h");
_Static_assert(PAL_STAGE_COUNT <= MH2IPC_FC_HIDE_BITS, "the block's hide mask holds the stages");

static unsigned int sInForce KERN_HOSTBSS;   /* the sequence of the host's pose that the draws since the last poll are made with */
/* The pause's step count. The host counts the steps it grants from its own start; the core counts those it has taken in Pal_FreezeStep's
 * statics, which a save state holds (they come back with a load). The core is handed the host's count plus sOff, so that the two
 * stand apart by a fixed amount; a load puts the core's count back and the next poll finds the new sOff, so that no step the host
 * granted before the load can run after it (no burst), and none is lost that was asked after it. The words are the host's, not the
 * game's: a state does not hold them. */
static unsigned int sOff KERN_HOSTBSS;
static int sRebase KERN_HOSTBSS;

/* What the host is told of the draws: the pose in force, and whether the last draw was a field scene's with the freecam's view in it.
 * A game frame spans two VBlanks or more, so the poll that follows the freecam's turning on may come before any draw made with its
 * pose, and the last draw's answer (a menu's, from before) would be taken for the pose's. Until the first draw with the pose the host
 * is told nothing new (its sequence is behind the one it waits for): a field draw made since the poll that took the pose
 * (Pal_FreecamDraws moved) answers; and two game steps begun since with no field draw say that the step between them was not a field
 * scene's (a step draws at most once, after it begins), and the host is told that at once. After the first answer the last draw's is
 * told at each poll. */
static unsigned int sEcho KERN_HOSTBSS;     /* the pose sequence of the last answer */
static int sSaid KERN_HOSTBSS;              /* ...and its "field scene with the pose" */
static unsigned int sMarkDraws KERN_HOSTBSS;
static unsigned int sMarkSteps KERN_HOSTBSS;
static int sFirst KERN_HOSTBSS;             /* the freecam was turned on and no answer has come for it yet */
static int sWasOn KERN_HOSTBSS;

static void FreecamPoll(void)
{
    IpcFreecam fc;
    f32 eye[3], at[3], fov;
    int have = Ipc_FreecamRead(&fc);
    s32 why = 0;
    u32 taken = 0;
    s32 state = Pal_FreezeState(&why, &taken);
    u32 draws = Pal_FreecamDraws(), steps = Pal_FreecamSteps();
    if (have && sRebase) {
        sRebase = 0;
        sOff = taken - fc.grant;
        sMarkDraws = draws;   /* a load puts the game's step count back: count again from here */
        sMarkSteps = steps;
    }
    if (!sFirst) {
        sEcho = sInForce;
        sSaid = Pal_FreecamActive();
    } else if (draws != sMarkDraws) {
        sFirst = 0;
        sEcho = sInForce;
        sSaid = Pal_FreecamActive();
    } else if (steps - sMarkSteps >= 2) {
        sFirst = 0;
        sEcho = sInForce;
        sSaid = 0;
    }
    Ipc_FreecamStatus(sSaid, sEcho, Kern_VblankCount() + Pal_FreezeBias(), state, why, taken - sOff);
    if (have) {
        PalFreecamPose pose;
        pose.on = fc.on;
        pose.eye[0] = fc.eye[0];
        pose.eye[1] = fc.eye[1];
        pose.eye[2] = fc.eye[2];
        pose.yaw = fc.yaw;
        pose.pitch = fc.pitch;
        pose.roll = fc.roll;
        pose.fov = fc.fov;
        pose.hide = fc.hide;   /* the HUD's stages together, when the host asks (photo mode's H) */
        Pal_FreecamSetPose(&pose);
        if (fc.on && !sWasOn) {   /* turned on: the counts start from this poll */
            sFirst = 1;
            sMarkDraws = draws;
            sMarkSteps = steps;
        }
        if (!fc.on) {
            sFirst = 0;
        }
        sWasOn = fc.on;
        sInForce = fc.seq;
        Pal_FreezeSet(fc.frozen, fc.grant + sOff);
    }
    if (Pal_FreecamGameCamera(eye, at, &fov)) {
        Ipc_FreecamCamera(eye, at, fov);
    }
}

static void VblankInput(u64 n)
{
    (void)n;
    FreecamPoll();
    PadFromHost();
    Io_KbVBlank(KeyPeek, KeyTake);
}

/* Save states (kernel/state.c): the renderer's part of a state is in the host's process, so it crosses the IPC (the
 * block's window: pc/ipc/mh2ipc.h, Mh2IpcState); the host side is put in step after a load. */
static const char *sStateWhy KERN_HOSTBSS;

static const void *StateGet(u32 *size)
{
    return Ipc_StateGsGet(size, &sStateWhy);
}

static int StateSet(const void *data, u32 size)
{
    return Ipc_StateGsSet(data, size);
}

static const char *StateIdentity(void)
{
    return Ipc_StateIdentity();
}

static const char *StateError(void)
{
    return sStateWhy;
}

static const KStateHost sStateHost = {StateGet, StateSet, StateIdentity, StateError};

static void StateResume(void)
{
    sRebase = 1;
    Ipc_LoadingScreen(0);   /* the loaded game's screen is open or not; the pacer waits for the next Loading_Open */
    Ipc_StateLoaded(Kern_VblankCount() + 1);   /* the game goes on by starting this VBlank */
}

/* The section has no bytes: the owner is here for its resume. */
static const KStateOwner sStateOwner = {"IPC", 0, 0, 0, StateResume, 0, 0, 0};

void Kern_InitIpc(void)
{
    Ipc_Init();
    if (Ipc_StateHasGs()) {
        Kern_StateSetHost(&sStateHost);
    }
    Kern_StateRegister(&sStateOwner);
    Io_PadRumble = Rumble;
    Io_KbHostWants = KeyboardWanted;
    Kern_SetVblankHook(KERN_VB_PACE, VblankPace);
    Kern_SetVblankHook(KERN_VB_INPUT, VblankInput);
    Kern_SetVblankHook(KERN_VB_END, Movie_VBlankEnd);
}
