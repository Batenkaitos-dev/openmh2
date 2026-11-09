/*
 * The GS thread: the GS ring's consumer. GIF data and privileged writes go
 * to the renderer, mh2gs (paraLLEl-GS on Vulkan, pc/gs/,
 * loaded at run time so that it stays a replaceable LGPL library); a VSync
 * scans out and hands the previous picture to the window (one VBlank late,
 * while the GPU draws the next, as the original does); a readback asks
 * the renderer for the transfer's data, or gives zeros without one.
 *
 * Every call into mh2gs is made from this thread, mh2gs_init and
 * mh2gs_shutdown too: Granite (paraLLEl-GS's Vulkan layer) registers the
 * thread that makes the device as its thread 0 (a thread-local index), and
 * logs "Thread does not exist in thread manager" for every command buffer
 * another thread asks for. gs_init starts the thread and waits for its
 * answer, before the core starts.
 */
#include <stdio.h>
#include <string.h>

#include "hfr.h"
#define HF_STAT_MAX 60000   /* (development builds) the values the percentiles are made of */
#include "mh2host.h"
#include "packs.h"
#include "../../pc/gs/mh2gs.h"

#ifdef _WIN32
#define MH2GS_LIB "mh2gs.dll"
#else
#define MH2GS_LIB "mh2gs.so"
#endif

static struct {
    SDL_SharedObject *lib;
    mh2gs_gif_fn gif;
    mh2gs_priv_write_fn priv_write;
    mh2gs_vsync_fn vsync;
    mh2gs_get_frame_fn get_frame;
    mh2gs_read_fifo_fn read_fifo;
    mh2gs_shutdown_fn shutdown;
    mh2gs_init_fn init;
    mh2gs_state_get_fn state_get;
    mh2gs_state_set_fn state_set;
    mh2gs_state_size_fn state_size;
    mh2gs_interp_status_fn interp_status;
    mh2gs_interp_set_alphas_fn set_alphas;
    mh2gs_interp_mids_fn mids;
    mh2gs_interp_stop_fn interp_stop;
    mh2gs_frame_poll_fn frame_poll;
    mh2gs_set_notify_fn set_notify;
    mh2gs_camera_fn camera;
    /* the VSync being made (on_vsync): the pictures the library tells of while mh2gs_vsync runs (gs_notify) are entered at once */
    uint64_t cur_vblank;
    int cur_real, note_real, in_call;   /* in_call: inside mh2gs_vsync (the step down waits for its return) */
    uint32_t note_mids;
    /* the frame rate in use (30, 60, 120, 144) and, above 30, the timed presenter's schedule */
    int rate;
    int hf_on;                  /* the in-between pictures are made for the timed presenter (present.c) */
    HfrClock clk;
    double hf_b;                /* the time the real picture of this VSync is shown */
    double hf_tau[HFR_MAX];     /* the times of the pictures asked for */
    uint16_t hf_alpha[HFR_MAX];
    int hf_n;
    struct HfPend {
        uint64_t id, vblank;
        double tau;
        int real;
        unsigned alpha;
        int unfinished_late;   /* a look 3 ms or less before its time found the copy not finished */
    } pend[2 * HFR_MAX + 4];
    int npend;
    uint32_t late_hist, late_n;
    double last_look;           /* the time of the last look at the copies (hf_poll) */
    double hf_lag;              /* ns from the VSync to the time its real picture is shown: a VBlank, as at 60, and more while pictures are late */
    uint32_t interp_state;   /* the last state of the in-between picture the log has told of */
    char identity[32];   /* the renderer's build and the settings that change its state (save states) */
    int ok;
    SDL_Semaphore *ready;
    SDL_Thread *thread;
    SDL_AtomicInt stop;
    SDL_AtomicU32 vsyncs;
    SDL_AtomicU32 last_vblank;   /* the VBlank of the last VSync record: it goes back with a load of a save state */
    uint64_t prev_id, prev_vblank;
    /* a photo (freecam.c, photo mode): one at a time, the first picture at or after photo_after */
    SDL_AtomicInt photo_req;     /* 1 while pending */
    SDL_AtomicInt photo_done;    /* 1 saved, -1 failed, 0 none (gs_photo_result takes it) */
    SDL_AtomicU32 photo_w, photo_h;
    uint64_t photo_after;
    char photo_path[1500];
    /* the previous VSync's movie picture (movie.c), shown in place of the GS's at the next one */
    int prev_movie;
    int flip;       /* display registers were written since the last VSync: the game flipped */
    uint8_t *movie;
    size_t movie_cap;
    uint32_t movie_w, movie_h;
} gs;

