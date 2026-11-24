/*
 * The protocol between mh2 (the 64-bit host) and mh2-core (the 32-bit game):
 * one block of shared memory, the same layout in both processes
 *.
 *
 * The host creates the block and starts mh2-core with its name in the
 * environment variable MH2_IPC. Without it, mh2-core runs headless and
 * every IPC call goes nowhere (pc/core/ipc/core_ipc.h).
 *
 * The block:
 *   the header (MH2IPC_HEADER_BYTES): the states of the two processes, the
 *     control words, the input mailbox, and the two rings' indices;
 *   the GS ring (core -> host): records in the order the core made them:
 *     GIF data, privileged register writes, VSync, readback requests and
 *     the movie commands;
 *   the sample ring (core -> host): 48 kHz stereo s16, interleaved;
 *   the readback buffer (host -> core): the data of the last readback.
 *
 * Every ring has one producer and one consumer. Indices are byte counts
 * that run freely (mod 2^32) over a power-of-two ring. A record is 16
 * bytes of Mh2IpcRec, then its payload padded to 16 bytes, and never wraps:
 * a record that wouldn't fit before the ring's end follows a PAD record
 * that fills the rest.
 *
 * No system headers here, and no pointers or host-sized types in the
 * block: both a 32-bit and a 64-bit process include this. The static
 * assertions below pin the layout.
 *
 * The functions at the end are the rings' logic, shared by both sides. The
 * waiting and waking (futexes on Linux, named events on Windows) and the
 * mapping are in mh2ipc_os.h.
 */
#ifndef MH2IPC_H
#define MH2IPC_H

typedef unsigned char mh2ipc_u8;
typedef unsigned short mh2ipc_u16;
typedef unsigned int mh2ipc_u32;
typedef int mh2ipc_s32;
typedef unsigned long long mh2ipc_u64;

#define MH2IPC_MAGIC 0x3243504Du   /* "MPC2", little-endian */
#define MH2IPC_VERSION 11u
/* The release build (MH2_RELEASE) leaves out some of the host's features, so its header lacks the development
 * build's two blocks (Mh2IpcPlay, Mh2IpcWatch) and the words that answer them. Every other field is at the same
 * offset in both: the blocks that only the development build has are in reserved space in a release header (the
 * pass's ring and the live plans follow the watch's place in both). The same version number serves both. A release core runs under a
 * development host. The header's size (header_bytes) is the same constant
 * in both, and the core refuses a smaller one than its own. */
#define MH2IPC_ENV "MH2_IPC"         /* the block's name, in mh2-core's environment */

/* The block's parts, in this order. */
#define MH2IPC_HEADER_BYTES 0x10000u
#define MH2IPC_GS_BYTES (32u << 20)        /* about ten frames of the busiest scenes */
#define MH2IPC_AUDIO_BYTES (256u << 10)    /* 65536 frames, 1.37 s */
#define MH2IPC_RB_BYTES (4u << 20)         /* the GS's whole memory */
#define MH2IPC_STATE_BYTES (20u << 20)     /* the window for the renderer's part of a save state: the local memory and 16 KB (4.2 MB at 4 MB, 8.2 at 8, 16.2 at 16) */
#define MH2IPC_TOTAL_BYTES (MH2IPC_HEADER_BYTES + MH2IPC_GS_BYTES + MH2IPC_AUDIO_BYTES + MH2IPC_RB_BYTES + MH2IPC_STATE_BYTES)
/* The second ring (development builds, the pass on a thread of its own: MH2_PASS_THREAD=1): the block is this much larger then, and only then; the ring's size
 * is in the header (pass.size, 0: no ring). */
#define MH2IPC_PASS_BYTES (16u << 20)

/* The largest payload of one record; GIF data and readbacks are split. */
#define MH2IPC_REC_MAX (1u << 20)

#define MH2IPC_AUDIO_RATE 48000u

/* The processes' states (Mh2IpcHeader.host_state, core_state). */
enum {
    MH2IPC_STATE_NONE = 0,
    MH2IPC_STATE_RUNNING = 1,    /* the core: attached */
    MH2IPC_STATE_EXITED = 2,     /* exit code in core_exit_code */
    MH2IPC_STATE_FATAL = 3,      /* message in fatal_message */
};

/* The core's exit status when it was given something it refuses to run with (a text pack that can't be read, a save
 * state made with another pack): core_state is MH2IPC_STATE_FATAL with the message to show, and nothing is wrong with the
 * program. The core's host.h has the same number (HOST_EXIT_REFUSED). */
#define MH2IPC_EXIT_REFUSED 2
#define MH2IPC_CORE_BASE 0u      /* core_kind: the base core (it does not write the word) */
#define MH2IPC_CORE_CONTENT 1u   /* the content core: its renderer has 8 MB */

/* Mh2IpcHeader.host_flags: what the host has (logs only: never game-visible). */
enum {
    MH2IPC_HOST_GS = 1u << 0,     /* a GS renderer (mh2gs on a Vulkan device) */
    MH2IPC_HOST_AUDIO = 1u << 1,  /* a sound device */
    MH2IPC_HOST_MOVIE = 1u << 2,  /* the movie player (mh2movie) */
    MH2IPC_HOST_PACE = 1u << 3,   /* real-time play: the core paces its VBlanks to the host's clock */
    /* A USB keyboard is connected to the PS2 (the owner's keyboard sessions, 2026-11-03): the core's IOP answers
     * "one keyboard" from the first request, and the game sets it up as a PS2 with a keyboard does. Written once,
     * before the host starts the core, and unchanged for the session (the game's boot sequence depends on it).
     * The keys come in Mh2IpcKeys. Unlike the others, this flag decides what the game computes: a USB keyboard is
     * a device the game itself reads, not only something the host shows. */
    MH2IPC_HOST_USB_KEYBOARD = 1u << 4,
    /* fast loading (the setting fast_loading): the core does not wait for real time while the game's loading screen is open
     * (Ipc_LoadingScreen). Pacing only: the VBlanks, the frame clock and everything computed are what they are paced. */
    MH2IPC_HOST_FASTLOAD = 1u << 5,
};

/* Mh2IpcHeader.pace_flags (host -> core, changes at any time): the host's pacing of the core's VBlanks (host_ipc.c, Ipc_Pace).
 *  - HOLD: the plain pause. The core lets no VBlank through but those the host grants (pace_grant, a count that only grows:
 *    one VBlank for each); it keeps the last picture on the screen, nothing else is touched.
 *  - FF: fast-forward. The core does not wait for real time (or waits for 100 / pace_cap of a VBlank's time, with a cap), and
 *    the samples of those VBlanks are dropped. Never while a movie is open: its picture and sound are timed by the VBlanks.
 * Pacing is all of it: the game cannot tell, and a recorded session is the same with or without. */
