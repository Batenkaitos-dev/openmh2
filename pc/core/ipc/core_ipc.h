/*
 * mh2-core's side of the IPC with the host, mh2. The core's modules call these:
 *   gfx      the GS stream (Ipc_GsGif, Ipc_GsPrivWrite, Ipc_GsVSync) and
 *            readbacks (Ipc_GsReadback);
 *   snd      the samples (Ipc_AudioWrite);
 *   io       the input (Ipc_ReadPad, Ipc_ReadKeyboard, Ipc_SetRumble);
 *   kernel   the connection (Ipc_Init at start, Ipc_Exit), the player's
 *            quit (Ipc_QuitRequested) and the fatal message (Ipc_Fatal).
 *
 * Headless (no host: MH2_IPC unset, as in every check), each call goes
 * nowhere: the stream and the samples are dropped, a readback gives zeros,
 * the pads and the keyboard are disconnected and the player never quits.
 * What the game computes must never depend on whether a host is there:
 * the only values from the host the game may see are the input and the
 * readbacks, and nothing else crosses to the game.
 *
 * Every function uses the host's calling convention (cdecl), so both the
 * game-facing files (regcall) and the host_ files can call them. No system
 * headers here.
 */
#ifndef MH2_CORE_IPC_H
#define MH2_CORE_IPC_H

#define IPC_CALL __attribute__((cdecl))

/* ---- the connection (kernel) ---- */

/* Attaches to the host named by MH2_IPC. Returns 1 when attached, 0 when
 * headless; a block that can't be opened is fatal. Call once, first. */
IPC_CALL int Ipc_Init(void);
/* 1 when attached (for logs: never let the game's state depend on it). */
IPC_CALL int Ipc_Attached(void);
/* The core ends normally: the host shows what is still in the stream, then
 * closes. */
IPC_CALL void Ipc_Exit(int code);
/* The core ends on a fatal error: the host shows `msg` to the player. Call
 * it before aborting (Host_Fatal). */
IPC_CALL void Ipc_Fatal(const char *msg);
/* The player closed the window: the core should end at its next safe
 * point (the run loop, between VBlanks). Never game-visible. */
IPC_CALL int Ipc_QuitRequested(void);

/* ---- the GS stream (gfx), in the order the GS receives it ---- */

/* GIF data of PATH 1, 2 or 3: `qwords` whole 128-bit words, GIFtags
 * included, as the GIF hands them to the GS. A transfer may be split
 * anywhere between qwords. */
IPC_CALL void Ipc_GsGif(unsigned int path, const void *data, unsigned int qwords);
/* A write of a privileged GS register (0x12000000-0x120000E0,
 * 0x12001000-0x12001080). */
IPC_CALL void Ipc_GsPrivWrite(unsigned int addr, unsigned long long value);
/* The VBlank's scanout: the host shows the picture of this field
 * (0 or 1; CSR's FIELD). `vblank` counts VBlanks from the start. */
IPC_CALL void Ipc_GsVSync(unsigned int field, unsigned long long vblank);
/* A local -> host transfer (TRXDIR = 1): `qwords` 128-bit words of what
 * the GS reads out, into dst, after everything sent before. Returns 1 when
 * the host's renderer gave them, 0 when they are zeros (headless, or a
 * host without a renderer). The caller decides what the game sees (zeros
 * in every check 3.5) and records it. */
IPC_CALL int Ipc_GsReadback(void *dst, unsigned int qwords);

/* ---- the samples (snd) ---- */

#define IPC_AUDIO_RATE 48000
/* `frames` frames of 48 kHz stereo, interleaved signed 16-bit (left,
 * right). Never blocks: what doesn't fit in the host's ring is dropped. */
IPC_CALL void Ipc_AudioWrite(const short *samples, unsigned int frames);

/* ---- the input (io), the host's current state ---- */

