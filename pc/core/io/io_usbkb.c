/* The IOP's USB keyboard service (0x80000211), and the keyboard it serves.
 *
 * Without a keyboard (the default: neither the host nor --usb-keyboard says so) every
 * request's answer is zeros, as the original's stub: sceUsbKbInit finds none and
 * deletes its semaphore, and the game's keyboard input is the pad's software keyboard.
 *
 * With one connected, the
 * service answers as the original's model of a PS2 with a USB keyboard (iop_usbkb.c, the same
 * protocol; libusbkb):
 *   fno 1  GetInfo     answer (0x90 bytes): byte 0 the slots (the connected flags that follow), byte 1 the
 *                      connected keyboards, bytes 2.. a flag a slot: 1, 1, 1.
 *   fno 2  Read        request byte 0 the port; answer (0x50): byte 0 the LEDs, byte 1 the report's length
 *                      (0: none waiting; 8), byte 2 the modifier bits, byte 3 0, bytes 4.. the six keys.
 *   fno 4  SetLEDStatus, fno 5 SetLEDMode: request byte 0 the port, byte 1 the value; the answer echoes byte 1.
 *   fno 6  ClearRbuf   the reports waiting are dropped.
 * The keyboard is a USB boot-protocol keyboard. Its report (modifier bits, 0, six usage IDs) of a VBlank where it
 * changed waits in a queue of 32, as a keyboard's 125 Hz reports wait in the IOP; a Read takes the oldest. In the
 * automatic LED modes (0 and 1) a Caps Lock, Num Lock or Scroll Lock key going down toggles its LED; in the
 * manual mode (2) only SetLEDStatus changes them. The game uses mode 0.
 *
 * Where the reports come from, at the start of each VBlank (Io_KbVBlank, with the pads' step): the host's key
 * events, which make the report (the keys down longest first, six at most; more than six is the phantom state, six
 * times usage 1, as a keyboard sends it).
 * A report is queued only when it differs from the last one.
 *
 * Without a keyboard, the host's event ring is emptied unread. */
#include "io/io.h"
#include "io/io_kernel.h"

#define KB_QUEUE 32
#define KB_KEYS_DOWN 16      /* the keys the model keeps down (a keyboard has 6 in a report; the rest is the phantom state) */
#define KB_LED_NUM 1
#define KB_LED_CAPS 2
#define KB_LED_SCROLL 4

/* The host asks for a keyboard (ipc_glue.c sets it; unset: no). */
s32 (*Io_KbHostWants)(void);

static struct {
    s32 option;            /* --usb-keyboard */
    s32 connected;
    u8 cur[8];             /* the report in force */
    u8 queue[KB_QUEUE][8];
    s32 head, count;
    u8 leds;
    s32 ledMode;
    u8 down[KB_KEYS_DOWN]; /* the keys down, the oldest first (the modifiers are in mods) */
    s32 ndown;
    u8 mods;
    u8 touched[256];       /* usages an event of this VBlank changed: a second event on the same key waits */
} g_kb;

s32 Io_KbOption(s32 argc, char **argv, s32 i)
{
    (void)argc;
    const char *a = argv[i], *b = "--usb-keyboard";
    while (*a && *a == *b) {
        a++;
        b++;
    }
    if (*a || *b)
        return 0;
    g_kb.option = 1;
    return 1;
}

s32 Io_KbConnected(void)
{
    return g_kb.connected;
}

static void Push(const u8 *r)
{
    if (g_kb.count == KB_QUEUE) {   /* a keyboard nobody reads: the oldest is lost */
        g_kb.head = (g_kb.head + 1) % KB_QUEUE;
        g_kb.count--;
    }
    __builtin_memcpy(g_kb.queue[(g_kb.head + g_kb.count) % KB_QUEUE], r, 8);
    g_kb.count++;
}

static s32 HasKey(const u8 *r, u8 usage)
{
    for (s32 i = 2; i < 8; i++)
        if (r[i] == usage)
            return 1;
    return 0;
}

