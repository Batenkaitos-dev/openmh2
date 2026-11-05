/*
 * The input (from the original's platform_sdl.c): one DualShock
 * 2 on port 0, from the first SDL gamepad (by button position: south =
 * Cross, east = Circle, west = Square, north = Triangle; the analog
 * triggers are L2 and R2) and from the keyboard (PCSX2's default layout:
 * arrows = D-pad, K = Cross, L = Circle, J = Square, I = Triangle, Q/E =
 * L1/R1, 1/3 = L2/R2, 2/4 = L3/R3, Return = START, Backspace = SELECT,
 * W/A/S/D = left stick, T/F/G/H = right stick). The USB keyboard (SDL's
 * scancodes are HID usage IDs) is not handed over: disconnected. The main thread writes
 * the state into the mailbox when it changes; the core samples it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

enum {
    PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3,
    PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};
enum { PADB_NONE = -1, PADB_TRIGGER_L = 1000, PADB_TRIGGER_R = 1001 };

typedef struct Binding {
    const char *name;
    uint16_t bit;
    SDL_Scancode key;
    int pad;         /* an SDL_GamepadButton, a trigger, or PADB_NONE */
    int pressure;    /* the button's index in the pressures, or -1 */
} Binding;

/* the bindings: settings.c changes them (input_bind) before input_init */
static Binding k_bind[] = {
    {"up", PAD_UP, SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP, 2},
    {"down", PAD_DOWN, SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 3},
    {"left", PAD_LEFT, SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1},
    {"right", PAD_RIGHT, SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0},
    {"cross", PAD_CROSS, SDL_SCANCODE_K, SDL_GAMEPAD_BUTTON_SOUTH, 6},
    {"circle", PAD_CIRCLE, SDL_SCANCODE_L, SDL_GAMEPAD_BUTTON_EAST, 5},
    {"square", PAD_SQUARE, SDL_SCANCODE_J, SDL_GAMEPAD_BUTTON_WEST, 7},
    {"triangle", PAD_TRIANGLE, SDL_SCANCODE_I, SDL_GAMEPAD_BUTTON_NORTH, 4},
    {"l1", PAD_L1, SDL_SCANCODE_Q, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 8},
    {"r1", PAD_R1, SDL_SCANCODE_E, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 9},
    {"l2", PAD_L2, SDL_SCANCODE_1, PADB_TRIGGER_L, 10},
    {"r2", PAD_R2, SDL_SCANCODE_3, PADB_TRIGGER_R, 11},
    {"l3", PAD_L3, SDL_SCANCODE_2, SDL_GAMEPAD_BUTTON_LEFT_STICK, -1},
    {"r3", PAD_R3, SDL_SCANCODE_4, SDL_GAMEPAD_BUTTON_RIGHT_STICK, -1},
    {"start", PAD_START, SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START, -1},
    {"select", PAD_SELECT, SDL_SCANCODE_BACKSPACE, SDL_GAMEPAD_BUTTON_BACK, -1},
};
/* the keyboard's sticks: up, down, left, right of the left stick, then the right */
static SDL_Scancode k_stick[8] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
                                        SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H};

static const char *const k_stick_name[8] = {"lstick_up", "lstick_down", "lstick_left", "lstick_right",
                                             "rstick_up", "rstick_down", "rstick_left", "rstick_right"};

/* A binding from mh2.ini (settings.c), as the original's: KEY is "key.NAME" or
 * "pad.NAME", NAME a button (k_bind's names) or, for keys, a stick's direction
 * (k_stick_name); VALUE an SDL key name, or a gamepad button's name (SDL's:
 * a b x y, south east west north, start, back, leftshoulder, dpup, ...),
 * lefttrigger or righttrigger; "none" unbinds. 0, -1 (no such button) or -2
 * (no such key or gamepad button). */
