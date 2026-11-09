/*
 * The freecam, the host's side: the controller. The core's seam (pc/core/pal_freecam.c) draws the world from a pose the host
 * gives it; this file keeps the pose, moves it from the keys, the mouse and a gamepad, and writes it to the IPC's freecam block
 * (Mh2IpcFreecam) for the core to read at each VBlank's start. Back comes the game's own camera (cam_*), where the freecam
 * starts and where R returns it.
 *
 * Keys (settings freecam_key..., rebindable; "none" unbinds): F9 toggles, W S A D move along the view and across it, Q E go
 * down and up, Z X change the field of view, R puts the pose back on the game's camera, Tab hands the pad to the game and
 * back (hold), Escape leaves. Shift is fast, Ctrl slow; the wheel changes the speed (the setting freecam_speed is the
 * speed in game units a second at the wheel's middle). The mouse looks while the freecam has control (relative mode).
 * A gamepad: the left stick moves, the right stick looks, L2 and R2 go down and up, L1 and R1 change the speed.
 *
 * While the freecam has control the game's input is neutral (freecam_game_input = neutral, the default): the keyboard
 * and the gamepad belong to the freecam and the game is handed a pad with nothing pressed. With freecam_game_input = pad
 * the keyboard and the mouse are the freecam's and the gamepad still plays. The keys are off while the keyboard types
 * (keyboard.c): the freecam holds, as for Tab.
 *
 * The pose changes with the real time that passed (frame-rate independent).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

#define PI_F 3.14159265358979f

enum { FK_TOGGLE, FK_FWD, FK_BACK, FK_LEFT, FK_RIGHT, FK_DOWN, FK_UP, FK_RESET, FK_FOVNARROW, FK_FOVWIDE, FK_HOLD, FK_PAUSE, FK_STEP,
       FK_PHOTO, FK_HIDE, FK_COUNT };

static const struct { const char *name, *dflt; SDL_Scancode sc; } k_def[FK_COUNT] = {
    {"freecam_key", "F9", SDL_SCANCODE_F9},
    {"freecam_key_forward", "W", SDL_SCANCODE_W},
    {"freecam_key_back", "S", SDL_SCANCODE_S},
    {"freecam_key_left", "A", SDL_SCANCODE_A},
    {"freecam_key_right", "D", SDL_SCANCODE_D},
    {"freecam_key_down", "Q", SDL_SCANCODE_Q},
    {"freecam_key_up", "E", SDL_SCANCODE_E},
    {"freecam_key_reset", "R", SDL_SCANCODE_R},
    {"freecam_key_fov_narrow", "Z", SDL_SCANCODE_Z},
    {"freecam_key_fov_wide", "X", SDL_SCANCODE_X},
    {"freecam_key_hold", "Tab", SDL_SCANCODE_TAB},
    {"freecam_key_pause", "Space", SDL_SCANCODE_SPACE},
    {"freecam_key_step", "Period", SDL_SCANCODE_PERIOD},
    {"freecam_key_photo", "P", SDL_SCANCODE_P},
    {"freecam_key_hide", "H", SDL_SCANCODE_H},
};

#define LOOK_RAD_PER_COUNT 0.0022f   /* the mouse */
#define LOOK_RAD_PER_SEC 2.2f        /* the right stick at full tilt */
#define FOV_MIN 0.1745f              /* 10 degrees */
#define FOV_MAX 2.0944f              /* 120 degrees */
#define PITCH_MAX 1.5533f            /* 89 degrees */
#define SPEED_NOTCH 1.2f
#define SPEED_DEFAULT 400.0f         /* game units a second */
#define PAD_L3_BIT (1u << 1)         /* the pad's buttons (Mh2IpcPad.buttons, as input.c has them) */
#define PAD_R3_BIT (1u << 2)
#define PAD_GRACE_NS 60000000ull     /* a stick press waits this long for the other before the game sees it */

static struct {
    SDL_Scancode key[FK_COUNT];
    int on, hold;
    int hide;                    /* the HUD is hidden (H): the block's hide mask carries the HUD's stages */
    int photo;                   /* 0 none; 1 asked, waiting for a draw made with the block as it is; 2 handed to the renderer */
    uint32_t photo_seq;          /* the block's sequence the photo waits for the core to have drawn with */
    uint64_t photo_t0;           /* SDL_GetTicks when it was asked */
    char photo_path[1500];
    int paused;                  /* the pause is asked for (Space): the block's FROZEN flag */
    uint32_t grant;              /* the steps asked for (Period); only grows */
    uint32_t fz_state, fz_why, fz_taken, fz_base;   /* the core's answer: Pal_FreezeState's, and the steps taken when the pause began */
    char title[100];
    int auto_hold;               /* the core says the last draw was not a field scene's: the game has its input back, the pose waits */
    uint32_t on_seq;             /* the block's sequence that turned the freecam on: the core's answers count from it */
    int game_pad;                /* freecam_game_input = pad */
    uint32_t seq;                /* the block's sequence (the host's) */
    int published;               /* the block holds this pose */
    float eye[3], yaw, pitch, fov;
    float game_fov;              /* the game's own, when the pose began (a 0 fov in the block means the game's) */
    float notches;               /* the wheel's: the speed is the setting times SPEED_NOTCH to this */
    float base;                  /* the setting's speed */
    float mdx, mdy, wheel;       /* the mouse's, since the last update */
    int rel_mouse;
    uint64_t last_ticks;         /* SDL_GetTicksNS at the last update */
    int l1, r1;                  /* the gamepad's speed buttons, to see a press */
    uint64_t last_vblank;
    char line[120];
    int started;
} fc = {.key = {SDL_SCANCODE_F9, SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_Q,
                SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE,
                SDL_SCANCODE_PERIOD, SDL_SCANCODE_P, SDL_SCANCODE_H}};