static void Service(IopRpcCall *c)
{
    u8 *r = c->reply;
    const u8 *q = c->data;
    if (!g_kb.connected)
        return;   /* the reply stays zero-filled */
    switch (c->fno) {
    case 1:
        if (c->recv_size >= 3) {
            r[0] = 1;
            r[1] = 1;
            r[2] = 1;
        }
        return;
    case 2:
        if (c->recv_size < 10)
            return;
        r[0] = g_kb.leds;
        if (g_kb.count) {
            const u8 *rep = g_kb.queue[g_kb.head];
            g_kb.head = (g_kb.head + 1) % KB_QUEUE;
            g_kb.count--;
            r[1] = 8;
            r[2] = rep[0];
            __builtin_memcpy(r + 4, rep + 2, 6);
        }
        return;
    case 4:
    case 5:
        if (c->size >= 2 && c->recv_size >= 2) {
            if (c->fno == 4)
                g_kb.leds = q[1] & 7;
            else
                g_kb.ledMode = q[1];
            r[1] = q[1];
        }
        return;
    case 6:
        g_kb.head = g_kb.count = 0;
        return;
    default:
        return;
    }
}

void IoKb_Register(void)
{
    const char *want = g_kb.option || (Io_KbHostWants && Io_KbHostWants()) ? "1" : "0";
    const char *use = want;
    __builtin_memset(&g_kb.connected, 0, sizeof(g_kb) - __builtin_offsetof(typeof(g_kb), connected));
    g_kb.connected = use[0] == '1';
    Iop_RegisterService(0x80000211u, "usb 211", Service);
}

/* One report for this VBlank, queued for the service if it changed. */
void Io_KbSetReport(const u8 *report, u32 bytes)
{
    u8 r[8];
    if (!g_kb.connected)
        return;
    __builtin_memset(r, 0, sizeof(r));
    if (bytes > sizeof(r))
        bytes = sizeof(r);
    __builtin_memcpy(r, report, bytes);
    if (!__builtin_memcmp(r, g_kb.cur, sizeof(r)))
        return;
    if (g_kb.ledMode != 2) {
        static const struct { u8 usage, led; } lock[3] = {{0x53, KB_LED_NUM}, {0x39, KB_LED_CAPS}, {0x47, KB_LED_SCROLL}};
        for (s32 i = 0; i < 3; i++)
            if (HasKey(r, lock[i].usage) && !HasKey(g_kb.cur, lock[i].usage))
                g_kb.leds ^= lock[i].led;
    }
    __builtin_memcpy(g_kb.cur, r, sizeof(r));
    Push(r);
}

/* A key event of the host (the usage ID of the keyboard page; usage 0 up: every key up). 0 when it must wait
 * for the next VBlank: the key's state was changed already in this one, and a key that goes down and up inside
 * a VBlank would never be seen. */
static s32 KeyEvent(u8 usage, s32 down)
{
    s32 i;
    if (usage == 0) {
        g_kb.ndown = 0;
        g_kb.mods = 0;
        return 1;
    }
    if (g_kb.touched[usage])
        return 0;
    g_kb.touched[usage] = 1;
    if (usage >= 0xE0 && usage <= 0xE7) {
        if (down)
            g_kb.mods |= (u8)(1u << (usage - 0xE0));
        else
            g_kb.mods &= (u8) ~(1u << (usage - 0xE0));
        return 1;
    }
    for (i = 0; i < g_kb.ndown && g_kb.down[i] != usage; i++)
        ;
    if (down && i == g_kb.ndown && g_kb.ndown < KB_KEYS_DOWN) {
        g_kb.down[g_kb.ndown++] = usage;
    } else if (!down && i < g_kb.ndown) {
        for (; i + 1 < g_kb.ndown; i++)
            g_kb.down[i] = g_kb.down[i + 1];
        g_kb.ndown--;
    }
    return 1;
}

/* At each VBlank's start (the glue, with the pads): the host's events, then this VBlank's report. peek and take
 * are the event ring (NULL: none); not connected, the ring is emptied unread. */
void Io_KbVBlank(s32 (*peek)(u32 *event), void (*take)(void))
{
    u8 r[8];
    u32 ev;
    __builtin_memset(g_kb.touched, 0, sizeof(g_kb.touched));
    while (peek && take && peek(&ev)) {
        if (g_kb.connected && !KeyEvent((u8)(ev & 0xFF), (ev & 0x100) != 0))
            break;
        take();
    }
    if (!g_kb.connected)
        return;
    __builtin_memset(r, 0, sizeof(r));
    r[0] = g_kb.mods;
    if (g_kb.ndown > 6) {
        __builtin_memset(r + 2, 1, 6);   /* ErrorRollOver, as a keyboard sends it */
    } else {
        for (s32 i = 0; i < g_kb.ndown; i++)
            r[2 + i] = g_kb.down[i];
    }
    Io_KbSetReport(r, sizeof(r));
}