static void gs_notify(void *user, uint64_t id, int index);


/* The texture code's lines (mh2tex): in the log. */
static void on_gs_log(const char *line, void *user)
{
    (void)user;
    host_log("%s", line);
}

static void on_event(int event, uint64_t payload, void *user)
{
    /* SIGNAL, FINISH and LABEL are the core's (it finds them in the stream) */
    (void)event;
    (void)payload;
    (void)user;
}

/* The main thread: loads mh2gs and finds its functions. 0 = loaded. */
static int gs_load(void)
{
    if (g_cfg.gs_off) {
        host_log("graphics off (--gs-off)");
        return -1;
    }
    const char *base = SDL_GetBasePath();
    char path[1200];
    snprintf(path, sizeof(path), "%s%s", base ? base : "", MH2GS_LIB);
    gs.lib = SDL_LoadObject(path);
    if (!gs.lib) {
        host_log("cannot load %s: %s; running without graphics", path, SDL_GetError());
        return -1;
    }
    /* the library's ABI first: a config of another layout must not be handed to it */
    mh2gs_abi_version_fn abi = (mh2gs_abi_version_fn)SDL_LoadFunction(gs.lib, "mh2gs_abi_version");
    if (!abi || abi() != MH2GS_ABI_VERSION) {
        host_log("%s is from another version (ABI %d, this host wants %d); running without graphics", path,
                 abi ? abi() : 0, MH2GS_ABI_VERSION);
        SDL_UnloadObject(gs.lib);
        gs.lib = NULL;
        return -1;
    }
    gs.init = (mh2gs_init_fn)SDL_LoadFunction(gs.lib, "mh2gs_init");
    gs.gif = (mh2gs_gif_fn)SDL_LoadFunction(gs.lib, "mh2gs_gif");
    gs.priv_write = (mh2gs_priv_write_fn)SDL_LoadFunction(gs.lib, "mh2gs_priv_write");
    gs.vsync = (mh2gs_vsync_fn)SDL_LoadFunction(gs.lib, "mh2gs_vsync");
    gs.get_frame = (mh2gs_get_frame_fn)SDL_LoadFunction(gs.lib, "mh2gs_get_frame");
    gs.read_fifo = (mh2gs_read_fifo_fn)SDL_LoadFunction(gs.lib, "mh2gs_read_fifo");
    gs.shutdown = (mh2gs_shutdown_fn)SDL_LoadFunction(gs.lib, "mh2gs_shutdown");
    gs.state_get = (mh2gs_state_get_fn)SDL_LoadFunction(gs.lib, "mh2gs_state_get");
    gs.state_set = (mh2gs_state_set_fn)SDL_LoadFunction(gs.lib, "mh2gs_state_set");
    gs.state_size = (mh2gs_state_size_fn)SDL_LoadFunction(gs.lib, "mh2gs_state_size");
    gs.interp_status = (mh2gs_interp_status_fn)SDL_LoadFunction(gs.lib, "mh2gs_interp_status_get");
    gs.set_alphas = (mh2gs_interp_set_alphas_fn)SDL_LoadFunction(gs.lib, "mh2gs_interp_set_alphas");
    gs.mids = (mh2gs_interp_mids_fn)SDL_LoadFunction(gs.lib, "mh2gs_interp_mids");
    gs.interp_stop = (mh2gs_interp_stop_fn)SDL_LoadFunction(gs.lib, "mh2gs_interp_stop");
    gs.frame_poll = (mh2gs_frame_poll_fn)SDL_LoadFunction(gs.lib, "mh2gs_frame_poll");
    gs.set_notify = (mh2gs_set_notify_fn)SDL_LoadFunction(gs.lib, "mh2gs_set_notify");   /* optional */
    gs.camera = (mh2gs_camera_fn)SDL_LoadFunction(gs.lib, "mh2gs_camera");   /* optional */
    if (!gs.state_get || !gs.state_set || !gs.state_size) {
        host_log("%s has no save state functions; running without graphics", path);
        SDL_UnloadObject(gs.lib);
        gs.lib = NULL;
        return -1;
    }
    if (!gs.init || !gs.gif || !gs.priv_write || !gs.vsync || !gs.get_frame || !gs.read_fifo || !gs.shutdown) {
        host_log("%s is incomplete or from another version; running without graphics", path);
        SDL_UnloadObject(gs.lib);
        gs.lib = NULL;
        return -1;
    }
    host_log("paraLLEl-GS renderer loaded (%s)", path);
    return 0;
}