enum {
    MH2IPC_PACE_HOLD = 1u << 0,
    MH2IPC_PACE_FF = 1u << 1,
};
/* Mh2IpcHeader.pace_state (core -> host) */
enum {
    MH2IPC_PACE_HELD = 1u << 0,      /* the core waits in the pause now */
    MH2IPC_PACE_UNPACED = 1u << 1,   /* the VBlank in progress is not waited for (fast loading or fast-forward): its samples are dropped */
    MH2IPC_PACE_LOADING = 1u << 2,   /* the game's loading screen is open (the host logs how long, in either mode) */
};

/* The events one side waits on (mh2ipc_os.h). */
enum {
    MH2IPC_EV_GS_DATA = 0,    /* the host waits for GS records (word: gs.head) */
    MH2IPC_EV_GS_SPACE = 1,   /* the core waits for room in the GS ring (word: gs.tail) */
    MH2IPC_EV_RB_DONE = 2,    /* the core waits for a readback (word: rb_done_seq) */
    MH2IPC_EV_CONTROL = 3,    /* the host waits for the core's state (word: core_state) */
    MH2IPC_EV_SCHED = 4,      /* the core waits for the pad schedule (word: sched.horizon) */
    MH2IPC_EV_PASS_DATA = 5,  /* the host's reader waits for the pass thread's records (word: pass.head; development builds) */
    MH2IPC_EV_PASS_SPACE = 6, /* the pass thread waits for room in its ring (word: pass.tail) */
    MH2IPC_EV_COUNT = 7
};

/* GS ring records (Mh2IpcRec.type). */
enum {
    MH2IPC_REC_PAD = 0,          /* skip to the ring's end */
    MH2IPC_REC_GIF = 1,          /* arg0 = PATH (1-3); payload: whole qwords, GIFtags included */
    MH2IPC_REC_PRIV = 2,         /* arg0 = address (0x12000000-0x120000E0, 0x12001000-0x12001080); payload: u64 */
    MH2IPC_REC_VSYNC = 3,        /* arg0 = field; arg1 = the movie clock + 1 (0: no movie shown); payload: u64 VBlank */
    MH2IPC_REC_READBACK = 4,     /* arg0 = qwords (<= MH2IPC_RB_BYTES / 16); arg1 = sequence number */
    MH2IPC_REC_MOVIE_OPEN = 5,   /* payload: Mh2IpcMovieOpen, then the file's path (NUL-terminated) */
    MH2IPC_REC_MOVIE_CLOSE = 6,  /* arg0 = the movie's id: it ends or is skipped */
    /* Save states (Mh2IpcState): the core waits for the answer (rb_done_seq = arg1), as for a readback. In the ring so
     * that the renderer's state is that of the records before it. */
    MH2IPC_REC_STATE_GET = 7,    /* arg1 = sequence number: the renderer's block goes to the state window (gs_bytes, gs_result) */
    MH2IPC_REC_STATE_SET = 8,    /* arg0 = the block's bytes in the state window; arg1 = sequence number: the renderer takes it (gs_result) */
    MH2IPC_REC_CAMERA = 9,       /* payload: 32 floats, the view and the projection (gMtxView, gMtxProjClip) a model is drawn with; the renderer's
                                  * in-between pictures use it, nothing else: it is not part of the picture's stream (the ring's hash skips it) */
    MH2IPC_REC_MODEL = 10,       /* payload: Mh2IpcModel. The DMA chain of a Model_Draw starts here: the GIF data (path 1) that follows, up to the next
                                  * MODEL record, is what that draw made. arg0 = the model's handle (0: the end of the draw's chain, where the data that
                                  * follows is nobody's; no payload), arg1 = the draw's flags. Like the camera, it is not part of
                                  * the picture's stream (the ring's hash skips it) */
    MH2IPC_REC_ANCHOR = 11,      /* payload: Mh2IpcAnchor. A 2D packet that an element placed from a 3D point queued starts here (PC_GS_ANCHOR): the GIF
                                  * data (path 2) that follows, up to the next ANCHOR record, is that element's. arg0 = the anchor's id (0: the end of the
                                  * packet, no payload). Not part of the picture's stream (the ring's hash skips it) */
    MH2IPC_REC_PASS = 12,        /* The second pass (development builds). arg0 = the kind: 0 and 1 bracket the real records of a DMA window (a transfer to
                                  * VIF1), 2 begins the pass's stream for it (arg1 = the alpha in 1/256), 3 is GIF data of the pass (arg1 = the path; payload:
                                  * whole qwords), 4 ends it, 5 says that no pass was run for the window. On the pass thread's ring: 6 says that all of a window's
                                  * records are sent; 7 is a whole moment in one record (arg1 as kind 2's; the data: a qword with the number of qwords that follow, then
                                  * for each piece of kind 3 a qword (path, qwords, 0, 0) and its data), 8 the continuation of a kind 7 that was too long for one record.
                                  * Not part of the picture's stream (the ring's hash skips it) */
    MH2IPC_REC_STAMP = 13,       /* Development builds, with the second pass on: before each VSync record, the time the core reached the VSync, payload: u64 mh2ipc_time_ns(), then a u64 with the pacing's timeline at this VBlank (0: the VBlank was not paced, 1: on the timeline, 2: the timeline began or broke here).
                                  * The host's clock uses it in place of the time its reader took the record up. arg0 = the live plan's VBlank number the step's moments were made at (0: none; plan 11's step 5a). Not part of the picture's stream (the ring's hash skips it) */
};

typedef struct Mh2IpcModel {
    mh2ipc_u32 prog;        /* the model's VU1 program */
    mh2ipc_u32 top;         /* the chain's first tag (the guest address the draw's packets start at) */
    mh2ipc_u32 last;        /* its last tag */
    float world[16];        /* gMtxSlots[0], the model's matrix (the game's row-major layout: a row vector times the matrix) at the draw */
    float cam[32];          /* the view and the projection the draw was made with (as MH2IPC_REC_CAMERA: gMtxView, gMtxProjClip) */
    mh2ipc_u32 site;        /* the return address of Model_Draw: where in the game the draw was called from (a diagnostic; development builds) */
} Mh2IpcModel;

