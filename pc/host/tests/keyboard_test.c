/*
 * The PC keyboard as the game's USB keyboard (keyboard.c): the mapping of a key to its usage, the typing toggle, the
 * Japanese keys' bindings, the releases, and the ring's order. Window events are made by hand; no window, no SDL init.
 *   host_keyboard_test
 */
#include <stdio.h>
#include <string.h>

#include "mh2host.h"

static int g_typing_calls, g_typing_on;
static char g_typing_key[32];

void host_log(const char *fmt, ...)
{
    (void)fmt;
}

void present_set_typing(int on, const char *toggle_key)
{
    g_typing_calls++;
    g_typing_on = on;
    snprintf(g_typing_key, sizeof(g_typing_key), "%s", toggle_key);
}

static Mh2IpcKeys ring;
static int bad;

static void Check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "host_keyboard: %s\n", what);
        bad++;
    }
}

static int Press(int scancode, int down, int repeat)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = (SDL_Scancode)scancode;
    e.key.down = down != 0;
    e.key.repeat = repeat != 0;
    return kbd_event(&e);
}

/* The events in the ring since `from` (the core's tail is never moved here: the test reads head). */
static mh2ipc_u32 Ev(mh2ipc_u32 i)
{
    return ring.events[i & (MH2IPC_KEY_RING - 1u)];
}

#define DOWN(u) ((u) | MH2IPC_KEY_DOWN)

