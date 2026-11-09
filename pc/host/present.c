/*
 * The window and the presentation: the GS thread submits each scanned-out picture,
 * and the main thread draws the latest one with SDL's renderer, at 4:3 and
 * letterboxed into the window (at 16:9 while widescreen is on and no movie plays). F11 or Alt+Enter: borderless fullscreen,
 * with VSync on there (a window's compositor shows whole pictures, so VSync
 * stays off in a window), unless --vsync or the setting vsync says on or off.
 * The core's VBlanks pace the game, never the display.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hfr.h"
#include "mh2host.h"

uint32_t g_present_event;
#define HFR_QUEUE 12

static struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Mutex *mu;
    unsigned char *pixels;   /* the latest picture, RGBA8, width * 4 a row */
    uint32_t width, height;
    size_t cap;
    int fresh;
    int shown;
    int movie;   /* the latest picture is a movie one */
    int typing;                     /* the USB keyboard types (keyboard.c) */
    char freecam[160];              /* the freecam's line (freecam.c), empty: it is off */
    char pace[60];                  /* the pause's or fast-forward's words (pace.c), empty: none */
    int dirty;                      /* the window is drawn again with the picture it has */
    char fctitle[100];              /* what the title says of it: FREECAM, FREECAM (PAUSED, 3 steps), PHOTO */
    char typing_note[100];          /* its line over the picture */
    char title_status[200];         /* a status of the host's, in the title */
    char note[200];                 /* a note for a few seconds (the save states'), over the picture and in the title */
    Uint64 note_until;
    int note_in_title;
    /* 60, 120 and 144 (hfr.h): the GS thread queues pictures with the time each is for, and the
     * main thread shows, for each refresh, the picture of greatest time not over that refresh's time plus half a period */
    struct {
        int on;
        int rate;                    /* the rate chosen */
        struct Slot {
            unsigned char *px;
            size_t cap;
            uint32_t w, h;
            uint64_t tau;
            int used, real;
        } slot[HFR_QUEUE];
        double refresh_ns;           /* the display's refresh period */
        int divider;                 /* one picture every divider-th refresh */
        double grid_ref;             /* the time of a refresh (the return of a present that waited), ns of SDL_GetTicksNS; 0: none yet */
        double next;                 /* the refresh the next present is for */
        double pick_at;              /* when to choose and draw it */
        uint64_t shown_tau;
        uint64_t t_check;            /* the next look at the display's refresh rate */
        double last_ret;             /* the return time of the last present (the statistics) */
        double t0;                   /* before the present now going on */
        int blocking;                /* the last present waited for a refresh */
        int vs_dirty;                /* apply_vsync again (the rate changed) */
        uint64_t n_present, n_blocked;
    } hf;
} pr;

static void apply_vsync(bool fullscreen)
{
    /* auto: on in fullscreen; at 60, 120 and 144 also in a window (the timed presenter waits for the refresh in its own loop) */
    int on = g_cfg.vsync < 0 ? (fullscreen || pr.hf.on) : g_cfg.vsync;
    if (pr.renderer)
        SDL_SetRenderVSync(pr.renderer, on ? 1 : 0);
}

static void set_fullscreen(bool on)
{
    if (!pr.window)
        return;
    SDL_SetWindowFullscreen(pr.window, on);
    apply_vsync(on);
    host_log("%s", on ? "fullscreen" : "windowed");
    if (on)
        SDL_HideCursor();
    else
        SDL_ShowCursor();
}

/* Before a message box: a box over a fullscreen window may not be seen. */
void present_leave_fullscreen(void)
{
    if (pr.window && (SDL_GetWindowFlags(pr.window) & SDL_WINDOW_FULLSCREEN))
        set_fullscreen(false);
}