typedef struct Mh2IpcAnchor {
    float point[3];         /* the world point the 2D element is placed from */
    float cam[32];          /* the view and the projection of the game at the call (gMtxView, gMtxProjClip) */
} Mh2IpcAnchor;

typedef struct Mh2IpcRec {
    mh2ipc_u32 type;
    mh2ipc_u32 bytes;    /* the payload's size, before padding */
    mh2ipc_u32 arg0;
    mh2ipc_u32 arg1;
} Mh2IpcRec;

typedef struct Mh2IpcMovieOpen {
    mh2ipc_u32 offset_lo, offset_hi;  /* the Sofdec file inside the file at the path */
    mh2ipc_u32 size;
    mh2ipc_u32 mode;                  /* the game's display mode (the descriptor's +0xE) */
    mh2ipc_s32 volume;                /* the game's movie volume, 0.1 dB steps (-960: silent) */
    mh2ipc_u32 id;                    /* the core's count of movies opened (never 0) */
    mh2ipc_u32 reserved[2];
} Mh2IpcMovieOpen;

/* One DualShock 2 as the host sees it. Button bits in the pad's own order,
 * 1 = pressed: 0 SELECT, 1 L3, 2 R3, 3 START, 4 UP, 5 RIGHT, 6 DOWN, 7 LEFT,
 * 8 L2, 9 R2, 10 L1, 11 R1, 12 TRIANGLE, 13 CIRCLE, 14 CROSS, 15 SQUARE.
 * Pressures (0-255): RIGHT LEFT UP DOWN TRIANGLE CIRCLE CROSS SQUARE L1 R1
 * L2 R2. Sticks: 0-255, 0x80 the centre, 0 up or left. */
typedef struct Mh2IpcPad {
    mh2ipc_u32 connected;
    mh2ipc_u16 buttons;
    mh2ipc_u8 rx, ry, lx, ly;
    mh2ipc_u8 pressure[12];
    mh2ipc_u8 camTurn, camTilt;   /* the modern camera's stick, each a signed byte (0: no push; an older writer left zeros) */
} Mh2IpcPad;

/* A USB keyboard: the keys down, as a bitmap of their HID usage IDs
 * (keyboard page 0x07: bit (u & 31) of keys[u >> 5]), and the modifier
 * byte of a boot report (bit 0 left Ctrl, 1 left Shift, 2 left Alt,
 * 3 left GUI, 4-7 the right ones). */
typedef struct Mh2IpcKeyboard {
    mh2ipc_u32 connected;
    mh2ipc_u32 modifiers;
    mh2ipc_u32 keys[8];
} Mh2IpcKeyboard;

#define MH2IPC_PADS 2

/* Written by the host whenever its input changes; a sequence lock: seq is
 * odd while the host writes. */
typedef struct Mh2IpcInput {
    mh2ipc_u32 seq;
    mh2ipc_u32 reserved;
    Mh2IpcPad pad[MH2IPC_PADS];
    Mh2IpcKeyboard keyboard;
} Mh2IpcInput;


/* The watch's size: a release header reserves its place, so that the pass's ring and the live plans are at the same offsets in both builds. */
#define MH2IPC_WATCH_BYTES 1204u

/* The live plan (the pass on; plan 11's step 5a), host -> core, a sequence lock as the input mailbox's: at each anchor the host writes the plan it predicts for the step two steps on
 * (hfr.h, hfr_predict), in the slot its number names, so that it is there before the core's first window of that step; the core's first window of a step reads the slot of the VBlank number the step's VSync record is expected to have (mh2ipc_liveplan_read)
 * and makes its moments at the alphas when the plan's `vblank` is that number. */
#define MH2IPC_LIVE_MAX 6u
typedef struct Mh2IpcLivePlan {
    mh2ipc_u32 seq;                       /* the host's: odd while it writes, 0 until the first write */
    mh2ipc_u32 vblank;                    /* the VBlank number (the VSync record's, low 32 bits) the step's VSync record is expected to have */
    mh2ipc_u32 n;                         /* the moments, rising: 0 to MH2IPC_LIVE_MAX */
    mh2ipc_u32 fb;                        /* when the plan is not in time: the moments of a step the pass makes by itself (60: one, at 128; above 60: none) */
    mh2ipc_u32 alpha[8];                  /* in 1/256 */
    mh2ipc_u32 reserved[4];
} Mh2IpcLivePlan;

/* The USB keyboard's key events, host -> core (MH2IPC_HOST_USB_KEYBOARD). One event a key going down or up, in the
 * order the player made them: the host's window sends a key press (its HID usage ID: SDL's scancodes are the
 * same numbers) with `down` 1, and its release with `down` 0. The usage IDs are the keyboard page's (0x07): 0x04-0x1D
 * the letters, 0x1E-0x27 the digits, 0x28 Return, 0x29 Escape, 0x2A Backspace, 0x2B Tab, 0x2C Space, ..., 0x3A-0x45
 * the function keys, 0x4F-0x52 the arrows, 0x87-0x8B the Japanese keyboard's extra keys, and 0xE0-0xE7 the modifiers
 * (Left Ctrl, Shift, Alt, GUI, then the right ones); other usages are passed to the game as they are. The
 * event with usage 0 and `down` 0 is "every key up" (the window lost the focus).
 *
 * The core takes the events at a VBlank's start, together with the pads, in the order they were made, and makes
 * the keyboard's report of that VBlank (the USB boot report: the modifier byte, 0, the six keys down longest first)
 * from them: a key that goes down and up inside one VBlank is held one VBlank, so that the game sees it (an
 * event that would undo one applied in this VBlank waits for the next). The host never writes tail, and the core
 * never writes head or events. A full ring (nothing read for 256 events) drops the host's new event and counts it
 * in dropped; the host sends "every key up" when it can again.
 *
 * The ring is a plain single-producer single-consumer ring with free-running indices (mod 2^32); the host writes
 * the event, then head (mh2ipc_keys_push). */
#define MH2IPC_KEY_RING 256u   /* events; a power of two */
#define MH2IPC_KEY_DOWN 0x100u /* in an event: the key went down (clear: up); the usage ID is the low byte */
typedef struct Mh2IpcKeys {
    mh2ipc_u32 head;           /* events written (the host's) */
    mh2ipc_u32 dropped;        /* events the host could not write (the host's) */
    mh2ipc_u32 reserved0[14];
    mh2ipc_u32 tail;           /* events read (the core's) */
    mh2ipc_u32 reserved1[15];
    mh2ipc_u32 events[MH2IPC_KEY_RING];   /* usage | MH2IPC_KEY_DOWN */
} Mh2IpcKeys;

