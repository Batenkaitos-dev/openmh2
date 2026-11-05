/*
 * The PC keyboard as the game's USB keyboard (usb_keyboard = on, --usb-keyboard; off unless you ask).
 *
 * The game reads a USB keyboard on the PS2 (the text entry of the name and the chat: letters, kana typed as romaji,
 * conversion). With the keyboard connected, the window's key presses reach it, key by key, as the USB usage the key
 * has: SDL's scancodes are the keyboard page's usage IDs (0x04-0x1D the letters, 0x28 Return, ..., 0xE0-0xE7 the
 * modifiers), so the capture is by the key's position and any PC keyboard layout works (the game's own keyboard was a
 * Japanese one; a US or other layout types its letters in the same places).
 *
 * The keyboard is also the pad's keyboard (input.c's bindings: arrows, I J K L, WASD ...). The two never share a
 * key press: the keyboard is in one of two modes, and the toggle key (Scroll Lock) changes it.
 *   - pad (the start): the keys drive the pad, as without a USB keyboard; the game's keyboard sees no key.
 *   - typing: the keys go to the game's keyboard; the pad's keyboard bindings are idle (a gamepad still works).
 * The window's title and a line over the picture say "typing" while it is on. Leaving the mode (and the window
 * losing the focus) releases every key the game was told is down.
 *
 * The Japanese keyboard's extra keys have no key on other layouts. A binding (usb_key_NAME = KEY in mh2.ini, `none`
 * to unbind) says which key of this keyboard sends it; that key then sends the Japanese key's usage instead of its own:
 *   yen        0x89  the key above Enter (default: Backslash, the same place)
 *   ro         0x87  (default: none)
 *   henkan     0x8A  convert (default: Pause)
 *   muhenkan   0x8B  do not convert (default: Application, the menu key)
 *   kana       0x88  Katakana/Hiragana (default: Right Alt)
 *   zenkaku    0x35  Hankaku/Zenkaku: on a Japanese keyboard the key under Escape sends the same usage as the
 *                    Grave key of other layouts, so the default (Grave) changes nothing; bind another key to type it
 * The events wait in the IPC's key ring (Mh2IpcKeys) for the core, which takes them at its VBlank.
 */
#include <string.h>

#include "mh2host.h"

typedef struct JisKey {
    const char *name;
    uint8_t usage;
    SDL_Scancode key;
} JisKey;

/* the toggle and the bindings: settings.c changes them (kbd_setting) before kbd_start */
static SDL_Scancode k_toggle = SDL_SCANCODE_SCROLLLOCK;
static JisKey k_jis[] = {
    {"yen", 0x89, SDL_SCANCODE_BACKSLASH},
    {"ro", 0x87, SDL_SCANCODE_UNKNOWN},
    {"henkan", 0x8A, SDL_SCANCODE_PAUSE},
    {"muhenkan", 0x8B, SDL_SCANCODE_APPLICATION},
    {"kana", 0x88, SDL_SCANCODE_RALT},
    {"zenkaku", 0x35, SDL_SCANCODE_GRAVE},
};

static struct {
    Mh2IpcKeys *ring;
    int typing;
    uint8_t held[256];   /* the usages the game was told are down */
    int lost;            /* a release did not fit in a full ring: "every key up" is owed */
} kb;

/* usb_keyboard_toggle = KEY, usb_key_NAME = KEY (KEY: an SDL key name, or none). 0; -1: no such setting; -2: no such key. */
int kbd_setting(const char *key, const char *value)
{
    int none = !SDL_strcasecmp(value, "none");
    SDL_Scancode sc = none ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(value);
    if (!none && sc == SDL_SCANCODE_UNKNOWN)
        return -2;
    if (!strcmp(key, "usb_keyboard_toggle")) {
        k_toggle = sc;
        return 0;
    }
    if (strncmp(key, "usb_key_", 8))
        return -1;
    for (size_t i = 0; i < sizeof(k_jis) / sizeof(k_jis[0]); i++)
        if (!SDL_strcasecmp(key + 8, k_jis[i].name)) {
            k_jis[i].key = sc;
            return 0;
        }
    return -1;
}