/* The GS thread: makes the renderer (gs.ok), before it reads the ring. */
static void gs_make_renderer(void)
{
    if (!gs.lib)
        return;
    mh2gs_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = MH2GS_ABI_VERSION;
    /* the enhancements (off by default): drawing only. The renderer's view of VRAM, which the guest
     * reads back, stays single-sampled; the GS stream the core sends is the same */
    cfg.super_sampling = g_cfg.ssaa >= 2 ? (uint32_t)g_cfg.ssaa : 1;
    cfg.high_resolution_scanout = g_cfg.hires ? 1 : 0;
    /* the in-between pictures (fps 60, 120 or 144; off by default): one renderer at every rate, the alphas planned here (hf_before_vsync) and
     * the pictures shown by the timed presenter (present.c); 60 is the one picture between each two (alpha 1/2); display only */
    gs.rate = 30;   /* 1.0.0 draws 30 pictures a second: no in-between pictures, no second renderer, no speculation (the library's interpolate is off) */
    cfg.interpolate = gs.rate >= 60 ? MH2GS_INTERP_ON : MH2GS_INTERP_OFF;
    if (gs.rate > 30)
        cfg.log = on_gs_log;   /* its summary at the end */
    cfg.on_event = on_event;
    /* the textures (off unless asked; the folders are the textures folder's dump and packs) */
    char tex_dump[1100] = "";
    enum { MAX_PACKS = 64 };
    static char pack_names[MAX_PACKS][PACK_NAME_MAX], pack_dirs[MAX_PACKS][1100];
    const char *pack_ptrs[MAX_PACKS];
    if (g_cfg.texture_dump || g_cfg.texture_packs) {
        const char *root = g_cfg.textures;
        char under_data[1024];
        if (!root && play_data_dir()) {
            snprintf(under_data, sizeof(under_data), "%stextures", play_data_dir());
            root = under_data;
        }
        if (!root)
            host_log("textures: no folder (textures = FOLDER, --textures FOLDER); the textures are neither written nor replaced");
        else {
            if (g_cfg.texture_dump)
                snprintf(tex_dump, sizeof(tex_dump), "%s/dump", root);
            if (g_cfg.texture_packs) {
                /* the enabled folders of <root>/packs, in load order (packs.h); the earlier pack wins a texture */
                char packs_root[1100];
                snprintf(packs_root, sizeof(packs_root), "%s/packs", root);
                int n = packs_enabled(packs_root, pack_names, MAX_PACKS);
                for (int i = 0; i < n; i++) {
                    snprintf(pack_dirs[i], sizeof(pack_dirs[i]), "%s/%s", packs_root, pack_names[i]);
                    pack_ptrs[i] = pack_dirs[i];
                    host_log("texture pack %d: %s", i + 1, pack_names[i]);
                }
                if (!n)
                    host_log("texture packs: none in %s", packs_root);
                cfg.texture_pack_dirs = pack_ptrs;
                cfg.texture_pack_count = (uint32_t)n;
            }
            cfg.texture_dump_dir = tex_dump;
            cfg.log = on_gs_log;
            cfg.texture_cache_mb = g_cfg.texture_cache_mb > 0 ? (uint32_t)g_cfg.texture_cache_mb : 0;
            cfg.texture_no_preload = g_cfg.texture_no_preload ? 1 : 0;
        }
    }
    int r = gs.init(&cfg);
    if (r == -4 && cfg.super_sampling > 1) {
        /* paraLLEl-GS could not be made with these samples (memory, most likely): the same renderer at one sample,
         * and the log and the session summary say so (settings_enhancements) */
        host_log("graphics: %ux anti-aliasing%s could not be made (paraLLEl-GS init failed, %d): trying one sample",
                 cfg.super_sampling, cfg.high_resolution_scanout ? " with the double-resolution picture" : "", r);
        g_cfg.ssaa_asked = (int)cfg.super_sampling;
        g_cfg.hires_asked = g_cfg.hires;
        g_cfg.ssaa = 0;
        g_cfg.hires = 0;
        cfg.super_sampling = 1;
        cfg.high_resolution_scanout = 0;
        r = gs.init(&cfg);
        if (r == 0)
            host_log("GS renderer: one sample, as the requested %ux super-sampling is not available", g_cfg.ssaa_asked);
    }
    if (r != 0) {
        static const char *why[] = {"", "library version mismatch", "no Vulkan loader",
                                    "no Vulkan device could be created",
                                    "the Vulkan device lacks features paraLLEl-GS needs",
                                    "only a software Vulkan device (e.g. llvmpipe), which paraLLEl-GS does not support"};
        host_log("GS renderer unavailable: %s (%d); running without graphics", r >= -5 && r < 0 ? why[-r] : "error", r);
        if (gs.rate > 30)
            host_log("%d frames per second: ignored (there is no renderer to make them)", gs.rate);
        return;
    }
    gs.ok = 1;
    if (gs.rate >= 60) {
        mh2gs_interp_status st;
        memset(&st, 0, sizeof(st));
        if (gs.interp_status)
            gs.interp_status(&st);
        if (st.state == 1 && gs.set_alphas && gs.mids && gs.interp_stop && gs.frame_poll) {
            gs.hf_on = 1;
            present_hfr(gs.rate);
            if (gs.set_notify)
                gs.set_notify(gs_notify, NULL);
        }
    }
    host_log("paraLLEl-GS renderer ready");
    if (cfg.super_sampling > 1)
        host_log("%ux super-sampling%s", cfg.super_sampling, cfg.high_resolution_scanout ? ", double-resolution picture" : "");
}