/* ---- the settings ---- */

static int key_index(const char *key)
{
    for (int i = 0; i < FK_COUNT; i++)
        if (!strcmp(key, k_def[i].name))
            return i;
    return -1;
}

/* settings.c: freecam_key... = KEY or none. 0; -1 not ours; -2 no such key. */
int freecam_setting(const char *key, const char *value)
{
    int i = key_index(key);
    if (i < 0)
        return -1;
    if (!SDL_strcasecmp(value, "none")) {
        fc.key[i] = SDL_SCANCODE_UNKNOWN;
        return 0;
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(value);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return -2;
    fc.key[i] = sc;
    return 0;
}

const char *freecam_key_text(const char *key)
{
    int i = key_index(key);
    const char *n = i < 0 || fc.key[i] == SDL_SCANCODE_UNKNOWN ? NULL : SDL_GetScancodeName(fc.key[i]);
    return n && n[0] ? n : "none";
}

static float setting_speed(void)
{
    float v = g_cfg.freecam_speed && g_cfg.freecam_speed[0] ? (float)atof(g_cfg.freecam_speed) : SPEED_DEFAULT;
    return v < 10.0f ? 10.0f : v > 50000.0f ? 50000.0f : v;
}

static float speed_now(void)
{
    return fc.base * powf(SPEED_NOTCH, fc.notches);
}

/* ---- the block ---- */

static int game_camera(float eye[3], float at[3], float *fov)
{
    const Mh2IpcFreecam *f = &g_ipc->freecam;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t a = mh2ipc_load(&f->cam_seq);
        if (a == 0 || (a & 1))
            continue;
        uint32_t w[7];
        memcpy(w, f->cam_eye, 12);
        memcpy(w + 3, f->cam_at, 12);
        w[6] = f->cam_fov;
        mh2ipc_fence();
        if (mh2ipc_load(&f->cam_seq) != a)
            continue;
        memcpy(eye, w, 12);
        memcpy(at, w + 3, 12);
        memcpy(fov, w + 6, 4);
        return 1;
    }
    return 0;
}

/* What the core says of its last draw (the block's st_* words): 1 with the values when it has said. */
static int core_status(int *active, uint32_t *echo, uint32_t *vblank, uint32_t pz[3])
{
    const Mh2IpcFreecam *f = &g_ipc->freecam;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t a = mh2ipc_load(&f->st_seq);
        if (a == 0 || (a & 1))
            continue;
        uint32_t act = f->st_active, e = f->st_echo, vb = f->st_vblank, z[3] = {f->st_state, f->st_why, f->st_taken};
        mh2ipc_fence();
        if (mh2ipc_load(&f->st_seq) != a)
            continue;
        *active = act != 0;
        *echo = e;
        *vblank = vb;
        memcpy(pz, z, sizeof z);
        return 1;
    }
    return 0;
}