/* The key bound by usb_keyboard_toggle or usb_key_NAME (the setting's key), as SDL names it, or "none". */
const char *kbd_key_name(const char *key)
{
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    if (!strcmp(key, "usb_keyboard_toggle"))
        sc = k_toggle;
    else if (!strncmp(key, "usb_key_", 8))
        for (size_t i = 0; i < sizeof(k_jis) / sizeof(k_jis[0]); i++)
            if (!SDL_strcasecmp(key + 8, k_jis[i].name))
                sc = k_jis[i].key;
    const char *n = sc == SDL_SCANCODE_UNKNOWN ? NULL : SDL_GetScancodeName(sc);
    return n && *n ? n : "none";
}

const char *kbd_toggle_name(void)
{
    const char *n = k_toggle == SDL_SCANCODE_UNKNOWN ? NULL : SDL_GetScancodeName(k_toggle);
    return n && *n ? n : "none";
}

void kbd_start(Mh2IpcKeys *ring)
{
    memset(&kb, 0, sizeof(kb));
    kb.ring = ring;
}

int kbd_typing(void)
{
    return kb.typing;
}

int kbd_toggle_scancode(void)
{
    return (int)k_toggle;
}

/* The usage a key sends, or 0 for a key that has none (not a keyboard-page usage). */
static int usage_of(SDL_Scancode sc)
{
    for (size_t i = 0; i < sizeof(k_jis) / sizeof(k_jis[0]); i++)
        if (k_jis[i].key != SDL_SCANCODE_UNKNOWN && k_jis[i].key == sc)
            return k_jis[i].usage;
    return (int)sc >= 4 && (int)sc <= 0xE7 ? (int)sc : 0;
}

static void release_all(void)
{
    int any = kb.lost;
    for (int u = 0; u < 256; u++)
        any |= kb.held[u];
    memset(kb.held, 0, sizeof(kb.held));
    kb.lost = 0;
    if (any && !mh2ipc_keys_push(kb.ring, 0, 0))   /* usage 0, up: every key up */
        kb.lost = 1;
}

/* Once a loop of the window: a release that found the ring full is made good when there is room. */
void kbd_update(void)
{
    if (kb.ring && kb.lost)
        release_all();
}

static void set_typing(int on)
{
    if (kb.typing == on)
        return;
    kb.typing = on;
    if (!on)
        release_all();
    present_set_typing(on, kbd_toggle_name());
    host_log(on ? "USB keyboard: typing (the keys go to the game; the pad's keyboard keys are off; %s plays again)"
                : "USB keyboard: pad (the keys drive the pad; %s types again)", kbd_toggle_name());
}


/* A window event. 1 when the keyboard took it for itself (the toggle); the caller passes every event on to the pad's
 * input as before, which reads the key state and ignores this one: while typing, input.c's keyboard bindings are idle. */
int kbd_event(const SDL_Event *e)
{
    if (!kb.ring)
        return 0;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        release_all();
        return 0;
    }
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP)
        return 0;
    if (e->key.repeat)   /* the library repeats a held key itself */
        return e->key.scancode == k_toggle;
    if (e->key.scancode == k_toggle && k_toggle != SDL_SCANCODE_UNKNOWN) {
        if (e->type == SDL_EVENT_KEY_DOWN)
            set_typing(!kb.typing);
        return 1;
    }
    if (!kb.typing)
        return 0;
    int u = usage_of(e->key.scancode);
    if (!u)
        return 0;
    if (e->type == SDL_EVENT_KEY_DOWN) {
        if (kb.held[u])
            return 0;
        if (mh2ipc_keys_push(kb.ring, (mh2ipc_u32)u, 1))
            kb.held[u] = 1;
    } else if (kb.held[u]) {
        kb.held[u] = 0;
        if (!mh2ipc_keys_push(kb.ring, (mh2ipc_u32)u, 0))   /* a full ring: every key up follows when there is room */
            kb.lost = 1;
    }
    return 0;
}
