/*
 * The host's pacing of the core: the plain pause, fast-forward, and fast loading's switch.
 *
 * All three are modes of one thing, the core's wait for real time at each VBlank's start (Ipc_Pace, pc/core/ipc/host_ipc.c).
 * The host writes three words of the IPC header and reads three back:
 *   pace_flags   HOLD: the pause; FF: fast-forward (the key is down)
 *   pace_grant   the VBlanks let through while HOLD (the step key adds one; it only grows)
 *   pace_cap     fast-forward's cap, in hundredths of the game's speed (0: none)
 *   pace_state   (core) HELD: it waits in the pause; UNPACED: the VBlank in progress is not waited for
 *   pace_taken   (core) the grants it has taken
 *   pace_vblank  (core) the VBlank starting
 * and the flag MH2IPC_HOST_FASTLOAD in host_flags (the setting fast_loading, set when mh2 starts).
 * Pacing is only the wait: no value the game computes, no VBlank's number and no line of a recorded session depends on it, so all
 * three work in a session that is being recorded or played back.
 *
 * Keys (settings pace_key_pause, pace_key_step, pace_key_fast): Space pauses and resumes and Period lets one VBlank through
 * (the camera's keys while the free camera is on: freecam.c takes them first); the fast-forward key is held. They are off
 * while the keyboard types (kbd_typing). Fast-forward is off in the pause, in the free camera's pause and while a movie is open
 * (the core's rule).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

enum { PK_PAUSE, PK_STEP, PK_FAST, PK_COUNT };
static const struct { const char *name; } k_pk[PK_COUNT] = {{"pace_key_pause"}, {"pace_key_step"}, {"pace_key_fast"}};

static struct {
    SDL_Scancode key[PK_COUNT];
    int paused;                 /* the pause is asked for */
    int ff;                     /* fast-forward is asked for (the word's FF bit) */
    uint32_t grant;             /* the steps granted: written to pace_grant */
    uint32_t base;              /* the core's pace_taken when the pause took hold, to count the steps from */
    int held;                   /* the core is in the pause (HELD) */
    int unpaced;                /* the core's UNPACED, for the log */
    int loading;                /* the core's LOADING, for the log */
    uint64_t loading_t0;
    uint32_t loading_v0;
    uint64_t unpaced_t0;        /* SDL_GetTicks when it began */
    uint32_t unpaced_v0;        /* the core's VBlank then */
    uint32_t cap;               /* the cap, hundredths; 0 none */
    char line[60];
    int started;
} pc = {.key = {SDL_SCANCODE_SPACE, SDL_SCANCODE_PERIOD, SDL_SCANCODE_GRAVE}};

/* ---- the settings ---- */

static int key_index(const char *key)
{
    for (int i = 0; i < PK_COUNT; i++)
        if (!strcmp(key, k_pk[i].name))
            return i;
    return -1;
}