int input_bind(const char *key, const char *value)
{
    int pad = key[0] == 'p', none = !SDL_strcasecmp(value, "none");
    const char *name = key + 4;
    Binding *b = NULL;
    SDL_Scancode *stick = NULL;
    for (size_t i = 0; i < sizeof(k_bind) / sizeof(k_bind[0]); i++)
        if (!SDL_strcasecmp(name, k_bind[i].name))
            b = &k_bind[i];
    for (int i = 0; i < 8 && !pad; i++)
        if (!SDL_strcasecmp(name, k_stick_name[i]))
            stick = &k_stick[i];
    if (!b && !stick)
        return -1;
    if (!pad) {
        SDL_Scancode sc = none ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(value);
        if (!none && sc == SDL_SCANCODE_UNKNOWN)
            return -2;
        if (b)
            b->key = sc;
        else
            *stick = sc;
        return 0;
    }
    if (none) {
        b->pad = PADB_NONE;
    } else if (!SDL_strcasecmp(value, "lefttrigger")) {
        b->pad = PADB_TRIGGER_L;
    } else if (!SDL_strcasecmp(value, "righttrigger")) {
        b->pad = PADB_TRIGGER_R;
    } else {
        SDL_GamepadButton g = SDL_GetGamepadButtonFromString(value);
        if (g == SDL_GAMEPAD_BUTTON_INVALID)
            return -2;
        b->pad = (int)g;
    }
    return 0;
}

int input_button(const char *name, int *key, int *pad)
{
    for (size_t i = 0; i < sizeof(k_bind) / sizeof(k_bind[0]); i++)
        if (!SDL_strcasecmp(name, k_bind[i].name)) {
            *key = (int)k_bind[i].key;
            *pad = k_bind[i].pad >= 0 && k_bind[i].pad < 1000 ? k_bind[i].pad : -1;
            return 0;
        }
    return -1;
}

static struct {
    SDL_Gamepad *gamepad;
    Mh2IpcPad pads[MH2IPC_PADS];
    Mh2IpcKeyboard kb;
    int written;
    unsigned int rumble;
    Uint64 rumble_at;
} in;



static void open_first_gamepad(void)
{
    if (in.gamepad)
        return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        in.gamepad = SDL_OpenGamepad(ids[0]);
        if (in.gamepad)
            host_log("gamepad: %s", SDL_GetGamepadName(in.gamepad));
    }
    SDL_free(ids);
}

void input_init(void)
{
    open_first_gamepad();
}

SDL_Gamepad *input_gamepad(void)
{
    return in.gamepad;
}

void input_event(const SDL_Event *e)
{
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        open_first_gamepad();
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED && in.gamepad && SDL_GetGamepadID(in.gamepad) == e->gdevice.which) {
        SDL_CloseGamepad(in.gamepad);
        in.gamepad = NULL;
        open_first_gamepad();
    }
}