/* A picture of the renderer's frame: the colour opaque (the GS's alpha is not alpha), RGB24, SDL_SavePNG (the path is UTF-8).
 * Written at once on this thread: the core may wait, but sees nothing of it. 1 when written. */
static int save_png(const mh2gs_frame *f, const char *path, uint64_t vblank)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)f->width, (int)f->height, SDL_PIXELFORMAT_RGBA32, (void *)f->pixels,
                                           (int)f->stride);
    SDL_Surface *o = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24) : NULL;
    int ok = o && SDL_SavePNG(o, path);
    if (ok)
        host_log("screenshot %s (%ux%u, VBlank %llu)", path, f->width, f->height, (unsigned long long)vblank);
    else
        host_log("screenshot %s: %s", path, SDL_GetError());
    SDL_DestroySurface(o);
    SDL_DestroySurface(s);
    return ok;
}

/* A movie picture kept for the next VSync's presentation (the original's keep_movie_frame). */
static int keep_movie(const uint8_t *pixels, uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h * 4;
    if (n > gs.movie_cap) {
        uint8_t *q = SDL_realloc(gs.movie, n);
        if (!q)
            return 0;
        gs.movie = q;
        gs.movie_cap = n;
    }
    memcpy(gs.movie, pixels, n);
    gs.movie_w = w;
    gs.movie_h = h;
    return 1;
}

/* The in-between picture's state, told in the log when it changes (mh2gs.h, mh2gs_interp_status). */
static void interp_note(void)
{
    if (!gs.interp_status || gs.rate <= 30)
        return;
    mh2gs_interp_status st;
    gs.interp_status(&st);
    if (st.state == gs.interp_state)
        return;
    gs.interp_state = st.state;
    static const char *const why[] = {
        "off", "on",
        NULL,   /* 2: unused (the texture packs work with the in-between frames since design B; the library never says it) */
        "the graphics card could not keep up; back to 30 frames per second",
        "the renderer for the in-between frames could not be made; 30 frames per second"};
    host_log("%d frames per second: %s", gs.rate, st.state < 5 && why[st.state] ? why[st.state] : "?");
    if (st.state != 1 && gs.hf_on) {   /* the library gave the mode up (or never had it) */
        gs.hf_on = 0;
        gs.rate = 30;
        present_hfr(0);
    }
}

/* ---- 120 and 144 frames per second: the schedule ---- */

/* The step down when too many pictures are late: 144, 120, 60, 30 (hfr_step_down). */
static void hf_fall_back(void)
{
    int nr = hfr_step_down(gs.rate, present_refresh_hz());
    host_log("the graphics card could not keep up with %d frames per second: %d frames per second", gs.rate, nr);
    gs.rate = nr;
    gs.late_hist = 0;
    gs.late_n = 0;
    gs.npend = 0;
    gs.hf_lag = HFR_VBLANK_NS;
    if (nr >= 60) {
        present_hfr(nr);
        return;
    }
    gs.hf_on = 0;
    present_hfr(0);
    gs.set_alphas(NULL, 0);
    gs.interp_stop();   /* 30: no in-between pictures */
}