/* A ring's indices, a cache line each for the constants, the producer and
 * the consumer. */
typedef struct Mh2IpcRing {
    mh2ipc_u32 offset;            /* from the block's start */
    mh2ipc_u32 size;              /* a power of two */
    mh2ipc_u32 reserved0[14];
    mh2ipc_u32 head;              /* the producer's: bytes written */
    mh2ipc_u32 consumer_waiting;  /* the consumer sleeps on head */
    mh2ipc_u32 dropped;           /* bytes the producer dropped (the sample ring, when full) */
    mh2ipc_u32 reserved1[13];
    mh2ipc_u32 tail;              /* the consumer's: bytes read */
    mh2ipc_u32 producer_waiting;  /* the producer sleeps on tail */
    mh2ipc_u32 reserved2[14];
} Mh2IpcRing;

/* The pad schedule, host -> core (a scripted run): the pad states of port 0 come together with the VBlank at which
 * each takes effect, so that the core's VBlank N gets the state that holds at N whatever the two processes' speeds.
 * Without it (enabled 0) the core reads the input mailbox at each VBlank, as the player's host writes it.
 *  - The host sets `enabled` before it starts the core, and unchanged for the session. It pushes entries in
 *    ascending VBlank order (an entry holds until the next) and stores `horizon` after them: the VBlank through
 *    which every entry has been pushed (the next one's VBlank - 1; ~0: there are no more).
 *  - At its VBlank N, step 7, the core reads horizon first, then takes the entries up to N; if the ring is
 *    empty and horizon < N it waits (MH2IPC_EV_SCHED) until the host has pushed more. Before the first entry the
 *    pad is neutral (connected, nothing pressed, sticks centred).
 *  - The ports other than 0, and the keyboard, come from the mailbox as ever. VBlanks count as in Ipc_Pace,
 *    in 32 bits.
 *  - A load of a save state goes back in VBlanks (version 7). The core then asks the host to start the schedule again:
 *    it stores rewind_vblank (the VBlank that starts next), then epoch + 1, and waits (the host looks each pass of its
 *    loop) for epoch_ack to equal epoch. The host, seeing a new epoch, drops the entries the core has not taken
 *    (head = tail: the core takes none while it waits), pushes again from the entry that holds at rewind_vblank on,
 *    stores horizon, and stores epoch_ack last. The core then forgets the pad it held. */
#define MH2IPC_SCHED_RING 256u   /* entries; a power of two */
typedef struct Mh2IpcSchedEntry {
    mh2ipc_u32 vblank;
    mh2ipc_u32 reserved;
    Mh2IpcPad pad;
} Mh2IpcSchedEntry;
typedef struct Mh2IpcSched {
    mh2ipc_u32 enabled;        /* the host's, once */
    mh2ipc_u32 reserved0[15];
    mh2ipc_u32 head;           /* the host's: entries pushed */
    mh2ipc_u32 horizon;        /* the host's: written after head */
    mh2ipc_u32 epoch_ack;      /* the host's: the epoch it has handled (written last) */
    mh2ipc_u32 reserved1[13];
    mh2ipc_u32 tail;           /* the core's: entries taken */
    mh2ipc_u32 waiting;        /* the core sleeps on horizon */
    mh2ipc_u32 epoch;          /* the core's: loads that went back; the host starts again for each */
    mh2ipc_u32 rewind_vblank;  /* the core's: the VBlank that starts next, written before epoch */
    mh2ipc_u32 reserved2[12];
    Mh2IpcSchedEntry entries[MH2IPC_SCHED_RING];
} Mh2IpcSched;

/* The freecam's mailbox (host_ipc.c reads it at each VBlank's start; pc/host/freecam.c writes it). Floats travel as their IEEE-754
 * single bits. seq is the host's, written last (a sequence lock: odd while it writes, 0 until the first write); cam_seq the
 * core's, the same way. flags: MH2IPC_FC_*. The pose is absolute (pc/core/pal_freecam.h, PalFreecamPose): eye in the game's
 * world, yaw and pitch and roll in radians, fov in radians (0: the game's). The core publishes the game's camera (eye, at, fov of
 * the last field frame's draw) after each VBlank's start, so the freecam starts where the game's camera is; cam_seq 0: none yet. */
enum {
    MH2IPC_FC_ON = 1u,            /* the freecam has the view */
    MH2IPC_FC_HOLD = 2u,          /* (the host's: the game has the pad; the pose stays) */
    MH2IPC_FC_FROZEN = 4u,        /* the pause: the host asks for frozen frames (Pal_FreezeSet's hold) */
    MH2IPC_FC_HIDE_SHIFT = 8,     /* the hide mask (PalFreecamPose.hide, a bit for each PAL_STAGE_*): 13 bits from here */
    MH2IPC_FC_HIDE_BITS = 13,
    MH2IPC_FC_HUD = 0x6FF         /* the HUD's stages together (photo mode's H): all but the scene's two fades and the loading letters */
};
typedef struct Mh2IpcFreecam {
    /* host -> core */
    mh2ipc_u32 seq;
    mh2ipc_u32 flags;          /* on, hold, frozen, hide mask */
    mh2ipc_u32 eye[3];
    mh2ipc_u32 yaw, pitch, roll;
    mh2ipc_u32 fov;
    mh2ipc_u32 reserved_hold;  /* (unused: the pause is the FROZEN flag) */
    mh2ipc_u32 step_grant;     /* the step count: one real frame for each, while the pause holds; it only grows (Pal_FreezeSet's grant) */
    mh2ipc_u32 reserved0[13];
    /* core -> host: the game's camera, so that the freecam starts where it is */
    mh2ipc_u32 cam_seq;
    mh2ipc_u32 cam_eye[3];
    mh2ipc_u32 cam_at[3];
    mh2ipc_u32 cam_fov;
    /* core -> host: what the last draw was (its own sequence lock, st_seq: odd while the core writes, 0 until the first write).
     * st_active: 1 when that draw was a field scene's with the pose in force (0: a menu, a result screen, the title, ...: the
     * host gives the game its input back and the pose waits). st_echo: the sequence of the pose that was in force for that draw
     * (the host knows its pose reached the draw once st_echo is at least the sequence it wrote; after the freecam is turned on, the
     * core leaves st_echo behind until a field draw has been made with the pose, or two game steps have begun with none: so the
     * answer of the first draw is that draw's). st_vblank: the VBlank whose
     * start published these words (low 32 bits). st_state, st_why, st_taken: Pal_FreezeState's answer (0 running, 1 waiting,
     * 2 frozen; why a pause waits: 1 not a field scene, 2 a movie is open, 3 a memory card request is running; the steps the core has
     * taken, counted as the host counts them: a load of a save state does not move it against the host's step_grant). */
    mh2ipc_u32 st_seq;
    mh2ipc_u32 st_active;
    mh2ipc_u32 st_echo;
    mh2ipc_u32 st_vblank;
    mh2ipc_u32 st_state, st_why, st_taken;
    mh2ipc_u32 reserved1[1];
} Mh2IpcFreecam;