int main(void)
{
    SDL_Event focus;
    mh2ipc_u32 n;

    memset(&ring, 0, sizeof(ring));
    kbd_start(&ring);

    /* the pad's mode (the start): the game's keyboard sees nothing, and the keys are not taken */
    Check(!kbd_typing(), "starts in typing mode");
    Check(Press(SDL_SCANCODE_A, 1, 0) == 0 && Press(SDL_SCANCODE_A, 0, 0) == 0, "a key was taken in pad mode");
    Check(ring.head == 0, "a key reached the game in pad mode");

    /* the toggle: taken (down, up and repeat), once per press, never sent */
    Check(strcmp(kbd_toggle_name(), "ScrollLock") == 0 || strcmp(kbd_toggle_name(), "Scroll Lock") == 0, "the default toggle is not Scroll Lock");
    Check(Press(SDL_SCANCODE_SCROLLLOCK, 1, 0) == 1, "the toggle was not taken");
    Check(kbd_typing() && g_typing_on && g_typing_calls == 1, "the toggle did not start typing");
    Check(Press(SDL_SCANCODE_SCROLLLOCK, 1, 1) == 1 && g_typing_calls == 1, "the toggle's key repeat toggled again");
    Check(Press(SDL_SCANCODE_SCROLLLOCK, 0, 0) == 1 && kbd_typing(), "the toggle's release changed the mode");
    Check(ring.head == 0, "the toggle reached the game");

    /* letters, in order, down and up; the key repeat is the library's, not sent */
    Press(SDL_SCANCODE_M, 1, 0);
    Press(SDL_SCANCODE_M, 1, 1);
    Press(SDL_SCANCODE_O, 1, 0);
    Press(SDL_SCANCODE_M, 0, 0);
    Press(SDL_SCANCODE_O, 0, 0);
    Check(ring.head == 4, "the letters' events are not four");
    Check(Ev(0) == DOWN(0x10) && Ev(1) == DOWN(0x12) && Ev(2) == 0x10 && Ev(3) == 0x12, "the letters' usages or order");

    /* positional: SDL's scancode is the HID usage (Return 0x28, F6 0x3F, the arrows 0x4F-0x52, the modifiers 0xE0-0xE7) */
    n = ring.head;
    Press(SDL_SCANCODE_RETURN, 1, 0);
    Press(SDL_SCANCODE_F6, 1, 0);
    Press(SDL_SCANCODE_LEFT, 1, 0);
    Press(SDL_SCANCODE_LSHIFT, 1, 0);
    Press(SDL_SCANCODE_LCTRL, 1, 0);
    Check(Ev(n) == DOWN(0x28) && Ev(n + 1) == DOWN(0x3F) && Ev(n + 2) == DOWN(0x50) && Ev(n + 3) == DOWN(0xE1) &&
              Ev(n + 4) == DOWN(0xE0),
          "Return, F6, Left, Shift, Ctrl are not their usages");

    /* leaving the mode releases what is down, in one event (usage 0, up), and only then */
    n = ring.head;
    Press(SDL_SCANCODE_SCROLLLOCK, 1, 0);
    Check(!kbd_typing() && !g_typing_on, "the toggle did not leave typing");
    Check(ring.head == n + 1 && Ev(n) == 0, "leaving typing did not send every key up");
    Press(SDL_SCANCODE_SCROLLLOCK, 0, 0);
    Press(SDL_SCANCODE_RETURN, 0, 0);   /* the release of a key the game thinks is up already: nothing */
    Check(ring.head == n + 1, "a key's release reached the game after leaving typing");
    Press(SDL_SCANCODE_SCROLLLOCK, 1, 0);
    Press(SDL_SCANCODE_SCROLLLOCK, 0, 0);
    Check(kbd_typing(), "the second press did not start typing again");
    n = ring.head;
    Press(SDL_SCANCODE_SCROLLLOCK, 1, 0);
    Press(SDL_SCANCODE_SCROLLLOCK, 0, 0);
    Check(ring.head == n, "leaving typing with no key down sent an event");

    /* a key that was down before typing began is not the game's: its release is ignored */
    Press(SDL_SCANCODE_W, 1, 0);   /* pad mode */
    Press(SDL_SCANCODE_SCROLLLOCK, 1, 0);
    Press(SDL_SCANCODE_SCROLLLOCK, 0, 0);
    n = ring.head;
    Press(SDL_SCANCODE_W, 0, 0);
    Check(ring.head == n, "the release of a key held from pad mode reached the game");

    /* the window losing the focus releases the keys */
    Press(SDL_SCANCODE_B, 1, 0);
    n = ring.head;
    memset(&focus, 0, sizeof(focus));
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    kbd_event(&focus);
    Check(ring.head == n + 1 && Ev(n) == 0, "losing the focus did not release the keys");
    kbd_event(&focus);
    Check(ring.head == n + 1, "losing the focus again sent another release");

    /* the Japanese keys: the defaults, the bindings, none */
    n = ring.head;
    Press(SDL_SCANCODE_BACKSLASH, 1, 0);
    Press(SDL_SCANCODE_PAUSE, 1, 0);
    Press(SDL_SCANCODE_APPLICATION, 1, 0);
    Press(SDL_SCANCODE_RALT, 1, 0);
    Press(SDL_SCANCODE_GRAVE, 1, 0);
    Check(Ev(n) == DOWN(0x89) && Ev(n + 1) == DOWN(0x8A) && Ev(n + 2) == DOWN(0x8B) && Ev(n + 3) == DOWN(0x88) &&
              Ev(n + 4) == DOWN(0x35),
          "the default Yen, Henkan, Muhenkan, Kana and Hankaku/Zenkaku keys send their usages");
    Press(SDL_SCANCODE_SCROLLLOCK, 1, 0);   /* leave: everything up */
    Press(SDL_SCANCODE_SCROLLLOCK, 0, 0);
    Press(SDL_SCANCODE_SCROLLLOCK, 1, 0);
    Press(SDL_SCANCODE_SCROLLLOCK, 0, 0);
    Check(kbd_setting("usb_key_yen", "F1") == 0 && kbd_setting("usb_key_ro", "Insert") == 0, "a binding was refused");
    n = ring.head;
    Press(SDL_SCANCODE_F1, 1, 0);
    Press(SDL_SCANCODE_INSERT, 1, 0);
    Press(SDL_SCANCODE_BACKSLASH, 1, 0);
    Check(Ev(n) == DOWN(0x89) && Ev(n + 1) == DOWN(0x87) && Ev(n + 2) == DOWN(0x31),
          "a rebound Yen and Ro, and the Backslash key on its own usage");
    Check(kbd_setting("usb_key_henkan", "none") == 0, "none was refused");
    n = ring.head;
    Press(SDL_SCANCODE_PAUSE, 1, 0);
    Check(Ev(n) == DOWN(0x48), "an unbound Henkan key lost its own usage");
    Check(kbd_setting("usb_key_henkan", "Nonsense Key") == -2, "a bad key name was taken");
    Check(kbd_setting("usb_key_nothing", "F2") == -1 && kbd_setting("usb_nothing", "F2") == -1, "an unknown setting was taken");
    Check(kbd_setting("usb_keyboard_toggle", "F9") == 0, "the toggle was refused a key");
    Check(strcmp(kbd_toggle_name(), "F9") == 0, "the toggle is not F9");
    Check(kbd_toggle_scancode() == SDL_SCANCODE_F9, "the toggle's scancode");
    n = ring.head;
    Check(Press(SDL_SCANCODE_SCROLLLOCK, 1, 0) == 0 && Ev(n) == DOWN(0x47), "Scroll Lock is not an ordinary key once the toggle moved");
    Check(Press(SDL_SCANCODE_F9, 1, 0) == 1 && !kbd_typing(), "the moved toggle did not leave typing");

    /* a usage beyond the keyboard page is not sent */
    Press(SDL_SCANCODE_F9, 0, 0);
    Press(SDL_SCANCODE_F9, 1, 0);
    Press(SDL_SCANCODE_F9, 0, 0);
    n = ring.head;
    Check(Press(0x120, 1, 0) == 0 && ring.head == n, "a scancode beyond 0xE7 reached the game");

    /* a full ring drops the event and counts it; a release that does not fit is made good by kbd_update */
    memset(&ring, 0, sizeof(ring));
    kbd_start(&ring);
    kbd_setting("usb_keyboard_toggle", "ScrollLock");
    Press(SDL_SCANCODE_SCROLLLOCK, 1, 0);
    Press(SDL_SCANCODE_SCROLLLOCK, 0, 0);
    ring.head = MH2IPC_KEY_RING - 1u;   /* 255 events the core has not read */
    Press(SDL_SCANCODE_X, 1, 0);        /* takes the last slot */
    Check(ring.head - ring.tail == MH2IPC_KEY_RING, "the ring is not full");
    Press(SDL_SCANCODE_X, 0, 0);        /* does not fit */
    Check(ring.head - ring.tail == MH2IPC_KEY_RING, "a release was written into a full ring");
    ring.tail += 8;
    kbd_update();
    Check(ring.head - ring.tail == MH2IPC_KEY_RING - 8 + 1 && Ev(ring.head - 1) == 0, "kbd_update did not send every key up once there was room");
    n = ring.head;
    kbd_update();
    Check(ring.head == n, "kbd_update sent it twice");

    if (bad)
        return 1;
    printf("host_keyboard: ok\n");
    return 0;
}
