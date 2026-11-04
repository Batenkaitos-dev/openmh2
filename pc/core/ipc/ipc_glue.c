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
 * The pads as the original game has them (platform_sdl.c,
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
#include "io/io.h"
#include "io/io_pad.h"
#include "ipc/core_ipc.h"
#include "kernel/kern.h"

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

static void VblankPace(u64 n)
{
    Ipc_Pace(n);
}

static void VblankInput(u64 n)
{
    (void)n;
    PadFromHost();
    Io_KbVBlank(KeyPeek, KeyTake);
}

void Kern_InitIpc(void)
{
    Ipc_Init();
    Io_PadRumble = Rumble;
    Io_KbHostWants = KeyboardWanted;
    Kern_SetVblankHook(KERN_VB_PACE, VblankPace);
    Kern_SetVblankHook(KERN_VB_INPUT, VblankInput);
    Kern_SetVblankHook(KERN_VB_END, Movie_VBlankEnd);
}