static uint32_t fbits(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

static void publish(void)
{
    Mh2IpcFreecam *f = &g_ipc->freecam;
    uint32_t flags = (fc.on ? MH2IPC_FC_ON : 0u) | (fc.hold ? MH2IPC_FC_HOLD : 0u) | (fc.on && fc.paused ? MH2IPC_FC_FROZEN : 0u) |
                     (fc.on && fc.hide ? (uint32_t)MH2IPC_FC_HUD << MH2IPC_FC_HIDE_SHIFT : 0u);
    fc.seq++;                               /* the sequence is even at rest: odd while it writes */
    mh2ipc_store(&f->seq, fc.seq);
    mh2ipc_fence();
    f->flags = flags;
    f->step_grant = fc.grant;
    for (int i = 0; i < 3; i++)
        f->eye[i] = fbits(fc.eye[i]);
    f->yaw = fbits(fc.yaw);
    f->pitch = fbits(fc.pitch);
    f->roll = fbits(0.0f);
    f->fov = fbits(fc.fov);
    mh2ipc_fence();
    fc.seq++;
    mh2ipc_store(&f->seq, fc.seq);
    fc.published = 1;
}

/* ---- the pose ---- */

static void pose_from_game(const float eye[3], const float at[3], float fov)
{
    float d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    float h = sqrtf(d[0] * d[0] + d[2] * d[2]);
    for (int i = 0; i < 3; i++)
        fc.eye[i] = eye[i];
    fc.yaw = atan2f(d[0], d[2]);
    fc.pitch = atan2f(d[1], h);
    fc.game_fov = fov;
    fc.fov = fov;
}

static void say(const char *msg)
{
    host_log("%s", msg);
    present_note(msg);
}

static int controlling(void)
{
    return fc.on && !fc.hold && !fc.auto_hold && !kbd_typing();
}

int freecam_active(void)
{
    return fc.on;
}

int freecam_paused(void)
{
    return fc.on && fc.paused;
}

int freecam_controls(void)
{
    return controlling();
}

int freecam_game_pad(void)
{
    return fc.game_pad;
}


static const char *why_text(uint32_t why)
{
    return why == 1 ? "not in the village or a hunt" : why == 2 ? "a movie is playing" : why == 3 ? "the memory card is in use" : "";
}

/* The pause's words for the title and the line: PAUSED (with the steps taken), or what a pause that waits waits for. */
static void pause_text(char *out, size_t cap)
{
    out[0] = 0;
    if (!fc.paused)
        return;
    if (fc.fz_state == 2) {
        uint32_t n = fc.fz_taken - fc.fz_base;
        if (n)
            snprintf(out, cap, " (PAUSED, %u frame%s)", (unsigned)n, n == 1 ? "" : "s");
        else
            snprintf(out, cap, " (PAUSED)");
    } else if (fc.fz_state == 1 && fc.fz_why) {
        snprintf(out, cap, " (PAUSE WAITING: %s)", why_text(fc.fz_why));
    } else {
        snprintf(out, cap, " (PAUSING)");
    }
}

static void show_state(void)
{
    if (!fc.on) {
        present_set_freecam(NULL, NULL);
        return;
    }
    char pz[100];
    pause_text(pz, sizeof pz);
    /* photo mode: the game paused and the HUD hidden */
    int photo = fc.paused && fc.hide && fc.fz_state == 2;
    if (photo) {
        uint32_t n = fc.fz_taken - fc.fz_base;
        if (n)
            snprintf(pz, sizeof pz, " (%u step%s)", (unsigned)n, n == 1 ? "" : "s");
        else
            pz[0] = 0;
    }
    snprintf(fc.title, sizeof fc.title, photo ? "PHOTO%s" : "FREECAM%s%s", pz, fc.hide ? " (HUD hidden)" : "");
    snprintf(fc.line, sizeof(fc.line), photo ? "PHOTO%s%s  speed %.0f  fov %.0f" : "FREECAM%s%s  speed %.0f  fov %.0f", pz,
             fc.auto_hold ? " (not a field scene: the game has the pad)" : fc.hold ? " (hold: the game has the pad)" : "", speed_now(),
             fc.fov * 180.0f / PI_F);
    present_set_freecam(fc.line, fc.title);
    static char last[100];
    if (strcmp(last, fc.title)) {
        snprintf(last, sizeof last, "%s", fc.title);
        host_log("freecam: title: %s", fc.title);
    }
}

static void mouse_mode(void)
{
    int want = controlling() && !g_cfg.no_window;
    if (want != fc.rel_mouse) {
        fc.rel_mouse = want;
        present_relative_mouse(want);
    }
}

static void turn_on(void)
{
    pace_release();   /* the plain pause ends: the camera has its own, and needs the game running */
    float eye[3], at[3], fov;
    fc.started = 1;
    fc.base = setting_speed();
    fc.game_pad = g_cfg.freecam_input && !SDL_strcasecmp(g_cfg.freecam_input, "pad");
    if (!game_camera(eye, at, &fov)) {
        say("freecam: no camera yet (it starts in the village or a hunt)");
        return;
    }
    pose_from_game(eye, at, fov);
    fc.on = 1;
    fc.hold = 0;
    fc.paused = 0;
    fc.hide = 0;
    fc.auto_hold = 0;
    fc.mdx = fc.mdy = fc.wheel = 0.0f;
    fc.last_ticks = SDL_GetTicksNS();
    host_log("freecam: on (%s; game input: %s)", freecam_key_text("freecam_key"), fc.game_pad ? "the pad" : "neutral");
    publish();
    fc.on_seq = fc.seq;
    show_state();
    mouse_mode();
}

static void turn_off(void)
{
    if (!fc.on)
        return;
    fc.on = 0;
    fc.hold = 0;
    fc.paused = 0;   /* leaving the freecam resumes the game first: the same write ends the pause */
    fc.hide = 0;
    fc.auto_hold = 0;
    publish();
    show_state();
    mouse_mode();
    host_log("freecam: off");
}

static void reset_pose(void)
{
    float eye[3], at[3], fov;
    if (!game_camera(eye, at, &fov))
        return;
    pose_from_game(eye, at, fov);
    fc.notches = 0.0f;
    host_log("freecam: back on the game's camera");
    publish();
    show_state();
}

/* Space: the pause. The core takes it at its next VBlank; whether it took hold, and if not why, comes back in the block. */
static void toggle_pause(void)
{
    fc.paused = !fc.paused;
    fc.last_ticks = SDL_GetTicksNS();
    publish();
    if (fc.paused) {
        fc.fz_base = fc.fz_taken;
        say("freecam: pause asked for");
    } else {
        say("freecam: resumed");
    }
    show_state();
}

/* H: the HUD's stages are left out of the draw (the core's gate; the fades stay). */
static void toggle_hide(void)
{
    fc.hide = !fc.hide;
    publish();
    say(fc.hide ? "freecam: HUD hidden" : "freecam: HUD shown");
    show_state();
}

/* ---- photo mode ---- */

/* The pictures folder (the setting pictures, else the user data folder's pictures): 1 when there is one. */
static int pictures_dir(char *out, size_t cap)
{
    if (g_cfg.pictures_dir && g_cfg.pictures_dir[0])
        snprintf(out, cap, "%s", g_cfg.pictures_dir);
    else if (play_data_dir())
        snprintf(out, cap, "%spictures", play_data_dir());
    else
        return 0;
    size_t n = strlen(out);
    while (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\'))
        out[--n] = 0;
    return 1;
}

/* P: a photo. The file is MH2-YYYYMMDD-HHMMSS.png (the host's wall clock) in the pictures folder. The request waits until the core has
 * drawn with the block as it is now (the hide key's mask, the pose), then the renderer's next picture is written on its thread. */
static void photo_key(void)
{
    char dir[1300];
    if (fc.photo) {
        say("photo: the last photo is not done yet");
        return;
    }
    if (!pictures_dir(dir, sizeof dir)) {
        say("photo: no photos folder (this is an automated run: name one with pictures = FOLDER)");
        return;
    }
    if (!SDL_CreateDirectory(dir)) {
        char m[300];
        snprintf(m, sizeof m, "photo: cannot make the folder %s (%s)", dir, SDL_GetError());
        say(m);
        return;
    }
    SDL_Time t = 0;
    SDL_DateTime d;
    SDL_GetCurrentTime(&t);
    if (!SDL_TimeToDateTime(t, &d, true))
        memset(&d, 0, sizeof d);
    char sep = strchr(dir, '\\') ? '\\' : '/';
    for (int n = 1; n < 100; n++) {   /* two in one second: MH2-...-2.png */
        if (n == 1)
            snprintf(fc.photo_path, sizeof fc.photo_path, "%s%cMH2-%04d%02d%02d-%02d%02d%02d.png", dir, sep, d.year, d.month, d.day, d.hour,
                     d.minute, d.second);
        else
            snprintf(fc.photo_path, sizeof fc.photo_path, "%s%cMH2-%04d%02d%02d-%02d%02d%02d-%d.png", dir, sep, d.year, d.month, d.day, d.hour,
                     d.minute, d.second, n);
        if (!SDL_GetPathInfo(fc.photo_path, NULL))
            break;
    }
    fc.photo = 1;
    fc.photo_seq = fc.seq;
    fc.photo_t0 = SDL_GetTicks();
}

/* Each pass: a photo asked for goes to the renderer once the core has answered for a draw with the block as it was asked; and the
 * renderer's answer is told. */
static void photo_poll(int have, uint32_t echo, uint32_t vb)
{
    if (fc.photo == 1 && have && (int32_t)(echo - fc.photo_seq) >= 0) {
        /* The core has drawn with this block by its VBlank VB. A picture shows a draw some VBlanks late: measured on the Windows GPU, a mask
         * written at VBlank 6000 was read at the core's 6006 and the first VSync whose picture had it was 6012 (30 fps frames; a draw
         * is flipped a frame later and the VSync shows the one before). Eight VBlanks on is past that, with a frame of 20 fps to spare. */
        if (gs_request_photo(fc.photo_path, (uint64_t)vb + 8)) {
            fc.photo = 2;
            fc.photo_t0 = SDL_GetTicks();
        } else {
            fc.photo = 0;
            say("photo: no photo (graphics are off)");
        }
    } else if (fc.photo == 1 && SDL_GetTicks() - fc.photo_t0 > 3000) {
        fc.photo = 0;
        say("photo: the game did not answer");
    }
    unsigned w, h;
    int r = gs_photo_result(&w, &h);
    if (r) {
        char m[1700];
        if (r > 0)
            snprintf(m, sizeof m, "photo: %s (%ux%u)", fc.photo_path, w, h);
        else
            snprintf(m, sizeof m, "photo: could not write %s", fc.photo_path);
        fc.photo = 0;
        say(m);
    } else if (fc.photo == 2 && SDL_GetTicks() - fc.photo_t0 > 5000) {
        fc.photo = 0;
        say("photo: no photo was made");
    }
}

/* Period: one real frame of the game while the pause holds. */
static void step_once(void)
{
    if (!fc.paused) {
        say("frame advance: pause first (Space)");
        return;
    }
    if (fc.fz_state != 2) {
        if (fc.fz_state == 1 && fc.fz_why) {
            char m[100];
            snprintf(m, sizeof m, "frame advance: the pause is waiting (%s)", why_text(fc.fz_why));
            say(m);
        } else {
            say("frame advance: the pause has not begun yet");
        }
        return;
    }
    fc.grant++;
    publish();
}

/* A key event of the window (the main loop): 1 when the freecam took it. */
int freecam_key(const SDL_Event *e)
{
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat || kbd_typing())
        return 0;
    SDL_Scancode sc = e->key.scancode;
    if (sc == SDL_SCANCODE_UNKNOWN)
        return 0;
    if (sc == fc.key[FK_TOGGLE]) {
        if (fc.on)
            turn_off();
        else
            turn_on();
        return 1;
    }
    if (!fc.on)
        return 0;
    if (sc == SDL_SCANCODE_ESCAPE) {
        turn_off();
        return 1;
    }
    if (sc == fc.key[FK_HOLD]) {
        fc.hold = !fc.hold;
        fc.last_ticks = SDL_GetTicksNS();
        publish();
        show_state();
        mouse_mode();
        return 1;
    }
    if (sc == fc.key[FK_PAUSE]) {
        toggle_pause();
        return 1;
    }
    if (sc == fc.key[FK_STEP]) {
        step_once();
        return 1;
    }
    if (sc == fc.key[FK_PHOTO]) {
        photo_key();
        return 1;
    }
    if (controlling() && sc == fc.key[FK_HIDE]) {
        toggle_hide();
        return 1;
    }
    if (controlling() && sc == fc.key[FK_RESET]) {
        reset_pose();
        return 1;
    }
    return 0;
}

/* Mouse events: the look and the wheel. */
void freecam_event(const SDL_Event *e)
{
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED || e->type == SDL_EVENT_WINDOW_FOCUS_LOST)
        fc.rel_mouse = 0;   /* SDL gave the mouse back with the focus: asked for again at the next update */
    if (!controlling())
        return;
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        fc.mdx += e->motion.xrel;
        fc.mdy += e->motion.yrel;
    } else if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        fc.wheel += e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
    }
}

