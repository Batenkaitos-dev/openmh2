/*
 * Save states, the host's side: the keys, the slots, the files and the messages. The core makes and loads the
 * states (its kernel/state.c); this file asks it through the IPC (Mh2IpcState), one request at a time, and tells the
 * player what came of it. The renderer's part of a state travels in the GS ring (gs.c, MH2IPC_REC_STATE_GET and _SET).
 *
 * The keys (rebindable: the settings state_key_save, state_key_load, state_key_next, state_key_prev; "none" unbinds):
 * Home saves to the current slot, End loads it, Page Down and Page Up choose the next and the previous slot. They
 * are off while the keyboard types (keyboard.c), like the development hotkeys. A slot is a file in the states
 * folder (the setting states; default: states in the user data folder, states-play for the development build), named
 * slot01.mh2state to slot10.mh2state. A save is written under a temporary name and renamed when the core
 * says it went through, so a failed save leaves the slot as it was.
 *
 * The host refuses a request, with its reason on the picture and in the log, while a movie is open (the core refuses
 * too, at the VBlank), while another request waits for its answer, and while a menu of the host is up. The core
 * has its own refusals (another build or setting made the state; a card file is open), which come back in the answer.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mh2host.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

#define SLOTS 10

static struct {
    SDL_Scancode key_save, key_load, key_next, key_prev;
    char dir[1100];             /* the folder in use, without a trailing separator; "" none */
    int started;
    int slot;                   /* 1 to SLOTS */
    uint32_t seq;               /* the last request sent */
    int flight;                 /* MH2IPC_ST_SAVE or _LOAD while one waits for its answer */
    int flight_slot;
    char tmp[1300], final[1300];
} st = {.key_save = SDL_SCANCODE_HOME, .key_load = SDL_SCANCODE_END, .key_next = SDL_SCANCODE_PAGEDOWN,
        .key_prev = SDL_SCANCODE_PAGEUP, .slot = 1};

/* settings.c: state_key_save/_load/_next/_prev = KEY or none. 0; -1 not ours; -2 no such key. */
int states_setting(const char *key, const char *value)
{
    SDL_Scancode *k = NULL;
    if (!strcmp(key, "state_key_save"))
        k = &st.key_save;
    else if (!strcmp(key, "state_key_load"))
        k = &st.key_load;
    else if (!strcmp(key, "state_key_next"))
        k = &st.key_next;
    else if (!strcmp(key, "state_key_prev"))
        k = &st.key_prev;
    if (!k)
        return -1;
    if (!SDL_strcasecmp(value, "none")) {
        *k = SDL_SCANCODE_UNKNOWN;
        return 0;
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(value);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return -2;
    *k = sc;
    return 0;
}

const char *states_key_text(const char *key)
{
    return states_key_name(!strcmp(key, "state_key_save") ? STATE_ACT_SAVE : !strcmp(key, "state_key_load") ? STATE_ACT_LOAD
                           : !strcmp(key, "state_key_next") ? STATE_ACT_NEXT : STATE_ACT_PREV);
}

/* The key names, for the log and the guide's text. */
const char *states_key_name(int which)
{
    SDL_Scancode sc = which == STATE_ACT_SAVE ? st.key_save : which == STATE_ACT_LOAD ? st.key_load
                      : which == STATE_ACT_NEXT ? st.key_next : st.key_prev;
    const char *n = sc == SDL_SCANCODE_UNKNOWN ? NULL : SDL_GetScancodeName(sc);
    return n && n[0] ? n : "none";
}

/* A menu of the host is up (the launcher): no state while it is. */
static int host_menu_up(void)
{
    return 0;
}

static void say(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void say(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    host_log("save state: %s", msg);
    present_note(msg);
}

static void slot_path(int slot, int tmp, char *out, size_t cap)
{
    snprintf(out, cap, "%s" SEP "slot%02d.mh2state%s", st.dir, slot, tmp ? ".tmp" : "");
}

/* The folder, once: the setting, else the user data folder's (none in an automated run: the guard). */
static void start(void)
{
    if (st.started)
        return;
    st.started = 1;
    if (g_cfg.states_dir && g_cfg.states_dir[0]) {
        snprintf(st.dir, sizeof(st.dir), "%s", g_cfg.states_dir);
    } else if (play_data_dir()) {
        snprintf(st.dir, sizeof(st.dir), "%sstates", play_data_dir());
    }
    size_t n = strlen(st.dir);
    while (n > 1 && (st.dir[n - 1] == '/' || st.dir[n - 1] == '\\'))
        st.dir[--n] = 0;
    if (st.dir[0])
        host_log("save states: folder %s; %s saves, %s loads, %s and %s choose the slot", st.dir, states_key_name(STATE_ACT_SAVE),
                 states_key_name(STATE_ACT_LOAD), states_key_name(STATE_ACT_NEXT), states_key_name(STATE_ACT_PREV));
    else
        host_log("save states: no folder (this is an automated run: name one with states = FOLDER)");
}

/* "slot 3 (saved 2026-11-04 12:30)" or "slot 3 (empty)". */
static void describe(int slot, char *out, size_t cap)
{
    char path[1300];
    SDL_PathInfo info;
    slot_path(slot, 0, path, sizeof(path));
    if (!st.dir[0] || !SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
        snprintf(out, cap, "slot %d (empty)", slot);
        return;
    }
    time_t t = (time_t)(info.modify_time / 1000000000ll);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char when[40];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    snprintf(out, cap, "slot %d (saved %s)", slot, when);
}

static void request(int kind, int slot)
{
    char why[160] = "";
    Mh2IpcState *s = &g_ipc->state;
    start();
    unsigned int cs = mh2ipc_load(&g_ipc->core_state);
    if (cs != MH2IPC_STATE_RUNNING)
        return;
    if (!st.dir[0]) {
        say("no folder for states (this is an automated run)");
        return;
    }
    if (st.flight) {
        say("wait: the last %s is not done", st.flight == MH2IPC_ST_SAVE ? "save" : "load");
        return;
    }
    if (movie_active()) {
        snprintf(why, sizeof(why), "not while a movie is open");
    } else if (host_menu_up()) {
        snprintf(why, sizeof(why), "not while a menu is open");
    }
    if (why[0]) {
        if (kind == MH2IPC_ST_SAVE)
            say("not saved to slot %d: %s", slot, why);
        else
            say("slot %d not loaded: %s", slot, why);
        return;
    }
    slot_path(slot, 0, st.final, sizeof(st.final));
    slot_path(slot, 1, st.tmp, sizeof(st.tmp));
    if (kind == MH2IPC_ST_LOAD) {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(st.final, &info) || info.type != SDL_PATHTYPE_FILE) {
            say("slot %d is empty", slot);
            return;
        }
    } else if (!SDL_CreateDirectory(st.dir)) {
        say("not saved: cannot make the folder %s (%s)", st.dir, SDL_GetError());
        return;
    }
    snprintf(s->req_path, sizeof(s->req_path), "%s", kind == MH2IPC_ST_SAVE ? st.tmp : st.final);
    s->req_kind = (uint32_t)kind;
    st.flight = kind;
    st.flight_slot = slot;
    mh2ipc_store(&s->req_seq, ++st.seq);   /* last: the core takes it at its next VBlank */
}

void states_action(int what, int arg)
{
    start();
    switch (what) {
    case STATE_ACT_SAVE:
        request(MH2IPC_ST_SAVE, st.slot);
        break;
    case STATE_ACT_LOAD:
        request(MH2IPC_ST_LOAD, st.slot);
        break;
    case STATE_ACT_NEXT:
    case STATE_ACT_PREV:
    case STATE_ACT_SET: {
        char d[100];
        st.slot = what == STATE_ACT_SET ? (arg < 1 ? 1 : arg > SLOTS ? SLOTS : arg)
                  : what == STATE_ACT_NEXT ? st.slot % SLOTS + 1 : (st.slot + SLOTS - 2) % SLOTS + 1;
        describe(st.slot, d, sizeof(d));
        say("%s", d);
        break;
    }
    default:
        break;
    }
}

/* The main loop's key event: 1 when it was one of ours. */
int states_key(const SDL_Event *e)
{
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat || kbd_typing())
        return 0;
    SDL_Scancode sc = e->key.scancode;
    if (sc == SDL_SCANCODE_UNKNOWN)
        return 0;
    if (sc == st.key_save)
        states_action(STATE_ACT_SAVE, 0);
    else if (sc == st.key_load)
        states_action(STATE_ACT_LOAD, 0);
    else if (sc == st.key_next)
        states_action(STATE_ACT_NEXT, 0);
    else if (sc == st.key_prev)
        states_action(STATE_ACT_PREV, 0);
    else
        return 0;
    return 1;
}