/* Save states. The core makes and loads the states (kernel/state.c); the host
 * asks, and carries the renderer's part:
 *  - A request (host -> core): the host writes req_kind and req_path, then req_seq last. The core looks at each VBlank's
 *    start (kernel/state.c, KState_VBlankStart) and, for a new req_seq, saves or loads there. It answers with ack_result,
 *    ack_vblank and ack_message, and ack_seq last (= req_seq). One request at a time: the host sends the next after the ack.
 *  - The renderer's block (the GS's memory and registers: 4.2 MB, mh2gs_state_get): the GS state is in the host's process, so
 *    the block crosses in the state window (MH2IPC_STATE_BYTES, at window_offset). For a save the core puts a
 *    MH2IPC_REC_STATE_GET in the GS ring, which the host's GS thread answers in order with the others, so the block is that of
 *    the records before it: it writes the block into the window, then gs_bytes and gs_result, then rb_done_seq (= the
 *    record's arg1, the sequence the readbacks use), and wakes the core. For a load the core copies the block into
 *    the window and puts MH2IPC_REC_STATE_SET (arg0 = bytes); the host applies it at that record and answers the same way.
 *    gs_result is 0, or mh2gs's negative code as an unsigned number.
 *  - identity (host, once, before the core starts): the renderer's build and the settings that change its state (ssaa, hires,
 *    texture packs); "" without a renderer. The header of a state keeps it, and a load under another is refused.
 *  - After a load (the core's, in its resume): loads counts them; audio_flush_head is the audio ring's head at that moment,
 *    and audio_flush_seq goes up: the host's sound thread drops what the ring holds up to that position (the old timeline's
 *    sound) and fades the rest in. The key ring is emptied by the core (tail = head). The pad schedule starts again
 *    (Mh2IpcSched). Real-time pacing ties its clock again at the loaded VBlank (the core's own). */
#define MH2IPC_STATE_PATH 1024u
#define MH2IPC_STATE_MSG 160u
enum {   /* req_kind */
    MH2IPC_ST_NONE = 0,
    MH2IPC_ST_SAVE = 1,
    MH2IPC_ST_LOAD = 2,
};
enum {   /* ack_result */
    MH2IPC_ST_DONE = 0,        /* it went through */
    MH2IPC_ST_REFUSED = 1,     /* ack_message says why */
};
typedef struct Mh2IpcState {
    /* the host's, once, before the core starts */
    mh2ipc_u32 window_offset;
    mh2ipc_u32 window_bytes;
    char identity[32];
    mh2ipc_u32 reserved0[6];
    /* host -> core */
    mh2ipc_u32 req_seq;
    mh2ipc_u32 req_kind;
    mh2ipc_u32 reserved1[14];
    char req_path[MH2IPC_STATE_PATH];
    /* core -> host */
    mh2ipc_u32 ack_seq;
    mh2ipc_u32 ack_result;
    mh2ipc_u32 ack_vblank_lo, ack_vblank_hi;   /* the state's VBlank: the one that starts when it is loaded */
    mh2ipc_u32 reserved2[12];
    char ack_message[MH2IPC_STATE_MSG];
    /* the renderer's block, core <-> host */
    mh2ipc_u32 gs_bytes;
    mh2ipc_u32 gs_result;
    mh2ipc_u32 reserved3[14];
    /* after a load, the core's */
    mh2ipc_u32 loads;
    mh2ipc_u32 audio_flush_head;
    mh2ipc_u32 audio_flush_seq;
    mh2ipc_u32 reserved4[13];
} Mh2IpcState;