/* ---- a development script (input.c's --pad-script: FCTOGGLE, FCON, FCOFF, FCRESET, FCHOLD, FCKEY, FCLOOK, FCWHEEL, FCSTICK) and the
 *      release test build's environment variable MH2_FREECAM_TEST ---- */
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
typedef struct FcStep {
    char what;                   /* T toggle, O on, F off, R reset, H hold, K key, L look, W wheel, S stick, P gamepad stick buttons, U pause, N step, I hide, C photo */
    uint64_t start, frames;
    SDL_Scancode keys[4];
    int nkeys;
    float a, b, c, d;
    int done;
} FcStep;
static FcStep sStep[4096];
static int sNStep, sScripted;
static float sStick[4];          /* a script's sticks: lx, ly, rx, ry, -1 to 1 (0 when none is held) */
/* The looks the script has made and the core has not yet drawn with: the development tree's pose check needs the VBlank of the first draw
 * that has each (the picture shows a draw some VBlanks later), because the core runs ahead of the renderer by the ring's fill, and a
 * loaded machine makes that many frames. */
static struct { uint64_t start; uint32_t seq; } sLook[8];
static int sNLook;

/* The script's keys name SDL's scancodes with the spaces left out: W, D, LSHIFT, LCTRL, TAB, SPACE, F9, ... */
static SDL_Scancode script_key(const char *name)
{
    if (!SDL_strcasecmp(name, "LSHIFT"))
        return SDL_SCANCODE_LSHIFT;
    if (!SDL_strcasecmp(name, "LCTRL"))
        return SDL_SCANCODE_LCTRL;
    return SDL_GetScancodeFromName(name);
}