/* Before a VSync: the clock, and the alphas of the refreshes of the frame whose real picture this VSync ends (ANCHOR: it flips). */
static void hf_before_vsync(uint64_t vblank, int anchor, double arrival)
{
    double t = hfr_clock(&gs.clk, arrival, vblank);
    gs.hf_n = 0;
    if (anchor) {
        if (gs.hf_lag < HFR_VBLANK_NS)
            gs.hf_lag = HFR_VBLANK_NS;
        double b = t + gs.hf_lag;   /* the real picture is shown a VBlank after its VSync, as at 60 (more while the pictures are late: hf_poll) */
        double ref, rd;
        int dv;
        if (!present_grid(&ref, &rd, &dv)) {   /* no refresh seen yet: a grid of the rate itself, on the real picture */
            ref = b;
            rd = 1e9 / (double)gs.rate;
            dv = 1;
        }
        gs.hf_b = b;
        if (gs.rate <= 60)   /* 60: the one picture between each two, at half a step, on the game's clock (the display's grid takes it at the nearest refresh) */
            gs.hf_n = hfr_plan_half(b, gs.hf_alpha, gs.hf_tau);
        else
            gs.hf_n = hfr_plan(b, ref, rd, dv, gs.hf_alpha, gs.hf_tau, HFR_MAX);
    }
    double al[HFR_MAX];
    for (int i = 0; i < gs.hf_n; i++)
        al[i] = (double)gs.hf_alpha[i] / 256.0;
    gs.set_alphas(al, (uint32_t)gs.hf_n);
}

static void hf_add(uint64_t id, uint64_t vblank, double tau, int real, unsigned alpha)
{
    if (!id || gs.npend >= (int)(sizeof(gs.pend) / sizeof(gs.pend[0])))
        return;
    gs.pend[gs.npend++] = (struct HfPend){id, vblank, tau, real, alpha, 0};
}

/* After a VSync: the pictures it made wait for their copies. */
static void hf_after_vsync(uint64_t id, uint64_t vblank, int real)
{
    uint64_t ids[HFR_MAX];
    uint32_t n = gs.mids(ids, HFR_MAX);   /* in the order of the alphas given before this VSync (0: not made) */
    if (real && !gs.note_real)
        hf_add(id, vblank, gs.hf_b, 1, 0);
    for (uint32_t i = gs.note_mids; i < n && i < (uint32_t)gs.hf_n; i++)
        hf_add(ids[i], vblank, gs.hf_tau[i], 0, gs.hf_alpha[i]);
}

static void hf_poll(void);

/* The library tells of a picture while mh2gs_vsync is still making the step's others (mh2gs_set_notify): it is entered now, and the copies
 * started before are looked at, so that a finished one reaches the window at once and not when the call returns. */
static void gs_notify(void *user, uint64_t id, int index)
{
    (void)user;
    if (id && index == -1) {
        if (gs.cur_real) {
            hf_add(id, gs.cur_vblank, gs.hf_b, 1, 0);
            gs.note_real = 1;
        }
    } else if (id && index >= 0 && index < gs.hf_n && (uint32_t)index == gs.note_mids) {
        hf_add(id, gs.cur_vblank, gs.hf_tau[index], 0, gs.hf_alpha[index]);
        gs.note_mids = (uint32_t)index + 1;
    } else if (id && index >= 0) {
        return;
    }
    hf_poll();
}


/* The copies that are finished go to the window with their times; one not finished 3 ms before its time is late. Sixteen of the last 32 late: a step down. */
static void hf_poll(void)
{
    if (!gs.npend)
        return;
    const double now = (double)SDL_GetTicksNS();
    /* Late is judged by the work, not by when it was looked at: the GS thread looks at the copies when it has a moment (not while it makes a frame's
     * pictures, which can take 20 ms), and a copy that is found finished long after it was due may have been finished long before. A picture is late
     * when a look 3 ms or less before its time found it not finished, or when it was looked at closely (the last look 2 ms ago or less) and had
     * only just finished; a copy first seen finished after a long blind stretch is not counted. Nothing here waits for the window, so a window that
     * is hidden or not in front makes no difference. */
    const double since_look = now - gs.last_look;
    gs.last_look = now;
    int keep = 0;
    for (int i = 0; i < gs.npend; i++) {
        struct HfPend pd = gs.pend[i];
        mh2gs_frame f;
        memset(&f, 0, sizeof(f));
        int r = gs.frame_poll(pd.id, &f);
        if (r == 0 && now < pd.tau + 50e6) {
            if (now > pd.tau - 3e6)
                pd.unfinished_late = 1;
            gs.pend[keep++] = pd;
            continue;
        }
        int late = r != 1 || pd.unfinished_late || (now > pd.tau - 3e6 && since_look <= 2e6);
        if (r == 1) {
            present_queue(f.pixels, f.width, f.height, f.stride, (uint64_t)pd.tau, pd.real);
        }
        if (gs.clk.steady < 32) {   /* not at real time (a pause, fast-forward, a hitch): late means nothing */
            gs.late_hist = 0;
            gs.late_n = 0;
        } else {
            gs.late_hist = (gs.late_hist << 1) | (late ? 1u : 0u);
            if (gs.late_n < 32)
                gs.late_n++;
        }
    }
    gs.npend = keep;
    if (gs.in_call)   /* the lag and the step down are decided between two calls, not inside one */
        return;
    if (gs.late_n >= 32) {
        /* every 32 pictures: 4 or more late, the pictures are shown 2 ms later (up to 2 VBlanks); none late, 1 ms sooner (down to 1 VBlank); half of them late at the
         * most: the card cannot keep up, not a hitch */
        int nl = __builtin_popcount(gs.late_hist);
        gs.late_n = 0;
        if (nl >= 16 && gs.hf_lag >= 2.0 * HFR_VBLANK_NS - 1.0)
            hf_fall_back();
        else if (nl >= 4 && gs.hf_lag < 2.0 * HFR_VBLANK_NS)
            gs.hf_lag += 2e6;
        else if (nl == 0 && gs.hf_lag > HFR_VBLANK_NS)
            gs.hf_lag -= 1e6;
    }
}