int present_init(void)
{
    g_present_event = SDL_RegisterEvents(1);
    pr.mu = SDL_CreateMutex();
    SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE;
#if defined(MH2_RELEASE) && defined(MH2_FREECAM_TEST)
    {   /* the release test build (set by hand, never shipped): MH2_WINDOW_MODE=hidden|unfocused keeps its window off the keyboard's focus */
        const char *m = hfs_getenv("MH2_WINDOW_MODE");
        if (m && *m)
            wflags |= SDL_WINDOW_NOT_FOCUSABLE | (!strcmp(m, "hidden") ? SDL_WINDOW_HIDDEN : 0);
    }
#endif
    pr.window = SDL_CreateWindow(MH2_WINDOW_TITLE, 960, 720, wflags);
    if (!pr.window) {
        host_log("SDL_CreateWindow: %s", SDL_GetError());
        return -1;
    }
    pr.renderer = SDL_CreateRenderer(pr.window, NULL);
    if (!pr.renderer) {
        host_log("SDL_CreateRenderer: %s", SDL_GetError());
        return -1;
    }
    host_log("renderer: %s", SDL_GetRendererName(pr.renderer));
    apply_vsync(false);
    SDL_SetRenderDrawColor(pr.renderer, 0, 0, 0, 255);
    SDL_RenderClear(pr.renderer);
    SDL_RenderPresent(pr.renderer);
    if (g_cfg.fullscreen)
        set_fullscreen(true);
    return 0;
}

/* The window's title: the host's status, and "typing" while the keyboard is the game's USB keyboard. */
static void retitle(void)
{
    char t[400];
    int n = snprintf(t, sizeof(t), "%s", MH2_WINDOW_TITLE);
    if (pr.title_status[0])
        n += snprintf(t + n, sizeof(t) - (size_t)n, " - %s", pr.title_status);
    if (pr.typing)
        n += snprintf(t + n, sizeof(t) - (size_t)n, " - %s", pr.typing_note);
    if (pr.freecam[0])
        n += snprintf(t + n, sizeof(t) - (size_t)n, " - %s", pr.fctitle[0] ? pr.fctitle : "FREECAM");
    if (pr.pace[0])
        n += snprintf(t + n, sizeof(t) - (size_t)n, " - %s", pr.pace);
    pr.note_in_title = pr.note[0] && SDL_GetTicks() < pr.note_until;
    if (pr.note_in_title)
        snprintf(t + n, sizeof(t) - (size_t)n, " - %s", pr.note);
    if (pr.window)
        SDL_SetWindowTitle(pr.window, t);
}

void present_set_typing(int on, const char *toggle_key)
{
    pr.typing = on;
    snprintf(pr.typing_note, sizeof(pr.typing_note), "USB keyboard: typing (%s plays)", toggle_key);
    retitle();
}

/* The freecam's line, over the picture's bottom right while it is on, and FREECAM in the title. */
void present_set_freecam(const char *line, const char *title)
{
    snprintf(pr.freecam, sizeof(pr.freecam), "%s", line ? line : "");
    snprintf(pr.fctitle, sizeof(pr.fctitle), "%s", title ? title : "");
    retitle();
}

/* The pause's or fast-forward's words (pace.c): top left over the picture, and in the title. The picture is drawn again with them. */
void present_set_pace(const char *line)
{
    snprintf(pr.pace, sizeof(pr.pace), "%s", line ? line : "");
    pr.dirty = 1;
    retitle();
}

/* The window changed (shown again, resized), or a line did: draw the picture it has once more. */
void present_dirty(void)
{
    pr.dirty = 1;
}

/* The freecam looks with the mouse: relative mode (the pointer hidden and held), or the pointer again. */
void present_relative_mouse(int on)
{
    if (pr.window)
        SDL_SetWindowRelativeMouseMode(pr.window, on != 0);
}

/* A note (a save state's result): over the picture and in the title for four seconds. */
void present_note(const char *msg)
{
    snprintf(pr.note, sizeof(pr.note), "%s", msg);
    pr.note_until = SDL_GetTicks() + 4000;
    pr.dirty = 1;
    retitle();
}