typedef struct Mh2IpcHeader {
    /* 0x000: written once by the host, before it starts the core */
    mh2ipc_u32 magic;
    mh2ipc_u32 version;
    mh2ipc_u32 header_bytes;
    mh2ipc_u32 total_bytes;
    mh2ipc_u32 host_pid;
    mh2ipc_u32 rb_offset;          /* the readback buffer */
    mh2ipc_u32 rb_size;
    /* mh2's --quit-after N: the core ends at the start of VBlank N + 1, after N VSyncs, whatever
     * the two processes' speeds (0: no such end). A scripted run ends at the same frame every time. */
    mh2ipc_u32 quit_vblank_lo;
    mh2ipc_u32 quit_vblank_hi;
    mh2ipc_u32 reserved0[7];
    /* 0x040: the host's */
    mh2ipc_u32 host_state;
    mh2ipc_u32 host_flags;         /* MH2IPC_HOST_* */
    mh2ipc_u32 quit_request;       /* the player closed the window: the core ends at its next safe point */
    mh2ipc_u32 rb_done_seq;        /* the readback of this sequence number is in the buffer */
    mh2ipc_u32 audio_queued;       /* frames queued for the sound device (pacing only) */
    mh2ipc_u32 presented_lo;       /* the VBlank of the last picture shown (logs only) */
    mh2ipc_u32 presented_hi;
    mh2ipc_u32 pace_flags;         /* MH2IPC_PACE_* */
    mh2ipc_u32 pace_grant;         /* the VBlanks let through while HOLD: it only grows */
    mh2ipc_u32 pace_cap;           /* fast-forward's cap, in hundredths of the game's speed (0: none) */
    mh2ipc_u32 reserved1[6];
    /* 0x080: the core's */
    mh2ipc_u32 core_pid;
    mh2ipc_u32 core_state;
    mh2ipc_u32 core_exit_code;
    mh2ipc_u32 rb_core_waiting;    /* the core sleeps on rb_done_seq */
    mh2ipc_u32 rumble[MH2IPC_PADS];   /* port's motors: small (0 or 1) | large (0-255) << 8 */
    /* The movie's sound (the core's, MH2IPC_REC_VSYNC's clock for the picture): the movie movie_sound_id
     * is mixed into the sample ring's bytes from movie_sound_begin to movie_sound_end (~0: on), as the
     * reference mixes it into the SPU2's output while it plays. The id is written last. */
    mh2ipc_u32 movie_sound_id;
    mh2ipc_u32 movie_sound_begin;
    mh2ipc_u32 movie_sound_end;
    mh2ipc_u32 pace_state;
    mh2ipc_u32 pace_taken;
    mh2ipc_u32 pace_vblank;
    mh2ipc_u32 reserved2[4];
    /* 0x0C0: host -> core */
    Mh2IpcInput input;
    mh2ipc_u32 reserved3a[(0x140 - 0x0C0 - sizeof(Mh2IpcInput)) / 4];   /* where the development build has its injections */
    /* 0x140, both builds: the freecam's mailbox, reserved and unused until the host's freecam */
    Mh2IpcFreecam freecam;
    /* core -> host, written once as the core attaches: the kind of core (MH2IPC_CORE_*). The base core writes nothing, so it stays 0. The host
     * has already chosen the renderer's memory from the core's file (main.c, core_is_content); this word is the check that the
     * choice was right (the content core's textures reach blocks above 16383, which a 4 MB renderer would wrap). */
    mh2ipc_u32 core_kind;
    mh2ipc_u32 reserved3[(0x200 - 0x140 - sizeof(Mh2IpcFreecam)) / 4 - 1];
    /* 0x200: core -> host, with core_state = FATAL */
    char fatal_message[512];
    /* 0x400 */
    Mh2IpcRing gs;
    Mh2IpcRing audio;
    /* 0x580: host -> core, the USB keyboard's key events */
    Mh2IpcKeys keys;
    /* host -> core, a scripted run's pad states by VBlank (the same in both builds) */
    Mh2IpcSched sched;
    /* save states: the requests, the answers, the renderer's block window (both builds) */
    Mh2IpcState state;
    mh2ipc_u8 watch_reserved[MH2IPC_WATCH_BYTES];   /* where the development build has its telemetry */
    /* core -> host, the pass thread's records (MH2IPC_REC_PASS only); the ring's offset and size are written by the host when the block is made (mh2ipc_layout_pass) */
    Mh2IpcRing pass;
    /* host -> core, the live alphas (plan 11's step 5a): after the pass ring, so that the older fields keep their places */
    Mh2IpcLivePlan live[2];               /* the plans for the two steps ahead: the slot of a plan is (its VBlank number / 2) & 1 (the pass on) */
} Mh2IpcHeader;

#define MH2IPC_ASSERT(c, m) _Static_assert(c, m)
MH2IPC_ASSERT(sizeof(mh2ipc_u32) == 4 && sizeof(mh2ipc_u64) == 8 && sizeof(mh2ipc_u16) == 2, "types");
MH2IPC_ASSERT(sizeof(Mh2IpcRec) == 16, "Mh2IpcRec");
MH2IPC_ASSERT(sizeof(Mh2IpcMovieOpen) == 32, "Mh2IpcMovieOpen");
MH2IPC_ASSERT(sizeof(Mh2IpcPad) == 24, "Mh2IpcPad");
MH2IPC_ASSERT(sizeof(Mh2IpcKeyboard) == 40, "Mh2IpcKeyboard");
MH2IPC_ASSERT(sizeof(Mh2IpcInput) == 96, "Mh2IpcInput");
MH2IPC_ASSERT(sizeof(Mh2IpcRing) == 192, "Mh2IpcRing");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcRing, head) == 64 && __builtin_offsetof(Mh2IpcRing, tail) == 128, "Mh2IpcRing");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, host_state) == 0x40, "Mh2IpcHeader");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, core_pid) == 0x80, "Mh2IpcHeader");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, input) == 0xC0, "Mh2IpcHeader");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, fatal_message) == 0x200, "Mh2IpcHeader");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, gs) == 0x400, "Mh2IpcHeader");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, keys) == 0x400 + 2 * 192, "Mh2IpcHeader");
MH2IPC_ASSERT(sizeof(Mh2IpcSchedEntry) == 32 && __builtin_offsetof(Mh2IpcSched, head) == 64 && __builtin_offsetof(Mh2IpcSched, tail) == 128 && __builtin_offsetof(Mh2IpcSched, entries) == 192, "Mh2IpcSched");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, sched) == 0x400 + 2 * 192 + sizeof(Mh2IpcKeys), "Mh2IpcHeader");
MH2IPC_ASSERT(sizeof(Mh2IpcLivePlan) == 64, "Mh2IpcLivePlan");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, pass) == 0x400 + 2 * 192 + sizeof(Mh2IpcKeys) + sizeof(Mh2IpcSched) + sizeof(Mh2IpcState) + MH2IPC_WATCH_BYTES, "Mh2IpcHeader");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, live) == __builtin_offsetof(Mh2IpcHeader, pass) + sizeof(Mh2IpcRing), "Mh2IpcHeader");
MH2IPC_ASSERT(sizeof(Mh2IpcKeys) == 128 + 4 * MH2IPC_KEY_RING && __builtin_offsetof(Mh2IpcKeys, tail) == 64 && __builtin_offsetof(Mh2IpcKeys, events) == 128, "Mh2IpcKeys");
MH2IPC_ASSERT(sizeof(Mh2IpcHeader) == 0x400 + 2 * 192 + sizeof(Mh2IpcKeys) + sizeof(Mh2IpcSched) + sizeof(Mh2IpcState) + MH2IPC_WATCH_BYTES + sizeof(Mh2IpcRing) + 2 * sizeof(Mh2IpcLivePlan), "Mh2IpcHeader");
MH2IPC_ASSERT(sizeof(Mh2IpcHeader) <= MH2IPC_HEADER_BYTES, "Mh2IpcHeader");
MH2IPC_ASSERT(sizeof(Mh2IpcFreecam) == 160 && __builtin_offsetof(Mh2IpcHeader, freecam) == 0x140, "Mh2IpcFreecam");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcHeader, state) == 0x400 + 2 * 192 + sizeof(Mh2IpcKeys) + sizeof(Mh2IpcSched), "Mh2IpcState");
MH2IPC_ASSERT(__builtin_offsetof(Mh2IpcState, req_seq) == 64 && __builtin_offsetof(Mh2IpcState, req_path) == 128 &&
                  __builtin_offsetof(Mh2IpcState, ack_seq) == 128 + MH2IPC_STATE_PATH, "Mh2IpcState");