/* settings.c: pace_key_pause, pace_key_step, pace_key_fast = KEY or none. 0; -1 not ours; -2 no such key. */
int pace_setting(const char *key, const char *value)
{
    int i = key_index(key);
    if (i < 0)
        return -1;
    if (!SDL_strcasecmp(value, "none")) {
        pc.key[i] = SDL_SCANCODE_UNKNOWN;
        return 0;
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(value);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return -2;
    pc.key[i] = sc;
    return 0;
}

const char *pace_key_text(const char *key)
{
    int i = key_index(key);
    const char *n = i < 0 || pc.key[i] == SDL_SCANCODE_UNKNOWN ? NULL : SDL_GetScancodeName(pc.key[i]);
    return n && n[0] ? n : "none";
}

/* fast_forward_cap: a multiple of the game's speed ("4"; 1.5 is allowed), or "none"; the default is 4. */
static uint32_t cap_setting(void)
{
    const char *s = g_cfg.fast_forward_cap;
    if (!s || !*s)
        return 400;
    if (!SDL_strcasecmp(s, "none") || !SDL_strcasecmp(s, "off") || !strcmp(s, "0"))
        return 0;
    double v = atof(s);
    if (v < 1.0)
        return 400;
    return v > 1000.0 ? 100000u : (uint32_t)(v * 100.0 + 0.5);
}

/* ---- the words ---- */

static void publish(void)
{
    uint32_t f = (pc.paused ? MH2IPC_PACE_HOLD : 0) | (pc.ff ? MH2IPC_PACE_FF : 0);
    mh2ipc_store(&g_ipc->pace_cap, pc.cap);
    mh2ipc_store(&g_ipc->pace_grant, pc.grant);
    mh2ipc_store(&g_ipc->pace_flags, f);
}

static void say(const char *msg)
{
    host_log("%s", msg);
    present_note(msg);
}

static void show(void)
{
    char t[60];
    t[0] = 0;
    if (pc.paused) {
        uint32_t n = mh2ipc_load(&g_ipc->pace_taken) - pc.base;
        if (!pc.held)
            snprintf(t, sizeof t, "PAUSING");
        else if (n)
            snprintf(t, sizeof t, "PAUSED, %u frame%s", (unsigned)n, n == 1 ? "" : "s");
        else
            snprintf(t, sizeof t, "PAUSED");
    } else if (pc.ff) {
        if (pc.cap)
            snprintf(t, sizeof t, "FAST FORWARD x%g", pc.cap / 100.0);
        else
            snprintf(t, sizeof t, "FAST FORWARD");
    }
    if (strcmp(t, pc.line)) {
        snprintf(pc.line, sizeof pc.line, "%s", t);
        present_set_pace(t);
        if (t[0])
            host_log("pace: %s (core VBlank %u)", t, (unsigned)mh2ipc_load(&g_ipc->pace_vblank));
    }
}

int pace_paused(void)
{
    return pc.paused;
}

static void set_pause(int on)
{
    if (pc.paused == on)
        return;
    pc.paused = on;
    if (on) {
        pc.base = mh2ipc_load(&g_ipc->pace_taken);
        pc.grant = pc.base;   /* nothing granted yet: the core's count and the host's are the same */
        pc.ff = 0;
    }
    publish();
    audio_mute_by(AUDIO_MUTE_PACE, on);
    say(on ? "pause asked for" : "resumed");
    show();
}

/* The free camera turns on: the plain pause ends (the camera has its own, and needs the game running). */
void pace_release(void)
{
    if (pc.paused)
        set_pause(0);
}

/* A save state's request is waiting for the next VBlank: while the pause holds, let one through. */
void pace_nudge(void)
{
    if (pc.paused && pc.held && mh2ipc_load(&g_ipc->pace_taken) == pc.grant) {
        pc.grant++;
        publish();
    }
}

static void step_once(void)
{
    if (!pc.paused) {
        say("frame advance: pause first");
        return;
    }
    if (!pc.held) {
        say("frame advance: the pause has not begun yet");
        return;
    }
    if (mh2ipc_load(&g_ipc->pace_taken) != pc.grant)
        return;   /* the last one is not through yet */
    pc.grant++;
    publish();
}

/* A key event of the window (the main loop, after the freecam's): 1 when it was the pause's or the step. */
int pace_key(const SDL_Event *e)
{
    if (e->type != SDL_EVENT_KEY_DOWN || kbd_typing() || freecam_active())
        return 0;
    SDL_Scancode sc = e->key.scancode;
    if (sc == SDL_SCANCODE_UNKNOWN)
        return 0;
    if (sc == pc.key[PK_PAUSE]) {
        if (!e->key.repeat)
            set_pause(!pc.paused);
        return 1;
    }
    if (sc == pc.key[PK_STEP]) {   /* a held key steps at its repeat rate */
        step_once();
        return 1;
    }
    return 0;
}

/* ---- a development script (input.c's --pad-script): PZPAUSE@V=MS/STEPS pauses when the game reaches V, waits MS milliseconds, lets STEPS
 *      VBlanks through 120 ms apart, waits 300 and resumes; PZFF@V+N holds fast-forward while the game is at V to V + N. Times are the
 *      host's, since a paused game's VBlanks do not pass. ---- */
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
typedef struct PzStep {
    char what;   /* P pause, F fast-forward */
    uint64_t v, n;
    uint64_t ms, steps;
    int phase;   /* P: 0 waiting for V, 1 pausing, 2 steps, 3 resuming, 4 done; F: 0 waiting, 1 on, 2 done */
    uint64_t t0, granted;
} PzStep;
static PzStep sPz[64];
static int sNPz;

int pace_script_cmd(const char *tok, const char *rest)
{
    if (SDL_strncasecmp(tok, "PZ", 2))
        return -2;
    if (sNPz == (int)(sizeof sPz / sizeof sPz[0]))
        return -1;
    PzStep s;
    memset(&s, 0, sizeof s);
    char buf[100];
    snprintf(buf, sizeof buf, "%s", rest);
    char *plus = strchr(buf, '+');
    if (plus)
        *plus = 0;
    s.n = plus ? strtoull(plus + 1, NULL, 10) : 0;
    char *eq = strchr(buf, '=');
    if (eq)
        *eq++ = 0;
    s.v = strtoull(buf, NULL, 10);
    if (!SDL_strcasecmp(tok + 2, "PAUSE")) {
        s.what = 'P';
        if (eq && sscanf(eq, "%llu/%llu", (unsigned long long *)&s.ms, (unsigned long long *)&s.steps) != 2)
            return -1;
    } else if (!SDL_strcasecmp(tok + 2, "FF")) {
        s.what = 'F';
        if (!s.n)
            s.n = 600;
    } else {
        return -1;
    }
    sPz[sNPz++] = s;
    return 0;
}

static void script_run(void)
{
    uint64_t v = gs_last_vblank(), now = SDL_GetTicks();
    int want_ff = 0;
    for (int i = 0; i < sNPz; i++) {
        PzStep *s = &sPz[i];
        if (s->what == 'F') {
            if (v >= s->v && v < s->v + s->n)
                want_ff = 1;
            continue;
        }
        switch (s->phase) {
        case 0:
            if (v >= s->v) {
                set_pause(1);
                s->phase = 1;
                s->t0 = now;
            }
            break;
        case 1:
            if (pc.held && now - s->t0 >= s->ms) {
                s->phase = 2;
                s->t0 = now;
            }
            break;
        case 2:
            if (s->granted >= s->steps) {
                if (now - s->t0 >= 300) {
                    set_pause(0);
                    s->phase = 4;
                }
            } else if (now - s->t0 >= 120 && mh2ipc_load(&g_ipc->pace_taken) == pc.grant) {
                step_once();
                s->granted++;
                s->t0 = now;
            }
            break;
        default:
            break;
        }
    }
    if (want_ff != pc.ff && !pc.paused && !freecam_paused()) {
        pc.ff = want_ff;
        publish();
        show();
    }
}
#else
#define sNPz 0
#endif


/* ---- each pass of the main loop ---- */

void pace_update(void)
{
    if (!pc.started) {
        pc.started = 1;
        pc.cap = cap_setting();
        publish();
    }
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
    if (sNPz)
        script_run();
#endif
    /* fast-forward: the key is down; not in a pause (either), not while the keyboard types */
    int allowed = !pc.paused && !freecam_paused() && !kbd_typing();
    if (!sNPz) {   /* (a script holds it for itself) */
        int down = 0;
        if (pc.key[PK_FAST] != SDL_SCANCODE_UNKNOWN && allowed) {
            int n = 0;
            const bool *ks = SDL_GetKeyboardState(&n);
            down = ks && (int)pc.key[PK_FAST] < n && ks[pc.key[PK_FAST]];
        }
        if (down != pc.ff) {
            pc.ff = down;
            publish();
            show();
        }
    }
    if (!allowed && pc.ff && !pc.paused) {   /* the free camera paused, or the keyboard types: back to the game's speed */
        pc.ff = 0;
        publish();
        show();
    }
    uint32_t st = mh2ipc_load(&g_ipc->pace_state);
    int held = (st & MH2IPC_PACE_HELD) != 0;
    if (held && !pc.held) {
        pc.held = 1;
        host_log("pace: the core holds at VBlank %u", (unsigned)mh2ipc_load(&g_ipc->pace_vblank));
        show();
    } else if (!held && pc.held && !pc.paused) {   /* (between two steps the core is not in the pause for a moment) */
        pc.held = 0;
    }
    if (pc.paused && pc.held) {
        static uint32_t last;
        uint32_t t = mh2ipc_load(&g_ipc->pace_taken);
        if (t != last) {
            last = t;
            show();
        }
    }
    int ld = (st & MH2IPC_PACE_LOADING) != 0 && !held;
    if (ld != pc.loading) {   /* the loading screen, in either mode: how long it took in the host's time */
        uint32_t cv = mh2ipc_load(&g_ipc->pace_vblank);
        if (ld) {
            pc.loading_t0 = SDL_GetTicks();
            pc.loading_v0 = cv;
        } else {
            host_log("pace: loading screen: %u VBlanks in %llu ms (VBlank %u to %u)", (unsigned)(cv - pc.loading_v0),
                     (unsigned long long)(SDL_GetTicks() - pc.loading_t0), (unsigned)pc.loading_v0, (unsigned)cv);
        }
        pc.loading = ld;
    }
    int un = (st & MH2IPC_PACE_UNPACED) != 0 && !held;
    if (un != pc.unpaced) {
        uint32_t cv = mh2ipc_load(&g_ipc->pace_vblank);
        if (un) {
            pc.unpaced_t0 = SDL_GetTicks();
            pc.unpaced_v0 = cv;
        } else {
            host_log("pace: unpaced stretch: %u VBlanks in %llu ms (VBlank %u to %u)", (unsigned)(cv - pc.unpaced_v0),
                     (unsigned long long)(SDL_GetTicks() - pc.unpaced_t0), (unsigned)pc.unpaced_v0, (unsigned)cv);
        }
        pc.unpaced = un;
    }
}