static void draw_osd_line(float x, float y, float scale, const char *text)
{
    SDL_FRect bg = {x - 2 * scale, y - 2 * scale, (float)strlen(text) * 8.0f * scale + 4 * scale, 12.0f * scale};
    SDL_SetRenderDrawBlendMode(pr.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(pr.renderer, 0, 0, 0, 170);
    SDL_RenderFillRect(pr.renderer, &bg);
    SDL_SetRenderScale(pr.renderer, scale, scale);
    SDL_SetRenderDrawColor(pr.renderer, 255, 255, 120, 255);
    SDL_RenderDebugText(pr.renderer, x / scale, y / scale, text);
    SDL_SetRenderScale(pr.renderer, 1.0f, 1.0f);
}
void present_toggle_fullscreen(void)
{
    if (pr.window)
        set_fullscreen(!(SDL_GetWindowFlags(pr.window) & SDL_WINDOW_FULLSCREEN));
}

/* ---- 120 and 144 frames per second ---- */

#ifdef _WIN32
#include <windows.h>
typedef HANDLE(WINAPI *AvSetFn)(LPCWSTR, LPDWORD);
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
#endif

/* The two threads that keep the game's time (the GS thread, which makes the pictures, and the main thread, which shows them) run first on a busy
 * machine: a thread at the normal priority waits a time slice (up to 15 ms) when another program uses all the cores, and a picture that is
 * ready on time is shown a refresh late. Raised priority; on Windows also the "Games" class of the multimedia scheduler (a share of the
 * time reserved for it), and the process is excluded from the power throttling that gives a window that is not in front a coarse timer
 * and the efficiency cores (Windows 11). MH2_RT=0 (development builds) leaves it all as it is. */
void host_realtime_thread(void)
{
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#ifdef _WIN32
    static int process_done;
    if (!process_done) {
        process_done = 1;
        typedef struct {
            ULONG Version;
            ULONG ControlMask;
            ULONG StateMask;
        } Throttle;
        typedef BOOL(WINAPI * SetProcInfo)(HANDLE, int, LPVOID, DWORD);
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        SetProcInfo fn = k ? (SetProcInfo)(void *)GetProcAddress(k, "SetProcessInformation") : NULL;
        if (fn) {
            Throttle t = {1, PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
            fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &t, sizeof(t));
        }
    }
    HMODULE av = LoadLibraryW(L"avrt.dll");
    AvSetFn set = av ? (AvSetFn)(void *)GetProcAddress(av, "AvSetMmThreadCharacteristicsW") : NULL;
    if (set) {
        DWORD idx = 0;
        set(L"Games", &idx);
    }
#endif
}

static double hf_now(void)
{
    return (double)SDL_GetTicksNS();
}

/* The display's refresh period: the window's display, or 60 Hz when SDL cannot say. Main thread. */
static void hf_refresh_update(void)
{
    double hz = 0;
    if (pr.window) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(pr.window);
        const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
        if (m && m->refresh_rate > 20.0f)
            hz = (double)m->refresh_rate;
    }
    if (hz <= 0)
        hz = 60.0;
    SDL_LockMutex(pr.mu);
    pr.hf.refresh_ns = 1e9 / hz;
    pr.hf.divider = pr.hf.rate > 0 && pr.hf.rate <= 60 ? 1 : hfr_divider(hz, pr.hf.rate > 0 ? pr.hf.rate : 120);   /* 60: every refresh looks for the picture of its time */
    SDL_UnlockMutex(pr.mu);
    pr.hf.t_check = SDL_GetTicksNS() + 2000000000ull;
}

/* The timed presenter on (RATE 60, 120 or 144) or off (0: one picture as it comes, at 30). */
void present_hfr(int rate)
{
    if (!pr.mu)
        return;
    SDL_LockMutex(pr.mu);
    pr.hf.rate = rate;
    pr.hf.on = rate >= 60;
    if (!pr.hf.on)
        for (int i = 0; i < HFR_QUEUE; i++)
            pr.hf.slot[i].used = 0;
    pr.hf.pick_at = 0;
    pr.hf.t_check = 0;
    pr.hf.vs_dirty = 1;   /* the main thread applies VSync again */
    SDL_UnlockMutex(pr.mu);
}

/* The refresh grid for the GS thread plan: the time of a refresh (ns of SDL_GetTicksNS), the display period and the divider. 0: no present has waited for a refresh yet. */
int present_grid(double *ref_ns, double *refresh_ns, int *divider)
{
    if (!pr.mu)
        return 0;
    SDL_LockMutex(pr.mu);
    int ok = pr.hf.grid_ref > 0;
    *ref_ns = pr.hf.grid_ref;
    *refresh_ns = pr.hf.refresh_ns > 0 ? pr.hf.refresh_ns : 1e9 / 60.0;
    *divider = pr.hf.divider > 0 ? pr.hf.divider : 1;
    SDL_UnlockMutex(pr.mu);
    return ok;
}