#ifdef MH2_STATES_TEST
/* A build made only to try the release host's keys' path (never shipped: the macro is set by hand for one build directory): the
 * environment variable MH2_STATES_TEST lists "S<vblank>" (save) and "L<vblank>" (load), comma separated, each done once when the
 * last VSync's VBlank reaches it, as the key would have. */
static void test_script(void)
{
    static struct { char what; unsigned long long at; int done; } step[16];
    static int n, init;
    if (!init) {
        init = 1;
        const char *e = hfs_getenv("MH2_STATES_TEST");
        while (e && *e && n < 16) {
            step[n].what = *e++;
            step[n].at = strtoull(e, (char **)&e, 10);
            n++;
            if (*e == ',')
                e++;
        }
    }
    for (int i = 0; i < n; i++)
        if (!step[i].done && gs_last_vblank() + 1 >= step[i].at && !st.flight) {
            step[i].done = 1;
            states_action(step[i].what == 'L' ? STATE_ACT_LOAD : STATE_ACT_SAVE, 0);
        }
}
#endif

/* Each pass of the main loop: the core's answer to a request. */
void states_update(void)
{
    start();
#ifdef MH2_STATES_TEST
    test_script();
#endif
    if (!st.flight || mh2ipc_load(&g_ipc->state.ack_seq) != st.seq)
        return;
    Mh2IpcState *s = &g_ipc->state;
    int kind = st.flight, slot = st.flight_slot;
    unsigned long long vb = (unsigned long long)s->ack_vblank_hi << 32 | s->ack_vblank_lo;
    char msg[MH2IPC_STATE_MSG];
    snprintf(msg, sizeof(msg), "%s", s->ack_message);
    int refused = s->ack_result != MH2IPC_ST_DONE;
    st.flight = 0;
    if (kind == MH2IPC_ST_SAVE) {
        if (refused) {
            SDL_RemovePath(st.tmp);
            say("not saved to slot %d: %s", slot, msg);
        } else if (!SDL_RenamePath(st.tmp, st.final)) {
            SDL_RemovePath(st.tmp);
            say("not saved to slot %d: %s", slot, SDL_GetError());
        } else {
            say("saved to slot %d (VBlank %llu)", slot, vb);
        }
    } else if (refused) {
        say("slot %d not loaded: %s", slot, msg);
    } else {
        say("loaded slot %d (VBlank %llu)", slot, vb);
    }
}