static void on_vsync(const Mh2IpcRec *rec, const unsigned char *p)
{
    uint64_t vblank;
    memcpy(&vblank, p, 8);
    const double arrival = gs.hf_on ? (double)SDL_GetTicksNS() : 0;
    const int flip_now = gs.flip;
    SDL_AddAtomicU32(&gs.vsyncs, 1);
    SDL_SetAtomicU32(&gs.last_vblank, (Uint32)vblank);
    /* a movie playing (movie.c) replaces the GS's picture */
    const uint8_t *mpix = NULL;
    uint32_t mw = 0, mh = 0;
    int movie = movie_vsync(rec->arg1, gs.flip, &mpix, &mw, &mh);
    gs.flip = 0;
    if (!gs.ok)
        return;
    interp_note();
    int show = !g_cfg.no_window;
    const int hfr = gs.hf_on;   /* the timed presenter shows the pictures: only the pictures of flips are copied back here, and the in-between ones with them */
    const int want = hfr ? show && flip_now && !movie : show;
    if (hfr)
        hf_before_vsync(vblank, flip_now && !movie, arrival);
    gs.cur_vblank = vblank;
    gs.cur_real = hfr && show && flip_now && !movie;
    gs.note_real = 0;
    gs.note_mids = 0;
    gs.in_call = hfr;
    int photo = SDL_GetAtomicInt(&gs.photo_req) && vblank >= gs.photo_after;
    uint64_t id = gs.vsync(rec->arg0, !movie && (want || photo));
    gs.in_call = 0;
    if (hfr) {
        hf_after_vsync(id, vblank, show && flip_now && !movie);
        hf_poll();
    }
    if (show && gs.prev_movie) {
        present_submit(gs.movie, gs.movie_w, gs.movie_h, gs.movie_w * 4, gs.prev_vblank, 1);
        mh2ipc_store(&g_ipc->presented_lo, (unsigned int)gs.prev_vblank);
        mh2ipc_store(&g_ipc->presented_hi, (unsigned int)(gs.prev_vblank >> 32));
    } else if (show && gs.prev_id && !hfr) {
        mh2gs_frame f;
        memset(&f, 0, sizeof(f));
        if (gs.get_frame(gs.prev_id, &f)) {
            present_submit(f.pixels, f.width, f.height, f.stride, gs.prev_vblank, 0);
            mh2ipc_store(&g_ipc->presented_lo, (unsigned int)gs.prev_vblank);
            mh2ipc_store(&g_ipc->presented_hi, (unsigned int)(gs.prev_vblank >> 32));
        }
    }
    gs.prev_id = id;
    gs.prev_vblank = vblank;
    gs.prev_movie = show && movie && keep_movie(mpix, mw, mh);
    if (!photo)
        return;
    mh2gs_frame f;
    memset(&f, 0, sizeof(f));
    if (movie) {
        f.pixels = mpix;
        f.width = mw;
        f.height = mh;
        f.stride = mw * 4;
    } else if (!gs.get_frame(id, &f)) {
        return;   /* no picture (the display off): the next VBlank's */
    }
    if (photo) {
        SDL_SetAtomicU32(&gs.photo_w, f.width);
        SDL_SetAtomicU32(&gs.photo_h, f.height);
        int ok = save_png(&f, gs.photo_path, vblank);
        SDL_SetAtomicInt(&gs.photo_req, 0);
        SDL_SetAtomicInt(&gs.photo_done, ok ? 1 : -1);
    }
}

/* A photo (photo mode): the first picture at a VBlank of AFTER or later, as the renderer's readback gives it (its size), written to
 * PATH (UTF-8) on the GS thread. 0 when there is no renderer or one is still pending. */