/* The display refresh rate as last read (Hz). */
double present_refresh_hz(void)
{
    return pr.hf.refresh_ns > 0 ? 1e9 / pr.hf.refresh_ns : 60.0;
}

/* A picture for the time TAU_NS (SDL_GetTicksNS): the real one of a game frame, or an in-between one. A real picture takes away the in-between ones of times after it. */
void present_queue(const void *pixels, uint32_t width, uint32_t height, uint32_t stride, uint64_t tau_ns, int real)
{
    if (!pr.mu || !width || !height)
        return;
    SDL_LockMutex(pr.mu);
    if (!pr.hf.on) {
        SDL_UnlockMutex(pr.mu);
        return;
    }
    if (real)
        for (int i = 0; i < HFR_QUEUE; i++)
            if (pr.hf.slot[i].used && pr.hf.slot[i].tau > tau_ns)
                pr.hf.slot[i].used = 0;
    int k = -1;
    for (int i = 0; i < HFR_QUEUE && k < 0; i++)
        if (!pr.hf.slot[i].used)
            k = i;
    if (k < 0) {   /* full: the oldest goes */
        k = 0;
        for (int i = 1; i < HFR_QUEUE; i++)
            if (pr.hf.slot[i].tau < pr.hf.slot[k].tau)
                k = i;
    }
    struct Slot *sl = &pr.hf.slot[k];
    size_t need = (size_t)width * height * 4;
    if (need > sl->cap) {
        unsigned char *p = realloc(sl->px, need);
        if (!p) {
            SDL_UnlockMutex(pr.mu);
            return;
        }
        sl->px = p;
        sl->cap = need;
    }
    for (uint32_t y = 0; y < height; y++)
        memcpy(sl->px + (size_t)y * width * 4, (const unsigned char *)pixels + (size_t)y * stride, (size_t)width * 4);
    sl->w = width;
    sl->h = height;
    sl->tau = tau_ns;
    sl->real = real;
    sl->used = 1;
    SDL_UnlockMutex(pr.mu);
}

/* The main loop wait for events, in ms: it ends in time for the next picture. */
int present_wait_ms(void)
{
    if (!pr.hf.on || !pr.hf.pick_at)
        return 4;
    double d = (pr.hf.pick_at - hf_now()) / 1e6 - 1.3;
    return d < 0 ? 0 : d > 4 ? 4 : (int)d;
}

/* Is it time to choose and draw a picture? Chooses it: the picture goes into pr.pixels (swapped, not copied) and fresh is set. Called
 * with the mutex NOT held; returns 1 when present_draw goes on to draw (a new picture, or a redraw that was asked for). */
static int hf_pick(void)
{
    double now = hf_now();
    if ((uint64_t)now >= pr.hf.t_check || !pr.hf.refresh_ns)
        hf_refresh_update();
    const double rd = pr.hf.refresh_ns, R = rd * pr.hf.divider;
    const double margin = 2.5e6 < 0.4 * R ? 2.5e6 : 0.4 * R;
    if (!pr.hf.pick_at) {
        pr.hf.next = now + R;
        pr.hf.pick_at = pr.hf.next - margin;
    }
    if (now < pr.hf.pick_at) {
        if (pr.hf.pick_at - now < 1.3e6)   /* the last stretch is waited for here, with the precise timer */
            SDL_DelayPrecise((Uint64)(pr.hf.pick_at - now));
        else {
            SDL_LockMutex(pr.mu);
            int dirty = pr.dirty && pr.texture;
            SDL_UnlockMutex(pr.mu);
            return dirty;   /* a redraw asked for is not held back */
        }
    }
    SDL_LockMutex(pr.mu);
    int chosen = -1;
    const double limit = pr.hf.next + R / 2.0;
    for (int i = 0; i < HFR_QUEUE; i++) {
        struct Slot *sl = &pr.hf.slot[i];
        if (sl->used && (double)sl->tau <= limit && (chosen < 0 || sl->tau > pr.hf.slot[chosen].tau))
            chosen = i;
    }
    int draw = 0;
    if (chosen >= 0) {
        struct Slot *sl = &pr.hf.slot[chosen];
        for (int i = 0; i < HFR_QUEUE; i++)
            if (pr.hf.slot[i].used && pr.hf.slot[i].tau < sl->tau) {
                pr.hf.slot[i].used = 0;
            }
        if (sl->tau != pr.hf.shown_tau) {
            unsigned char *tp = pr.pixels;
            size_t tc = pr.cap;
            pr.pixels = sl->px;
            pr.cap = sl->cap;
            sl->px = tp;
            sl->cap = tc;
            pr.width = sl->w;
            pr.height = sl->h;
            pr.movie = 0;
            pr.fresh = 1;
            pr.hf.shown_tau = sl->tau;
            draw = 1;
        }
        sl->used = 0;
    }
    draw = draw || (pr.dirty && pr.texture);
    SDL_UnlockMutex(pr.mu);
    if (!draw) {   /* nothing new for this refresh: the grid goes on by the clock */
        pr.hf.next += R;
        if (pr.hf.next + R / 2.0 < now)
            pr.hf.next = now + R;
        pr.hf.pick_at = pr.hf.next - margin;
    }
    return draw;
}