/* TOKEN@VBLANK[=ARGS][+N]: 0 taken; -1 bad; -2 not ours. */
int freecam_script_cmd(const char *tok, const char *rest)
{
    FcStep s;
    memset(&s, 0, sizeof(s));
    if (SDL_strncasecmp(tok, "FC", 2))
        return -2;
    if (sNStep == (int)(sizeof(sStep) / sizeof(sStep[0])))
        return -1;
    char buf[200];
    snprintf(buf, sizeof(buf), "%s", rest);
    char *plus = strchr(buf, '+');
    if (plus)
        *plus = 0;
    s.frames = plus ? strtoull(plus + 1, NULL, 10) : 0;
    char *eq = strchr(buf, '=');
    if (eq)
        *eq++ = 0;
    s.start = strtoull(buf, NULL, 10);
    const char *t = tok + 2;
    if (!SDL_strcasecmp(t, "TOGGLE"))
        s.what = 'T';
    else if (!SDL_strcasecmp(t, "ON"))
        s.what = 'O';
    else if (!SDL_strcasecmp(t, "OFF"))
        s.what = 'F';
    else if (!SDL_strcasecmp(t, "RESET"))
        s.what = 'R';
    else if (!SDL_strcasecmp(t, "HOLD"))
        s.what = 'H';
    else if (!SDL_strcasecmp(t, "PAUSE"))
        s.what = 'U';
    else if (!SDL_strcasecmp(t, "STEP"))
        s.what = 'N';
    else if (!SDL_strcasecmp(t, "HIDE"))
        s.what = 'I';
    else if (!SDL_strcasecmp(t, "PHOTO"))
        s.what = 'C';
    else if (!SDL_strcasecmp(t, "KEY") && eq) {
        s.what = 'K';
        if (!s.frames)
            s.frames = 6;
        char *save = NULL;
        for (char *n = SDL_strtok_r(eq, "/", &save); n; n = SDL_strtok_r(NULL, "/", &save)) {
            if (s.nkeys == 4 || (s.keys[s.nkeys] = script_key(n)) == SDL_SCANCODE_UNKNOWN)
                return -1;
            s.nkeys++;
        }
    } else if (!SDL_strcasecmp(t, "LOOK") && eq) {
        s.what = 'L';
        if (sscanf(eq, "%f/%f", &s.a, &s.b) != 2)
            return -1;
    } else if (!SDL_strcasecmp(t, "WHEEL") && eq) {
        s.what = 'W';
        s.a = (float)atof(eq);
    } else if (!SDL_strcasecmp(t, "PAD") && eq) {   /* FCPAD@V=L3/R3+N: the gamepad's stick buttons, N VBlanks */
        s.what = 'P';
        if (!s.frames)
            s.frames = 6;
        char *save = NULL;
        for (char *n = SDL_strtok_r(eq, "/", &save); n; n = SDL_strtok_r(NULL, "/", &save)) {
            if (!SDL_strcasecmp(n, "L3"))
                s.nkeys |= 1;
            else if (!SDL_strcasecmp(n, "R3"))
                s.nkeys |= 2;
            else
                return -1;
        }
    } else if (!SDL_strcasecmp(t, "STICK") && eq) {
        s.what = 'S';
        if (!s.frames)
            s.frames = 6;
        if (sscanf(eq, "%f/%f/%f/%f", &s.a, &s.b, &s.c, &s.d) != 4)
            return -1;
    } else {
        return -1;
    }
    sStep[sNStep++] = s;
    sScripted = 1;
    return 0;
}