int gs_request_photo(const char *path, uint64_t after)
{
    if (!gs.ok || SDL_GetAtomicInt(&gs.photo_req))
        return 0;
    snprintf(gs.photo_path, sizeof(gs.photo_path), "%s", path);
    gs.photo_after = after;
    SDL_SetAtomicInt(&gs.photo_done, 0);
    SDL_SetAtomicInt(&gs.photo_req, 1);   /* a full barrier: the path is there first */
    return 1;
}

/* The photo's end: 1 written (W and H its size), -1 failed, 0 none to report. Taken once. */
int gs_photo_result(unsigned *w, unsigned *h)
{
    int d = SDL_GetAtomicInt(&gs.photo_done);
    if (!d)
        return 0;
    *w = SDL_GetAtomicU32(&gs.photo_w);
    *h = SDL_GetAtomicU32(&gs.photo_h);
    SDL_SetAtomicInt(&gs.photo_done, 0);
    return d;
}


static void on_readback(const Mh2IpcRec *rec)
{
    unsigned char *dst = g_block + g_ipc->rb_offset;
    uint32_t qwords = rec->arg0 * 16 <= g_ipc->rb_size ? rec->arg0 : g_ipc->rb_size / 16;
    if (gs.ok)
        gs.read_fifo(dst, qwords);
    else
        memset(dst, 0, (size_t)qwords * 16);
    mh2ipc_store(&g_ipc->rb_done_seq, rec->arg1);
    mh2ipc_fence();
    if (__atomic_load_n(&g_ipc->rb_core_waiting, __ATOMIC_RELAXED))
        mh2ipc_wake(&g_map, MH2IPC_EV_RB_DONE, &g_ipc->rb_done_seq);
}

/* A save state's records (Mh2IpcState): the renderer's block into the state window (STATE_GET) or out of it (STATE_SET),
 * at this place in the ring, and the answer the core waits for, as for a readback. */
static void on_state(const Mh2IpcRec *rec)
{
    Mh2IpcState *st = &g_ipc->state;
    unsigned char *win = g_block + st->window_offset;
    int64_t r = -1;   /* MH2GS_STATE_NO_RENDERER */
    if (rec->type == MH2IPC_REC_STATE_GET) {
        st->gs_bytes = 0;
        if (gs.ok) {
            r = gs.state_get(win, st->window_bytes);
            if (r > 0) {
                st->gs_bytes = (uint32_t)r;
                r = 0;
            }
        }
    } else if (gs.ok && rec->arg0 <= st->window_bytes) {
        r = gs.state_set(win, rec->arg0);
    }
    if (r)
        host_log("save state: the renderer's part: error %d (%s)", (int)r, rec->type == MH2IPC_REC_STATE_GET ? "save" : "load");
    st->gs_result = (uint32_t)(int32_t)r;
    mh2ipc_store(&g_ipc->rb_done_seq, rec->arg1);
    mh2ipc_fence();
    if (__atomic_load_n(&g_ipc->rb_core_waiting, __ATOMIC_RELAXED))
        mh2ipc_wake(&g_map, MH2IPC_EV_RB_DONE, &g_ipc->rb_done_seq);
}

static void on_record(const Mh2IpcRec *rec)
{
    const unsigned char *p = (const unsigned char *)(rec + 1);
    switch (rec->type) {
    case MH2IPC_REC_GIF:
        if (gs.ok && rec->arg0 >= 1 && rec->arg0 <= 3)
            gs.gif(rec->arg0, p, rec->bytes & ~15u);
        break;
    case MH2IPC_REC_PRIV:
        gs.flip = 1;
        if (gs.ok) {
            uint64_t v;
            memcpy(&v, p, 8);
            gs.priv_write(rec->arg0, v);
        }
        break;
    case MH2IPC_REC_VSYNC:
        on_vsync(rec, p);
        break;
    case MH2IPC_REC_CAMERA:
        if (gs.ok && gs.camera && rec->bytes >= 32 * sizeof(float))
            gs.camera((const float *)p);
        break;
    case MH2IPC_REC_READBACK:
        on_readback(rec);
        break;
    case MH2IPC_REC_STATE_GET:
    case MH2IPC_REC_STATE_SET:
        on_state(rec);
        break;
    case MH2IPC_REC_MOVIE_OPEN:
        if (rec->bytes > sizeof(Mh2IpcMovieOpen)) {
            Mh2IpcMovieOpen m;
            char path[1024];
            size_t n = rec->bytes - sizeof(m) < sizeof(path) ? rec->bytes - sizeof(m) : sizeof(path);
            memcpy(&m, p, sizeof(m));
            memcpy(path, p + sizeof(m), n);
            path[n - 1] = 0;
            movie_open(&m, path);
        }
        break;
    case MH2IPC_REC_MOVIE_CLOSE:
        movie_close(rec->arg0);
        break;
    default:
        host_log("GS ring: a record of unknown type %u", rec->type);
        break;
    }
}