/* After a present: one that waited for the refresh (longer than a quarter of the period) tells the time of a refresh; the next is a period on. */
static void hf_presented(double t0, double t1)
{
    const double R = pr.hf.refresh_ns * pr.hf.divider;
    const double margin = 2.5e6 < 0.4 * R ? 2.5e6 : 0.4 * R;
    int blocked = t1 - t0 > 0.25 * R;
    SDL_LockMutex(pr.mu);
    if (blocked)
        pr.hf.grid_ref = t1;
    pr.hf.next = (blocked ? t1 : pr.hf.next) + R;
    if (pr.hf.next + R / 2.0 < t1)
        pr.hf.next = t1 + R;
    pr.hf.pick_at = pr.hf.next - margin;
    SDL_UnlockMutex(pr.mu);
    pr.hf.n_present++;
    pr.hf.n_blocked += blocked;
    pr.hf.blocking = blocked;
    pr.hf.last_ret = t1;
}

void present_submit(const void *pixels, uint32_t width, uint32_t height, uint32_t stride, uint64_t vblank, int movie)
{
    (void)vblank;
    if (!pr.mu || !width || !height)
        return;
    if (pr.hf.on) {   /* a movie picture (or anything the GS thread shows at once): the real picture, now */
        present_queue(pixels, width, height, stride, SDL_GetTicksNS(), 1);
        return;
    }
    SDL_LockMutex(pr.mu);
    size_t need = (size_t)width * height * 4;
    if (need > pr.cap) {
        unsigned char *p = realloc(pr.pixels, need);
        if (!p) {
            SDL_UnlockMutex(pr.mu);
            return;
        }
        pr.pixels = p;
        pr.cap = need;
    }
    for (uint32_t y = 0; y < height; y++)
        memcpy(pr.pixels + (size_t)y * width * 4, (const unsigned char *)pixels + (size_t)y * stride, (size_t)width * 4);
    pr.width = width;
    pr.height = height;
    pr.movie = movie;
    int pending = pr.fresh;   /* a picture waits to be drawn: its event is in the queue, one is enough (unpaced, hundreds a second) */
    pr.fresh = 1;
    SDL_UnlockMutex(pr.mu);
    if (pending)
        return;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = g_present_event;
    SDL_PushEvent(&e);
}