/* ---- the rings' logic (both sides) ---- */

static inline mh2ipc_u32 mh2ipc_load(const mh2ipc_u32 *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void mh2ipc_store(mh2ipc_u32 *p, mh2ipc_u32 v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* The flag-then-check order of a sleeper against the store-then-check
 * order of a waker: both sides put a full fence between them. */
static inline void mh2ipc_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static inline mh2ipc_u32 mh2ipc_rec_len(mh2ipc_u32 payload)
{
    return 16u + ((payload + 15u) & ~15u);
}

/* Lays out the block's header (the host, on a zeroed block). */
static inline void mh2ipc_layout(Mh2IpcHeader *h, mh2ipc_u32 host_pid)
{
    h->version = MH2IPC_VERSION;
    h->header_bytes = MH2IPC_HEADER_BYTES;
    h->total_bytes = MH2IPC_TOTAL_BYTES;
    h->host_pid = host_pid;
    h->gs.offset = MH2IPC_HEADER_BYTES;
    h->gs.size = MH2IPC_GS_BYTES;
    h->audio.offset = MH2IPC_HEADER_BYTES + MH2IPC_GS_BYTES;
    h->audio.size = MH2IPC_AUDIO_BYTES;
    h->rb_offset = MH2IPC_HEADER_BYTES + MH2IPC_GS_BYTES + MH2IPC_AUDIO_BYTES;
    h->rb_size = MH2IPC_RB_BYTES;
    h->state.window_offset = MH2IPC_HEADER_BYTES + MH2IPC_GS_BYTES + MH2IPC_AUDIO_BYTES + MH2IPC_RB_BYTES;
    h->state.window_bytes = MH2IPC_STATE_BYTES;
    h->host_state = MH2IPC_STATE_RUNNING;
    mh2ipc_fence();
    mh2ipc_store(&h->magic, MH2IPC_MAGIC);
}

/* The second ring's place (the host, after mh2ipc_layout and before it starts the core; the block was made MH2IPC_PASS_BYTES larger). */
static inline void mh2ipc_layout_pass(Mh2IpcHeader *h)
{
    h->pass.offset = MH2IPC_TOTAL_BYTES;
    h->pass.size = MH2IPC_PASS_BYTES;
    h->total_bytes = MH2IPC_TOTAL_BYTES + MH2IPC_PASS_BYTES;
}

/* Producer: the place of a record with `payload` bytes, or 0 when the ring
 * hasn't the room now. *advance is what mh2ipc_rec_commit adds to head
 * (with the PAD record before it, if any). The record's header is written
 * by mh2ipc_rec_commit; the payload by the caller, at the returned
 * pointer + 16. */
static inline Mh2IpcRec *mh2ipc_rec_reserve(Mh2IpcRing *r, mh2ipc_u8 *block, mh2ipc_u32 payload, mh2ipc_u32 *advance)
{
    mh2ipc_u32 len = mh2ipc_rec_len(payload);
    mh2ipc_u32 head = r->head;
    mh2ipc_u32 pos = head & (r->size - 1u);
    mh2ipc_u32 pad = pos + len > r->size ? r->size - pos : 0u;
    mh2ipc_u32 used = head - mh2ipc_load(&r->tail);
    if (r->size - used < pad + len)
        return 0;
    if (pad) {
        Mh2IpcRec *p = (Mh2IpcRec *)(block + r->offset + pos);
        p->type = MH2IPC_REC_PAD;
        p->bytes = pad - 16u;
        p->arg0 = p->arg1 = 0;
        pos = 0;
    }
    *advance = pad + len;
    return (Mh2IpcRec *)(block + r->offset + pos);
}

/* Producer: publishes the record. Returns 1 when the consumer sleeps and
 * must be woken. */
static inline int mh2ipc_rec_commit(Mh2IpcRing *r, Mh2IpcRec *rec, mh2ipc_u32 type, mh2ipc_u32 payload,
                                    mh2ipc_u32 arg0, mh2ipc_u32 arg1, mh2ipc_u32 advance)
{
    rec->type = type;
    rec->bytes = payload;
    rec->arg0 = arg0;
    rec->arg1 = arg1;
    mh2ipc_store(&r->head, r->head + advance);
    mh2ipc_fence();
    return __atomic_load_n(&r->consumer_waiting, __ATOMIC_RELAXED) != 0;
}

/* Consumer: the next record, or 0 when the ring is empty. PAD records are
 * skipped here. */
static inline const Mh2IpcRec *mh2ipc_rec_peek(Mh2IpcRing *r, const mh2ipc_u8 *block)
{
    for (;;) {
        mh2ipc_u32 tail = r->tail;
        if (mh2ipc_load(&r->head) == tail)
            return 0;
        mh2ipc_u32 pos = tail & (r->size - 1u);
        const Mh2IpcRec *rec = (const Mh2IpcRec *)(block + r->offset + pos);
        if (rec->type != MH2IPC_REC_PAD)
            return rec;
        mh2ipc_store(&r->tail, tail + (r->size - pos));
    }
}

/* Consumer: done with the record peek returned. Returns 1 when the
 * producer sleeps and must be woken. */
static inline int mh2ipc_rec_release(Mh2IpcRing *r, const Mh2IpcRec *rec)
{
    mh2ipc_store(&r->tail, r->tail + mh2ipc_rec_len(rec->bytes));
    mh2ipc_fence();
    return __atomic_load_n(&r->producer_waiting, __ATOMIC_RELAXED) != 0;
}

/* The sample ring: a byte stream of whole frames (4 bytes). The producer
 * writes all of `bytes` or nothing (counted in dropped). Returns 1 if
 * written. */
static inline int mh2ipc_stream_write(Mh2IpcRing *r, mh2ipc_u8 *block, const void *data, mh2ipc_u32 bytes)
{
    mh2ipc_u32 head = r->head;
    if (r->size - (head - mh2ipc_load(&r->tail)) < bytes) {
        __atomic_fetch_add(&r->dropped, bytes, __ATOMIC_RELAXED);
        return 0;
    }
    mh2ipc_u32 pos = head & (r->size - 1u);
    mh2ipc_u32 first = r->size - pos < bytes ? r->size - pos : bytes;
    __builtin_memcpy(block + r->offset + pos, data, first);
    __builtin_memcpy(block + r->offset, (const mh2ipc_u8 *)data + first, bytes - first);
    mh2ipc_store(&r->head, head + bytes);
    return 1;
}

/* The consumer: up to `bytes` bytes; returns how many it read. */
static inline mh2ipc_u32 mh2ipc_stream_read(Mh2IpcRing *r, const mh2ipc_u8 *block, void *out, mh2ipc_u32 bytes)
{
    mh2ipc_u32 tail = r->tail;
    mh2ipc_u32 avail = mh2ipc_load(&r->head) - tail;
    mh2ipc_u32 n = avail < bytes ? avail : bytes;
    mh2ipc_u32 pos = tail & (r->size - 1u);
    mh2ipc_u32 first = r->size - pos < n ? r->size - pos : n;
    __builtin_memcpy(out, block + r->offset + pos, first);
    __builtin_memcpy((mh2ipc_u8 *)out + first, block + r->offset, n - first);
    mh2ipc_store(&r->tail, tail + n);
    return n;
}

/* The input mailbox (a sequence lock). The host writes ... */
static inline void mh2ipc_input_write(Mh2IpcInput *in, const Mh2IpcPad *pads, const Mh2IpcKeyboard *kb)
{
    mh2ipc_u32 s = in->seq;
    mh2ipc_store(&in->seq, s + 1u);
    mh2ipc_fence();
    mh2ipc_u32 *dst = (mh2ipc_u32 *)in->pad;
    const mh2ipc_u32 *src = (const mh2ipc_u32 *)pads;
    for (unsigned i = 0; i < sizeof(in->pad) / 4; i++)
        __atomic_store_n(&dst[i], src[i], __ATOMIC_RELAXED);
    dst = (mh2ipc_u32 *)&in->keyboard;
    src = (const mh2ipc_u32 *)kb;
    for (unsigned i = 0; i < sizeof(in->keyboard) / 4; i++)
        __atomic_store_n(&dst[i], src[i], __ATOMIC_RELAXED);
    mh2ipc_store(&in->seq, s + 2u);
}

/* ... and the core reads a consistent copy. */
static inline void mh2ipc_input_read(Mh2IpcInput *in, Mh2IpcPad *pads, Mh2IpcKeyboard *kb)
{
    for (;;) {
        mh2ipc_u32 s = mh2ipc_load(&in->seq);
        if (s & 1u)
            continue;
        mh2ipc_u32 *dst = (mh2ipc_u32 *)pads;
        const mh2ipc_u32 *src = (const mh2ipc_u32 *)in->pad;
        for (unsigned i = 0; i < sizeof(in->pad) / 4; i++)
            dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
        dst = (mh2ipc_u32 *)kb;
        src = (const mh2ipc_u32 *)&in->keyboard;
        for (unsigned i = 0; i < sizeof(in->keyboard) / 4; i++)
            dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&in->seq, __ATOMIC_RELAXED) == s)
            return;
    }
}