static uint8_t axis_to_u8(Sint16 v)
{
    int x = ((int)v + 32768) >> 8;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

static uint8_t stick_keys(const bool *k, SDL_Scancode neg, SDL_Scancode pos)
{
    return k[neg] && !k[pos] ? 0x00 : k[pos] && !k[neg] ? 0xFF : 0x80;
}

static void read_pad(Mh2IpcPad *out, const bool *k)
{
    memset(out, 0, sizeof(*out));
    out->connected = 1;
    out->rx = out->ry = out->lx = out->ly = 0x80;
    if (k) {
        for (size_t i = 0; i < sizeof(k_bind) / sizeof(k_bind[0]); i++)
            if (k[k_bind[i].key])
                out->buttons |= k_bind[i].bit;
        if (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT])
            out->buttons &= (uint16_t)~PAD_START;   /* Alt+Enter is fullscreen, not START */
        out->ly = stick_keys(k, k_stick[0], k_stick[1]);
        out->lx = stick_keys(k, k_stick[2], k_stick[3]);
        out->ry = stick_keys(k, k_stick[4], k_stick[5]);
        out->rx = stick_keys(k, k_stick[6], k_stick[7]);
    }
    if (in.gamepad) {
        Sint16 trig[2] = {SDL_GetGamepadAxis(in.gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                          SDL_GetGamepadAxis(in.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)};
        for (size_t i = 0; i < sizeof(k_bind) / sizeof(k_bind[0]); i++) {
            int p = k_bind[i].pad;
            if (p == PADB_TRIGGER_L || p == PADB_TRIGGER_R) {
                Sint16 t = trig[p - PADB_TRIGGER_L];
                if (t > 8000) {
                    out->buttons |= k_bind[i].bit;
                    if (k_bind[i].pressure >= 0)
                        out->pressure[k_bind[i].pressure] = (uint8_t)(t >> 7);
                }
            } else if (p >= 0 && SDL_GetGamepadButton(in.gamepad, (SDL_GamepadButton)p)) {
                out->buttons |= k_bind[i].bit;
            }
        }
        Sint16 ax[4] = {SDL_GetGamepadAxis(in.gamepad, SDL_GAMEPAD_AXIS_RIGHTX),
                        SDL_GetGamepadAxis(in.gamepad, SDL_GAMEPAD_AXIS_RIGHTY),
                        SDL_GetGamepadAxis(in.gamepad, SDL_GAMEPAD_AXIS_LEFTX),
                        SDL_GetGamepadAxis(in.gamepad, SDL_GAMEPAD_AXIS_LEFTY)};
        uint8_t *dst[4] = {&out->rx, &out->ry, &out->lx, &out->ly};
        for (int i = 0; i < 4; i++)
            if (ax[i] < -4000 || ax[i] > 4000)   /* the keyboard's sticks win inside the dead zone */
                *dst[i] = axis_to_u8(ax[i]);
    }
    /* digital buttons press fully */
    static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                       PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    for (int i = 0; i < 12; i++)
        if (!out->pressure[i])
            out->pressure[i] = (out->buttons & order[i]) ? 0xFF : 0x00;
}

static __attribute__((unused)) void read_keyboard(Mh2IpcKeyboard *out, const bool *k, int numkeys)
{
    memset(out, 0, sizeof(*out));
    out->connected = 1;
    for (int u = 4; u < numkeys && u < 256; u++) {
        if (!k[u])
            continue;
        if (u >= 0xE0 && u <= 0xE7)
            out->modifiers |= 1u << (u - 0xE0);
        else
            out->keys[u >> 5] |= 1u << (u & 31);
    }
}

void input_update(void)
{
    int numkeys = 0;
    const bool *k = SDL_GetKeyboardState(&numkeys);
    Mh2IpcPad pads[MH2IPC_PADS];
    Mh2IpcKeyboard kb;
    memset(pads, 0, sizeof(pads));
    pads[1].rx = pads[1].ry = pads[1].lx = pads[1].ly = 0x80;
    /* typing: the keyboard's keys are the game's keyboard's, a gamepad still plays; the freecam in control: the keyboard is
     * its own (freecam.c) */
    read_pad(&pads[0], kbd_typing() || freecam_controls() ? NULL : k);
    freecam_pad_filter(&pads[0].buttons);   /* L3 and R3 together are the freecam's, not the game's */
    if (freecam_controls() && !freecam_game_pad()) {   /* the freecam has control and the game's input is neutral */
        memset(&pads[0], 0, sizeof(pads[0]));
        pads[0].connected = 1;
        pads[0].rx = pads[0].ry = pads[0].lx = pads[0].ly = 0x80;
    }
    /* The original game's keyboard slot (Mh2IpcKeyboard: the keyboard as a pad) is never handed over: the game's USB
     * keyboard, when connected (usb_keyboard), comes through the key ring (keyboard.c). */
    memset(&kb, 0, sizeof(kb));
    (void)numkeys;
    if (!in.written || memcmp(pads, in.pads, sizeof(pads)) || memcmp(&kb, &in.kb, sizeof(kb))) {
        memcpy(in.pads, pads, sizeof(pads));
        in.kb = kb;
        in.written = 1;
        mh2ipc_input_write(&g_ipc->input, pads, &kb);
    }
    /* the DualShock 2's small motor is on/off and high-pitched, the large one variable and low */
    /* (renewed while on: a host's rumble has a duration) */
    unsigned int r = mh2ipc_load(&g_ipc->rumble[0]);
    Uint64 now = SDL_GetTicks();
    if (in.gamepad && (r != in.rumble || (r && now - in.rumble_at > 1000))) {
        in.rumble_at = now;
        Uint16 low = (Uint16)(((r >> 8) & 0xFF) * 257u), high = (r & 1) ? 0xFFFF : 0;
        SDL_RumbleGamepad(in.gamepad, low, high, (low || high) ? 1500 : 0);
    }
    in.rumble = r;
}

void input_shutdown(void)
{
    if (in.gamepad)
        SDL_CloseGamepad(in.gamepad);
    in.gamepad = NULL;
}