static int script_key_down(SDL_Scancode sc)
{
    uint64_t v = gs_last_vblank() + 1;
    for (int i = 0; i < sNStep; i++)
        if (sStep[i].what == 'K' && v >= sStep[i].start && v < sStep[i].start + sStep[i].frames)
            for (int j = 0; j < sStep[i].nkeys; j++)
                if (sStep[i].keys[j] == sc)
                    return 1;
    return 0;
}

/* The gamepad's stick buttons a script holds at the VBlank being shown (PAD_L3_BIT, PAD_R3_BIT). */
static uint16_t script_pad_bits(void)
{
    uint64_t v = gs_last_vblank() + 1;
    uint16_t b = 0;
    for (int i = 0; i < sNStep; i++)
        if (sStep[i].what == 'P' && v >= sStep[i].start && v < sStep[i].start + sStep[i].frames)
            b |= (uint16_t)((sStep[i].nkeys & 1 ? PAD_L3_BIT : 0) | (sStep[i].nkeys & 2 ? PAD_R3_BIT : 0));
    return b;
}

/* A script's pause, step, hide or photo with the freecam off: the key would have gone to the game. 1 after saying why nothing happens. */
static int script_needs_freecam(void)
{
    if (fc.on)
        return 0;
    say("freecam: not on");
    return 1;
}

static void script_run(void)
{
    uint64_t v = gs_last_vblank() + 1;
    memset(sStick, 0, sizeof(sStick));
    for (int i = 0; i < sNStep; i++) {
        FcStep *s = &sStep[i];
        uint64_t at = v;
        if (s->what == 'L') {   /* a look is timed by the game's frames: the core's last draw, which is ahead of the renderer by the ring's fill
                                 * (4 to 7 VBlanks quiet, 17 or more on a loaded machine), so a look lands the same number of frames on whatever
                                 * the load; the renderer's VBlank only before the core has said anything */
            int act = 0;
            uint32_t echo = 0, cvb = 0, z[3];
            if (core_status(&act, &echo, &cvb, z) && cvb + 1 > at)
                at = (uint64_t)cvb + 1;
        }
        if (at < s->start)
            continue;
        if (s->what == 'S' && v < s->start + s->frames) {
            sStick[0] = s->a;
            sStick[1] = s->b;
            sStick[2] = s->c;
            sStick[3] = s->d;
        }
        if (s->done || s->what == 'K' || s->what == 'S' || s->what == 'P')
            continue;
        s->done = 1;
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        switch (s->what) {
        case 'T':
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.scancode = fc.key[FK_TOGGLE];
            freecam_key(&e);
            break;
        case 'O':
            if (!fc.on)
                turn_on();
            break;
        case 'F':
            turn_off();
            break;
        case 'R':
            if (controlling())
                reset_pose();
            break;
        case 'H':
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.scancode = fc.key[FK_HOLD];
            freecam_key(&e);
            break;
        case 'U':
        case 'N':
        case 'I':
        case 'C':
            if (script_needs_freecam())
                break;
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.scancode = fc.key[s->what == 'U' ? FK_PAUSE : s->what == 'N' ? FK_STEP : s->what == 'I' ? FK_HIDE : FK_PHOTO];
            freecam_key(&e);
            break;
        case 'L':
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.xrel = s->a;
            e.motion.yrel = s->b;
            freecam_event(&e);
            host_log("freecam: script: FCLOOK@%llu applied at VBlank %llu (the renderer's %llu)", (unsigned long long)s->start,
                     (unsigned long long)at, (unsigned long long)v);
            if (sNLook < 8) {
                sLook[sNLook].start = s->start;
                sLook[sNLook++].seq = 0;
            }
            break;
        case 'W':
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.y = s->a;
            freecam_event(&e);
            break;
        }
    }
}

/* The block was published (publish): the looks made since the last publish are in it. */
static void script_looks_published(uint32_t seq)
{
    for (int i = 0; i < sNLook; i++)
        if (!sLook[i].seq)
            sLook[i].seq = seq;
}

/* The core has drawn with the block of sequence ECHO by its VBlank VB: the looks in it are drawn. */
static void script_looks_drawn(int have, uint32_t echo, uint32_t vb)
{
    for (int i = 0; have && i < sNLook;) {
        if (sLook[i].seq && (int32_t)(echo - sLook[i].seq) >= 0) {
            host_log("freecam: script: FCLOOK@%llu first drawn by the core at VBlank %u", (unsigned long long)sLook[i].start, vb);
            sLook[i] = sLook[--sNLook];
        } else {
            i++;
        }
    }
}
#endif

/* ---- the update ---- */

static int key_down(const bool *k, int idx)
{
    SDL_Scancode sc = fc.key[idx];
    if (sc == SDL_SCANCODE_UNKNOWN)
        return 0;
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    if (sScripted && script_key_down(sc))
        return 1;
#endif
    return k[sc];
}

static float deadzone(float v)
{
    float a = fabsf(v);
    if (a < 0.15f)
        return 0.0f;
    a = (a - 0.15f) / 0.85f;
    return v < 0 ? -a : a;
}

static void move(float dt)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    float fwd = 0, side = 0, up = 0, lookx = fc.mdx * LOOK_RAD_PER_COUNT, looky = fc.mdy * LOOK_RAD_PER_COUNT;
    float fast = 1.0f;
    float st[4] = {0, 0, 0, 0};
    float zoom = 0.0f;
    fc.mdx = fc.mdy = 0.0f;
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
        || (sScripted && script_key_down(SDL_SCANCODE_LSHIFT))
#endif
    )
        fast *= 4.0f;
    if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
        || (sScripted && script_key_down(SDL_SCANCODE_LCTRL))