static int SDLCALL gs_main(void *arg)
{
    (void)arg;
    host_realtime_thread();
    gs_make_renderer();
    SDL_SignalSemaphore(gs.ready);
    Mh2IpcRing *r = &g_ipc->gs;
    for (;;) {
        const Mh2IpcRec *rec = mh2ipc_rec_peek(r, g_block);
        if (!rec) {
            unsigned int head = mh2ipc_load(&r->head);
            unsigned int st = mh2ipc_load(&g_ipc->core_state);
            if (SDL_GetAtomicInt(&gs.stop) || (st != MH2IPC_STATE_NONE && st != MH2IPC_STATE_RUNNING))
                if (head == r->tail)
                    break;
            mh2ipc_store(&r->consumer_waiting, 1);
            mh2ipc_fence();
            if (gs.hf_on)
                hf_poll();   /* the copies of the pictures that are not finished: looked at every millisecond */
            if (!mh2ipc_rec_peek(r, g_block))
                mh2ipc_wait(&g_map, MH2IPC_EV_GS_DATA, &r->head, head, gs.hf_on && gs.npend ? 1 : 20);
            mh2ipc_store(&r->consumer_waiting, 0);
            continue;
        }
        on_record(rec);
        if (mh2ipc_rec_release(r, rec))
            mh2ipc_wake(&g_map, MH2IPC_EV_GS_SPACE, &r->tail);
    }
    /* the movie stays (main.c frees it after the sound device): the device still plays the core's
     * last frames, with the movie's sound mixed in as the original's output has it */
    if (gs.ok)
        gs.shutdown();
    gs.ok = 0;
    SDL_free(gs.movie);
    gs.movie = NULL;
    return 0;
}

int gs_init(void)
{
    gs_load();
    gs.ready = SDL_CreateSemaphore(0);
    gs.thread = gs.ready ? SDL_CreateThread(gs_main, "gs", NULL) : NULL;
    if (!gs.thread) {
        host_log("the GS thread: %s", SDL_GetError());
        return -2;
    }
    /* The renderer may read the texture packs before it is ready (mh2tex.cpp, the preload: seconds with a big pack):
     * the window answers the system meanwhile */
    while (!SDL_WaitSemaphoreTimeout(gs.ready, 50))
        SDL_PumpEvents();
    gs_state_identity(g_ipc->state.identity, sizeof(g_ipc->state.identity));   /* for the save states: before the core starts */
    return gs.ok ? 0 : -1;
}

void gs_stop(void)
{
    SDL_SetAtomicInt(&gs.stop, 1);
    mh2ipc_wake(&g_map, MH2IPC_EV_GS_DATA, &g_ipc->gs.head);
    if (gs.thread)
        SDL_WaitThread(gs.thread, NULL);
    gs.thread = NULL;
    if (gs.ready)
        SDL_DestroySemaphore(gs.ready);
    gs.ready = NULL;
}

uint64_t gs_vsyncs(void)
{
    return SDL_GetAtomicU32(&gs.vsyncs);
}

uint64_t gs_last_vblank(void)
{
    return SDL_GetAtomicU32(&gs.last_vblank);
}

/* FNV-1a of the renderer's library file: its build. */
static uint32_t file_hash(const char *path)
{
    uint32_t h = 2166136261u;
    SDL_IOStream *f = SDL_IOFromFile(path, "rb");
    unsigned char buf[65536];
    size_t n;
    if (!f)
        return 0;
    while ((n = SDL_ReadIO(f, buf, sizeof(buf))) > 0)
        for (size_t i = 0; i < n; i++)
            h = (h ^ buf[i]) * 16777619u;
    SDL_CloseIO(f);
    return h;
}

/* The identity a save state keeps (Mh2IpcState.identity): the renderer's build and the settings that change the state
 * its block holds. Once the renderer is made (the samples may have fallen back), before the core starts. */
void gs_state_identity(char *out, size_t cap)
{
    out[0] = 0;
    if (!gs.ok)
        return;
    const char *base = SDL_GetBasePath();
    char path[1200];
    snprintf(path, sizeof(path), "%s%s", base ? base : "", MH2GS_LIB);
    snprintf(out, cap, "gs%d-%08x-s%dh%dp%d", MH2GS_ABI_VERSION, file_hash(path), g_cfg.ssaa >= 2 ? g_cfg.ssaa : 1,
             g_cfg.hires ? 1 : 0, g_cfg.texture_packs ? 1 : 0);
}