#define IPC_PADS 2
/* Same layout as Mh2IpcPad (pc/ipc/mh2ipc.h): button bits in the pad's
 * order, 1 = pressed (0 SELECT, 1 L3, 2 R3, 3 START, 4 UP, 5 RIGHT, 6 DOWN,
 * 7 LEFT, 8 L2, 9 R2, 10 L1, 11 R1, 12 TRIANGLE, 13 CIRCLE, 14 CROSS,
 * 15 SQUARE); pressures 0-255 for RIGHT LEFT UP DOWN TRIANGLE CIRCLE CROSS
 * SQUARE L1 R1 L2 R2; sticks 0-255, 0x80 the centre, 0 up or left. */
typedef struct IpcPad {
    unsigned int connected;
    unsigned short buttons;
    unsigned char rx, ry, lx, ly;
    unsigned char pressure[12];
    unsigned char reserved[2];
} IpcPad;

/* Same layout as Mh2IpcKeyboard: the keys down as a bitmap of HID usage
 * IDs (bit (u & 31) of keys[u >> 5]), the boot report's modifier bits. */
typedef struct IpcKeyboard {
    unsigned int connected;
    unsigned int modifiers;
    unsigned int keys[8];
} IpcKeyboard;

/* The pad on `port` (0 or 1) now. Returns its `connected`; headless, a
 * disconnected pad (no buttons, sticks centred). The io module samples it
 * where the game reads the pad. */
IPC_CALL int Ipc_ReadPad(unsigned int port, IpcPad *out);
/* The keyboard now; the same rules. */
IPC_CALL int Ipc_ReadKeyboard(IpcKeyboard *out);
/* The USB keyboard (the owner's keyboard sessions; pc/ipc/mh2ipc.h, Mh2IpcKeys). 1 when the host says a USB keyboard is
 * connected (MH2IPC_HOST_USB_KEYBOARD; headless: 0). The key events wait in a ring, in the order the player made
 * them: Ipc_KeyPeek gives the next without taking it (1; 0 when none: the event is usage | 0x100 when the key went
 * down), Ipc_KeyTake takes it. Headless, there are none. io makes the boot report of a VBlank from them. */
IPC_CALL int Ipc_KeyboardWanted(void);
IPC_CALL int Ipc_KeyPeek(unsigned int *event);
IPC_CALL void Ipc_KeyTake(void);
/* The pad's motors: small 0 or 1, large 0-255. */
IPC_CALL void Ipc_SetRumble(unsigned int port, unsigned int small, unsigned int large);


/* ---- real-time pacing (the kernel's VBlank step 2, KERN_VB_PACE) ---- */

/* At the start of VBlank `vblank`: when the host asks for real-time play
 * (MH2IPC_HOST_PACE: a window, without --fast), waits until the host's
 * clock reaches the VBlank's time (n x 16,683,350 ns from the first VBlank
 * paced; after falling more than 3 VBlanks behind, the clocks are tied
 * again at this one). Otherwise, and headless, returns at once. It only
 * waits: nothing the game can observe depends on it. */
IPC_CALL void Ipc_Pace(unsigned long long vblank);

/* ---- movies (the host's player, pc/core/ipc/ipc_movie.c) ---- */

/* The movie in `size` bytes at `offset` of the host file `path` starts
 * (id: the core's count of movies, never 0); mode is the game's display
 * mode, volume its movie volume (0.1 dB, -960 silent). The host decodes
 * it. */
IPC_CALL void Ipc_MovieOpen(unsigned int id, const char *path, unsigned long long offset, unsigned int size,
                            unsigned int mode, int volume);
/* The movie plays: its sound is mixed into the samples written from now
 * on, its picture shown at the following VSyncs by Ipc_MovieClock. */
IPC_CALL void Ipc_MoviePlay(unsigned int id);
/* The movie clock for the following VSyncs: the 48 kHz frames of the
 * SPU2's output since the movie began playing (the host shows frame 0 and
 * every frame whose time is at most clock / 48000 s), or -1: the GS's
 * picture. */
IPC_CALL void Ipc_MovieClock(int clock);
/* The movie ends or is skipped: its picture and its sound stop here. */
IPC_CALL void Ipc_MovieClose(unsigned int id);

#endif