#endif
    )
        fast *= 0.25f;
    if (key_down(k, FK_FWD))
        fwd += 1;
    if (key_down(k, FK_BACK))
        fwd -= 1;
    if (key_down(k, FK_RIGHT))
        side += 1;
    if (key_down(k, FK_LEFT))
        side -= 1;
    if (key_down(k, FK_UP))
        up += 1;
    if (key_down(k, FK_DOWN))
        up -= 1;
    if (key_down(k, FK_FOVNARROW))
        zoom -= 1;
    if (key_down(k, FK_FOVWIDE))
        zoom += 1;
    /* a gamepad: its sticks are the freecam's only when the game's pad is neutral */
    SDL_Gamepad *gp = fc.game_pad ? NULL : input_gamepad();
    if (gp) {
        st[0] = deadzone(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f);
        st[1] = deadzone(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f);
        st[2] = deadzone(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f);
        st[3] = deadzone(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f);
        up += SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000 ? 1.0f : 0.0f;
        up -= SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 ? 1.0f : 0.0f;
        int l1 = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), r1 = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        if (l1 && !fc.l1)
            fc.wheel -= 1.0f;
        if (r1 && !fc.r1)
            fc.wheel += 1.0f;
        fc.l1 = l1;
        fc.r1 = r1;
    }
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    if (sScripted)
        for (int i = 0; i < 4; i++)
            if (sStick[i] != 0.0f)
                st[i] = sStick[i];
#endif
    fwd -= st[1];   /* the stick's up is negative */
    side += st[0];
    lookx += st[2] * LOOK_RAD_PER_SEC * dt;
    looky += st[3] * LOOK_RAD_PER_SEC * dt;
    if (fc.wheel != 0.0f) {
        fc.notches += fc.wheel;
        if (fc.notches < -20.0f)
            fc.notches = -20.0f;
        if (fc.notches > 20.0f)
            fc.notches = 20.0f;
        fc.wheel = 0.0f;
    }
    fc.yaw -= lookx;   /* a turn to the right lowers the yaw: the game's world is right-handed (the view's right is -X at yaw 0) */
    fc.pitch -= looky;   /* the mouse's up (negative y) looks up */
    if (fc.yaw > PI_F)
        fc.yaw -= 2.0f * PI_F;
    if (fc.yaw < -PI_F)
        fc.yaw += 2.0f * PI_F;
    if (fc.pitch > PITCH_MAX)
        fc.pitch = PITCH_MAX;
    if (fc.pitch < -PITCH_MAX)
        fc.pitch = -PITCH_MAX;
    if (zoom != 0.0f) {
        fc.fov *= expf(zoom * 0.9f * dt);
        if (fc.fov < FOV_MIN)
            fc.fov = FOV_MIN;
        if (fc.fov > FOV_MAX)
            fc.fov = FOV_MAX;
    }
    float sp = speed_now() * fast * dt;
    float sy = sinf(fc.yaw), cy = cosf(fc.yaw), spch = sinf(fc.pitch), cpch = cosf(fc.pitch);
    /* forward along the view (the pitch included), the sideways step across it on the ground plane (the view's right is
     * (-cos yaw, 0, sin yaw)), up and down on Y */
    fc.eye[0] += sp * (fwd * sy * cpch - side * cy);
    fc.eye[1] += sp * (fwd * spch + up);
    fc.eye[2] += sp * (fwd * cy * cpch + side * sy);
}

#ifdef MH2_FREECAM_TEST
/* A build made only to try the release host's freecam (never shipped: the macro is set by hand for one build directory):
 * the environment variable MH2_FREECAM_TEST lists the script's commands, comma separated, as --pad-script writes them. */
static void test_env(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    const char *e = hfs_getenv("MH2_FREECAM_TEST");
    char buf[2000];
    snprintf(buf, sizeof(buf), "%s", e ? e : "");
    char *save = NULL;
    for (char *tok = SDL_strtok_r(buf, ",", &save); tok; tok = SDL_strtok_r(NULL, ",", &save)) {
        char *at = strchr(tok, '@');
        if (!at)
            continue;
        *at = 0;
        if ((!SDL_strncasecmp(tok, "PZ", 2) ? pace_script_cmd(tok, at + 1) : freecam_script_cmd(tok, at + 1)) != 0)   /* (pace.c's PZPAUSE, PZFF too) */
            host_log("MH2_FREECAM_TEST: cannot read \"%s@%s\"", tok, at + 1);
    }
}
#endif

/* ---- the gamepad's toggle: L3 and R3 together ---- */

static int pad_toggle_on(void)
{
    return g_cfg.freecam_pad && !SDL_strcasecmp(g_cfg.freecam_pad, "on");
}

/* The time of the stick presses: the host's clock, or in a script the game's (the VBlanks seen). */
static uint64_t pad_now(void)
{
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    if (sScripted)
        return gs_last_vblank() * 16683350ull + 1;
#endif
    return SDL_GetTicksNS() + 1;
}

/* Called by input.c on the buttons of the game's pad 0, before anything else touches them (a script of the pad is later). Both stick
 * buttons of the gamepad down together turn the freecam on or off (the setting freecam_pad_toggle, off by default: the game is then given its pad as it is). The game
 * never sees the combination: a stick button is kept from it while the other may still join (60 ms from its press), and, once both
 * were down, until both are up. So when the two are not pressed at the same frame the game sees nothing of the first for those
 * 60 ms; a press the other does not join in that time reaches the game late, and one shorter than that not at all. A session
 * the freecam is refused in keeps nothing from the game and only says why nothing happened (the development build's rule). */
