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
#define MH2IPC_VERSION 6u
/* The release build (MH2_RELEASE) leaves out some of the host's features, so its header lacks the development
 * build's two blocks (Mh2IpcPlay, Mh2IpcWatch) and the words that answer them. Every other field is at the same
 * offset in both, and the blocks that only the development build has are last (or in reserved space): the release
 * header is a prefix of the development one. The same version number serves both. A release core runs under a
 * development host. The header's size (header_bytes) is the same constant
 * in both, and the core refuses a smaller one than its own. */
#define MH2IPC_ENV "MH2_IPC"         /* the block's name, in mh2-core's environment */

/* The block's parts, in this order. */
#define MH2IPC_HEADER_BYTES 0x10000u
#define MH2IPC_GS_BYTES (32u << 20)        /* about ten frames of the busiest scenes */
#define MH2IPC_AUDIO_BYTES (256u << 10)    /* 65536 frames, 1.37 s */
#define MH2IPC_RB_BYTES (4u << 20)         /* the GS's whole memory */
#define MH2IPC_TOTAL_BYTES (MH2IPC_HEADER_BYTES + MH2IPC_GS_BYTES + MH2IPC_AUDIO_BYTES + MH2IPC_RB_BYTES)

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
};

/* The events one side waits on (mh2ipc_os.h). */
enum {
    MH2IPC_EV_GS_DATA = 0,    /* the host waits for GS records (word: gs.head) */
    MH2IPC_EV_GS_SPACE = 1,   /* the core waits for room in the GS ring (word: gs.tail) */
    MH2IPC_EV_RB_DONE = 2,    /* the core waits for a readback (word: rb_done_seq) */
    MH2IPC_EV_CONTROL = 3,    /* the host waits for the core's state (word: core_state) */
    MH2IPC_EV_SCHED = 4,      /* the core waits for the pad schedule (word: sched.horizon) */
    MH2IPC_EV_COUNT = 5
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
};

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
    mh2ipc_u8 reserved[2];
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
 *    in 32 bits. */
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
    mh2ipc_u32 reserved1[14];
    mh2ipc_u32 tail;           /* the core's: entries taken */
    mh2ipc_u32 waiting;        /* the core sleeps on horizon */
    mh2ipc_u32 reserved2[14];
    Mh2IpcSchedEntry entries[MH2IPC_SCHED_RING];
} Mh2IpcSched;

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
    mh2ipc_u32 reserved1[9];
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
    mh2ipc_u32 reserved2[7];
    /* 0x0C0: host -> core */
    Mh2IpcInput input;
    mh2ipc_u32 reserved3[(0x200 - 0x0C0 - sizeof(Mh2IpcInput)) / 4];
    /* 0x200: core -> host, with core_state = FATAL */
    char fatal_message[512];
    /* 0x400 */
    Mh2IpcRing gs;
    Mh2IpcRing audio;
    /* 0x580: host -> core, the USB keyboard's key events */
    Mh2IpcKeys keys;
    /* host -> core, a scripted run's pad states by VBlank (the same in both builds) */
    Mh2IpcSched sched;
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
MH2IPC_ASSERT(sizeof(Mh2IpcKeys) == 128 + 4 * MH2IPC_KEY_RING && __builtin_offsetof(Mh2IpcKeys, tail) == 64 && __builtin_offsetof(Mh2IpcKeys, events) == 128, "Mh2IpcKeys");
MH2IPC_ASSERT(sizeof(Mh2IpcHeader) == 0x400 + 2 * 192 + sizeof(Mh2IpcKeys) + sizeof(Mh2IpcSched), "Mh2IpcHeader");
MH2IPC_ASSERT(sizeof(Mh2IpcHeader) <= MH2IPC_HEADER_BYTES, "Mh2IpcHeader");

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
    h->host_state = MH2IPC_STATE_RUNNING;
    mh2ipc_fence();
    mh2ipc_store(&h->magic, MH2IPC_MAGIC);
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