/* The host's: the plan for the step after the one whose anchor this is. */
static inline void mh2ipc_liveplan_write(Mh2IpcLivePlan *lp, mh2ipc_u32 vblank, mh2ipc_u32 n, const mh2ipc_u32 *alpha, mh2ipc_u32 fb)
{
    mh2ipc_u32 s = lp->seq;
    s += s & 1u;
    mh2ipc_store(&lp->seq, s + 1u);
    mh2ipc_fence();
    if (n > MH2IPC_LIVE_MAX)
        n = MH2IPC_LIVE_MAX;
    __atomic_store_n(&lp->vblank, vblank, __ATOMIC_RELAXED);
    __atomic_store_n(&lp->n, n, __ATOMIC_RELAXED);
    __atomic_store_n(&lp->fb, fb, __ATOMIC_RELAXED);
    for (mh2ipc_u32 i = 0; i < 8u; i++)
        __atomic_store_n(&lp->alpha[i], i < n ? alpha[i] : 0u, __ATOMIC_RELAXED);
    mh2ipc_store(&lp->seq, s + 2u);
}

/* The core's: a consistent copy of the plan, or 0 (none written yet, or the host was writing on each of a few tries: the core never waits for the host). */
static inline int mh2ipc_liveplan_read(Mh2IpcLivePlan *lp, Mh2IpcLivePlan *out)
{
    for (int tries = 0; tries < 4; tries++) {
        mh2ipc_u32 s = mh2ipc_load(&lp->seq);
        if ((s & 1u) || !s)
            continue;
        out->vblank = __atomic_load_n(&lp->vblank, __ATOMIC_RELAXED);
        out->n = __atomic_load_n(&lp->n, __ATOMIC_RELAXED);
        out->fb = __atomic_load_n(&lp->fb, __ATOMIC_RELAXED);
        for (mh2ipc_u32 i = 0; i < 8u; i++)
            out->alpha[i] = __atomic_load_n(&lp->alpha[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&lp->seq, __ATOMIC_RELAXED) == s) {
            out->seq = s;
            if (out->n > MH2IPC_LIVE_MAX)
                out->n = MH2IPC_LIVE_MAX;
            return 1;
        }
    }
    return 0;
}

/* ---- the keyboard's key events ---- */

/* The host's: queue one event; 0 when the ring is full (counted in dropped). */
static inline int mh2ipc_keys_push(Mh2IpcKeys *k, mh2ipc_u32 usage, int down)
{
    mh2ipc_u32 head = k->head;
    if (head - mh2ipc_load(&k->tail) >= MH2IPC_KEY_RING) {
        mh2ipc_store(&k->dropped, k->dropped + 1u);
        return 0;
    }
    __atomic_store_n(&k->events[head & (MH2IPC_KEY_RING - 1u)], (usage & 0xFFu) | (down ? MH2IPC_KEY_DOWN : 0u), __ATOMIC_RELAXED);
    mh2ipc_store(&k->head, head + 1u);
    return 1;
}

/* The core's: the next event without taking it (0 when none); take it with mh2ipc_keys_take. */
static inline int mh2ipc_keys_peek(Mh2IpcKeys *k, mh2ipc_u32 *event)
{
    mh2ipc_u32 tail = k->tail;
    if (mh2ipc_load(&k->head) == tail)
        return 0;
    *event = __atomic_load_n(&k->events[tail & (MH2IPC_KEY_RING - 1u)], __ATOMIC_RELAXED);
    return 1;
}

static inline void mh2ipc_keys_take(Mh2IpcKeys *k)
{
    mh2ipc_store(&k->tail, k->tail + 1u);
}

#endif