static void pad_filter(uint16_t *buttons)
{
    static uint64_t since[2];
    static int combo;
    const uint16_t bit[2] = {PAD_L3_BIT, PAD_R3_BIT};
    int d[2] = {0, 0};
    SDL_Gamepad *gp = input_gamepad();
    if (gp) {
        d[0] = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        d[1] = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    }
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    uint16_t sb = script_pad_bits();
    for (int i = 0; i < 2; i++)
        if (sb & bit[i]) {
            d[i] = 1;
            *buttons |= bit[i];   /* the script presses it for the game too */
        }
#endif
    if (!pad_toggle_on())
        return;
    uint64_t now = pad_now();
    for (int i = 0; i < 2; i++) {
        if (d[i] && !since[i])
            since[i] = now;
        else if (!d[i])
            since[i] = 0;
    }
    int refused = 0;
    if (d[0] && d[1]) {
        if (!combo) {
            combo = 1;
            if (fc.on)
                turn_off();
            else
                turn_on();
        }
    } else if (!d[0] && !d[1]) {
        combo = 0;
    }
    if (refused)
        return;
    for (int i = 0; i < 2; i++)
        if (combo || (since[i] && now - since[i] < PAD_GRACE_NS))
            *buttons &= (uint16_t)~bit[i];
}

void freecam_pad_filter(uint16_t *buttons)
{
    pad_filter(buttons);
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    if (sScripted) {   /* a script's test: what the game is given of the two stick buttons, at each change */
        static uint16_t seen = 0xFFFF;
        uint16_t now = *buttons & (PAD_L3_BIT | PAD_R3_BIT);
        if (now != seen) {
            seen = now;
            host_log("freecam pad: the game is given %s%s at VBlank %llu", now & PAD_L3_BIT ? "L3 " : "", now & PAD_R3_BIT ? "R3" : now ? "" : "neither",
                     (unsigned long long)gs_last_vblank());
        }
    }
#endif
}

/* The core's answer on its last draw: a draw that was not a field scene's gives the game its input back and the pose waits. */
static void poll_status(void)
{
    int active = 0;
    uint32_t echo = 0, vb = 0, z[3];
    int have = core_status(&active, &echo, &vb, z);
    if (have && (z[0] != fc.fz_state || z[1] != fc.fz_why || z[2] != fc.fz_taken)) {
        if (z[0] == 2 && fc.fz_state != 2)
            fc.fz_base = z[2];   /* the steps are counted from where the pause took hold */
        if (z[0] != fc.fz_state)
            host_log("freecam: the game is %s%s%s (core VBlank %u)", z[0] == 2 ? "frozen" : z[0] == 1 ? "waiting to freeze" : "running",
                     z[0] == 1 && z[1] ? ": " : "", z[0] == 1 ? why_text(z[1]) : "", vb);
        fc.fz_state = z[0];
        fc.fz_why = z[1];
        fc.fz_taken = z[2];
        audio_mute_by(AUDIO_MUTE_FREEZE, z[0] == 2);   /* the sound fades out while the game is frozen and back in on the resume */
        show_state();
    }
    photo_poll(have, echo, vb);
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    script_looks_drawn(have, echo, vb);
#endif
    if (!fc.on) {
        fc.auto_hold = 0;
        return;
    }
    if (!have || (int32_t)(echo - fc.on_seq) < 0)
        return;   /* nothing yet from a draw made with this pose */
    /* The core answers for a draw made with the pose (a field scene's, or two game steps with none: pc/core/ipc/ipc_glue.c), so the
     * answer is believed as it comes. */
    if ((active == 0) != (fc.auto_hold != 0)) {
        fc.auto_hold = !active;
        host_log("freecam: VBlank %llu: %s", (unsigned long long)gs_last_vblank(), fc.auto_hold ? "the game is not in a field scene: its input is back, the pose waits"
                                              : "a field scene again: the freecam has control");
        show_state();
        mouse_mode();
    }
}

void freecam_update(void)
{
    uint64_t now = SDL_GetTicksNS();
#ifdef MH2_FREECAM_TEST
    test_env();
#endif
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    if (sNStep)
        script_run();
#endif
    if (!fc.on) {
        fc.last_ticks = now;
        fc.last_vblank = gs_last_vblank();
        if (fc.published) {
            fc.published = 0;
        }
        return;
    }
    float dt;
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    if (sScripted) {   /* a script's time is the game's: a pose depends on the VBlanks, not on the host's pace */
        uint64_t v = gs_last_vblank();
        dt = (float)(v - fc.last_vblank) / 59.94f;
        fc.last_vblank = v;
    } else
#endif
    {
        dt = (float)((double)(now - fc.last_ticks) * 1e-9);
        if (dt > 0.1f)
            dt = 0.1f;
    }
    fc.last_ticks = now;
    poll_status();
    mouse_mode();
    if (controlling()) {
        float before[6] = {fc.eye[0], fc.eye[1], fc.eye[2], fc.yaw, fc.pitch, fc.fov};
        float notches = fc.notches;
        move(dt);
        if (memcmp(before, &fc.eye[0], 12) || before[3] != fc.yaw || before[4] != fc.pitch || before[5] != fc.fov || notches != fc.notches) {
            publish();
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
            script_looks_published(fc.seq);
#endif
            if (notches != fc.notches || before[5] != fc.fov)
                show_state();
        }
    }
}