void present_draw(void)
{
    if (!pr.renderer)
        return;
    if (pr.hf.vs_dirty) {
        pr.hf.vs_dirty = 0;
        apply_vsync(pr.window && (SDL_GetWindowFlags(pr.window) & SDL_WINDOW_FULLSCREEN));
    }
    if (pr.hf.on && !hf_pick())
        return;
    SDL_LockMutex(pr.mu);
    if (pr.note[0] && !pr.dirty && SDL_GetTicks() >= pr.note_until) {   /* the note has run out: the picture is drawn without it */
        pr.note[0] = 0;
        pr.dirty = 1;
    }
    if (!pr.fresh && !(pr.dirty && pr.texture)) {
        SDL_UnlockMutex(pr.mu);
        return;
    }
    int fresh = pr.fresh;
    pr.fresh = 0;
    pr.dirty = 0;
    if (pr.texture && (pr.texture->w != (int)pr.width || pr.texture->h != (int)pr.height)) {
        SDL_DestroyTexture(pr.texture);
        pr.texture = NULL;
    }
    if (!pr.texture) {
        pr.texture = SDL_CreateTexture(pr.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)pr.width,
                                       (int)pr.height);
        if (pr.texture) {
            SDL_SetTextureBlendMode(pr.texture, SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(pr.texture, SDL_SCALEMODE_LINEAR);
        }
    }
    if (pr.texture && fresh)
        SDL_UpdateTexture(pr.texture, NULL, pr.pixels, (int)pr.width * 4);
    uint32_t w = pr.width, h = pr.height;
    int movie = pr.movie;
    SDL_UnlockMutex(pr.mu);
    if (!pr.texture)
        return;
    SDL_SetRenderDrawColor(pr.renderer, 0, 0, 0, 255);
    SDL_RenderClear(pr.renderer);
    /* the largest 4:3 area of the window, centred (a TV's shape); 16:9 while the widescreen setting is on and no
     * movie is shown (the movies keep their 4:3, pillarboxed in the wide window) */
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(pr.renderer, &ow, &oh);
    SDL_FRect dst = {0, 0, (float)ow, (float)oh};
    int aw = g_cfg.widescreen && !movie ? 16 : 4, ah = g_cfg.widescreen && !movie ? 9 : 3;
    if (ow * ah > oh * aw) {
        dst.w = (float)oh * (float)aw / (float)ah;
        dst.x = ((float)ow - dst.w) / 2.0f;
    } else {
        dst.h = (float)ow * (float)ah / (float)aw;
        dst.y = ((float)oh - dst.h) / 2.0f;
    }
    SDL_RenderTexture(pr.renderer, pr.texture, NULL, &dst);
    if (pr.note[0] && SDL_GetTicks() < pr.note_until) {   /* top centre */
        float sc = dst.w >= 1200.0f ? 2.0f : 1.0f;
        draw_osd_line(dst.x + (dst.w - (float)strlen(pr.note) * 8.0f * sc) / 2.0f, dst.y + 6 + 16 * sc, sc, pr.note);
    } else if (pr.note_in_title) {
        retitle();   /* the note has run out: the title loses it */
    }
    if (pr.freecam[0]) {   /* bottom right */
        float sc = dst.w >= 1200.0f ? 2.0f : 1.0f;
        draw_osd_line(dst.x + dst.w - 6 - (float)strlen(pr.freecam) * 8.0f * sc, dst.y + dst.h - 16 * sc, sc, pr.freecam);
    }
    if (pr.pace[0]) {   /* top left */
        float sc = dst.w >= 1200.0f ? 2.0f : 1.0f;
        draw_osd_line(dst.x + 6, dst.y + 6, sc, pr.pace);
    }
    if (pr.typing) {   /* top right: the other lines are at the left */
        float sc = dst.w >= 1200.0f ? 2.0f : 1.0f;
        draw_osd_line(dst.x + dst.w - 6 - (float)strlen(pr.typing_note) * 8.0f * sc, dst.y + 6, sc, pr.typing_note);
    }
    double hf_t0 = pr.hf.on ? hf_now() : 0;
    SDL_RenderPresent(pr.renderer);
    if (pr.hf.on)
        hf_presented(hf_t0, hf_now());
    if (!pr.shown++)
        host_log("first picture shown (%ux%u)", w, h);
}

void present_shutdown(void)
{
    for (int i = 0; i < HFR_QUEUE; i++)
        free(pr.hf.slot[i].px);
    if (pr.texture)
        SDL_DestroyTexture(pr.texture);
    if (pr.renderer)
        SDL_DestroyRenderer(pr.renderer);
    if (pr.window)
        SDL_DestroyWindow(pr.window);
    if (pr.mu)
        SDL_DestroyMutex(pr.mu);
    free(pr.pixels);
    memset(&pr, 0, sizeof(pr));
}
