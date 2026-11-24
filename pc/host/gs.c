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
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hfr.h"
/* A development build's switch (the environment); a release build reads none and names none */
#define GS_ENV(name) ((const char *)0)
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
    mh2gs_model_fn model;
    mh2gs_pass_fn pass;
    mh2gs_pass2_fn pass2;
    mh2gs_pass_wait_fn pass_wait;
    mh2gs_pass_moment_wait_fn pass_mom_wait;   /* the wait by picture (the pass with its ring; MH2_PASS_PICWAIT=1 in a development build) */
    mh2gs_pass_steady_fn pass_steady;
    double pw_lead_ns, pw_cap_ns;              /* a picture's moment is waited for until its time less a margin (MH2_PASS_PICLEAD, ms, 4), and never past this long after the VSync record (MH2_PASS_PICCAP, ms, 31) */
    uint64_t pw_waits, pw_late, pw_ready;      /* pictures whose moment was waited for; of those, the deadline came first; the moment was there (or none was to come) */
    SDL_Thread *pass_thread;   /* the reader of the pass thread's ring (the pass with its ring; MH2_PASS_THREAD=1 in a development build) */
    int pass_sync;             /* MH2_PASS_SYNC=1 (tests): the VSync waits for the pass thread without a limit */
    unsigned pass_wait_ms;     /* the limit, MH2_PASS_WAITMS (default 15) */
    int pass_waitfirst;        /* MH2_PASS_WAITFIRST=1 (tests): the wait comes before the real picture, as it did before the real picture went first */
    int real_due, real_started;   /* this VSync: a real picture is to be shown / has been started */
    double vs_arrival;         /* the time the VSync record was taken up (ns) */
    /* MH2_PASS (development): the core stamps each VSync record with the time it reached the VSync (MH2IPC_REC_STAMP), and the clock uses that time, not the time the reader took the record up */
    double stamp_t;            /* the stamp of the next VSync record, in SDL_GetTicksNS's time */
    unsigned stamp_state;      /* the core's pacing timeline at that VBlank (the stamp's second word): 0 none, 1 on it, 2 began or broke */
    unsigned stamp_plan, vs_plan;   /* the live plan's VBlank number the core's stamp names (0: none) and, for the VSync being made, the same */
    int live;                  /* MH2_PASS_LIVE=1 (development): the host publishes each step's plan at its anchor and the core's moments are made at it (plan 11's step 5a) */
    HfrPlan pred[2];           /* the plans published at the last two anchors (each for the step two anchors on, so that the core has it before its first window), by slot (VBlank number / 2) & 1 */
    uint32_t pred_vb[2];       /* the VBlank number each plan's step's VSync is expected to have (0: none) */
    uint64_t lv_steps, lv_nopred, lv_unused, lv_stale, lv_used, lv_pics_c, lv_pics_b;   /* anchors; with no plan published for them; whose moments the core did not make at the plan; whose b had moved; steps that kept their moments; pictures from a moment / B's by the match */
    int stamp_have, nostamp, notimeline;   /* a stamp is waiting; MH2_PASS_NOSTAMP=1 (the control): the stamps are not used; MH2_PASS_NOTIMELINE=1 (the control): the stamps' timeline state is ignored, a jump is judged by the old rule */
    uint64_t st_stamp_n, st_stamp_over1, st_stamp_over8;   /* VSync records that had a stamp; read over 1 ms and over 8 ms after it */
    double st_stamp_sum, st_stamp_max, stamp_off;
    int stamp_off_have;
    uint64_t st_rs_n, st_rs_over4, st_pw_slept, st_pw_real;   /* real pictures started; started over 4 ms after the record; waits that slept; of them with the real picture still to start */
    double st_rs_sum, st_rs_max, st_pw_ms;
    uint64_t st_pw_first, st_pw_last;   /* the VSync records (counted from 1) of the first and the last wait that slept with the real picture still to start */
    mh2gs_anchor_fn anchor;
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
    gs.model = (mh2gs_model_fn)SDL_LoadFunction(gs.lib, "mh2gs_model");   /* optional */
    gs.pass = (mh2gs_pass_fn)SDL_LoadFunction(gs.lib, "mh2gs_pass");   /* optional */
    gs.pass2 = (mh2gs_pass2_fn)SDL_LoadFunction(gs.lib, "mh2gs_pass2");   /* optional */
    gs.pass_wait = (mh2gs_pass_wait_fn)SDL_LoadFunction(gs.lib, "mh2gs_pass_wait");   /* optional */
    gs.pass_mom_wait = (mh2gs_pass_moment_wait_fn)SDL_LoadFunction(gs.lib, "mh2gs_pass_moment_wait");   /* optional, development builds of the library */
    gs.pass_steady = (mh2gs_pass_steady_fn)SDL_LoadFunction(gs.lib, "mh2gs_pass_steady");   /* optional */
    gs.anchor = (mh2gs_anchor_fn)SDL_LoadFunction(gs.lib, "mh2gs_anchor");   /* optional */
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
    /* 120 and 144 need HFR_MIN_CPUS logical processors (hfr_rate_rule): below that they are 60, and the log says so */
    int cpus = SDL_GetNumLogicalCPUCores();
    gs.rate = hfr_rate_rule(g_cfg.fps, cpus);
    if (g_cfg.fps > 60 && gs.rate == 60)
        host_log("%d frames per second needs %d logical processors (this computer has %d): 60 frames per second", g_cfg.fps, HFR_MIN_CPUS, cpus);
    cfg.interpolate = gs.rate >= 60 ? MH2GS_INTERP_ON : MH2GS_INTERP_OFF;
    /* the in-between pictures from the core's second pass, whose ring the host made for the rate (main.c): a release build's form, and a
     * development build's with MH2_PASS_CFG=1; otherwise a development build's library reads its switches */
    const int pass_cfg = 1;
    cfg.pass = pass_cfg && g_ipc->pass.size && gs.rate >= 60 ? MH2GS_PASS_ON : MH2GS_PASS_OFF;
    /* the renderer's local memory: the GS's 4 MB for the base core; 8 MB for the content core, whose texture area reaches past
     * block 16383 (a state is good only for the memory it was made with, so the two cores' states are not exchanged) */
    cfg.vram_mb = g_cfg.content_core ? 8 : 4;
    if (g_cfg.content_core)
        host_log("graphics memory: 8 MB (the content core)");
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

/* The live alphas (plan 11's step 5a), at an anchor whose real picture is shown at b: first the plan published at the last anchor against this one (the core made its moments at it when its stamp names this VBlank; each
 * refresh that has a moment within a quarter of a refresh of its time takes the moment's alpha, the others are B's; a step whose b has moved by over a millisecond is B's whole), then the plan of the next step, published
 * with the VBlank number its VSync is expected to have. At 60 the one picture is at 128 whenever the moment is there. */
static void hf_live(uint64_t vblank, double b)
{
    double ref, rd;
    int dv;
    if (!present_grid(&ref, &rd, &dv)) {
        ref = b;
        rd = 1e9 / (double)gs.rate;
        dv = 1;
    }
    const uint32_t vb = (uint32_t)vblank;
    const HfrPlan *pr = &gs.pred[(vb >> 1) & 1u];
    gs.lv_steps++;
    if (!gs.pred_vb[(vb >> 1) & 1u] || gs.pred_vb[(vb >> 1) & 1u] != vb)
        gs.lv_nopred++;
    else if (gs.vs_plan != vb)
        gs.lv_unused++;
    else {
        HfrPlan act;
        HfrMatch mt;
        act.b = b;
        act.n = gs.hf_n;
        for (int i = 0; i < gs.hf_n; i++) {
            act.alpha[i] = gs.hf_alpha[i];
            act.tau[i] = gs.hf_tau[i];
        }
        hfr_match(pr, &act, rd * dv, gs.rate <= 60 ? 1e9 : HFR_LIVE_STEP_NS, &mt);
        if (mt.stale)
            gs.lv_stale++;
        else
            gs.lv_used++;
        for (int i = 0; i < gs.hf_n; i++)
            if (mt.use[i] >= 0)
                gs.hf_alpha[i] = pr->alpha[mt.use[i]];
        /* The step whose b kept within the tolerance has the plan's pictures, at the plan's alphas, their times moved by b's shift: the actual refreshes' own list differs from the plan's by the return
         * times of the presents that fix the grid (they jitter by up to half a refresh) and by a refresh at the edge of the step, so a match by time left whole steps and single pictures to B though the
         * core had made every moment. MH2_LIVE_MATCH=1 gives the match by time (the control). */
        static int old_match = -1;
        if (old_match < 0)
            old_match = GS_ENV("MH2_LIVE_MATCH") != NULL && GS_ENV("MH2_LIVE_MATCH")[0] == '1';
        if (!old_match && !mt.stale && pr->n > 0) {
            gs.hf_n = pr->n < HFR_MAX ? pr->n : HFR_MAX;
            for (int i = 0; i < gs.hf_n; i++) {
                gs.hf_alpha[i] = pr->alpha[i];
                gs.hf_tau[i] = pr->tau[i] + (b - pr->b);
            }
            mt.matched = gs.hf_n;
            mt.bs = 0;
        }
        gs.lv_pics_c += (uint64_t)mt.matched;
        {   /* MH2_LIVE_LOG=FILE: a line for each anchor with a plan used: its VBlank, the plan's alphas, the actual refreshes' alphas before the match and after it, and the match's counts */
            static FILE *lf;
            static int lf_init;
            if (!lf_init) {
                lf_init = 1;
                const char *e = GS_ENV("MH2_LIVE_LOG");
                if (e)
                    lf = hfs_fopen(e, "w");
            }
            if (lf) {
                fprintf(lf, "anchor %u: stale %d b %.3f plan n %d:", vb, mt.stale, b / 1e6, pr->n);
                for (int i = 0; i < pr->n; i++)
                    fprintf(lf, " %u", (unsigned)pr->alpha[i]);
                fprintf(lf, "; actual n %d:", gs.hf_n);
                for (int i = 0; i < gs.hf_n; i++)
                    fprintf(lf, " %u", (unsigned)act.alpha[i]);
                fprintf(lf, " -> ");
                for (int i = 0; i < gs.hf_n; i++)
                    fprintf(lf, " %u", (unsigned)gs.hf_alpha[i]);
                fprintf(lf, "; matched %d bs %d\n", mt.matched, mt.bs);
                fflush(lf);
            }
        }
        gs.lv_pics_b += (uint64_t)mt.bs;
    }
    /* the plan of the step after the next: the core reads a step's plan at the step's first window, which it reaches before this anchor's record is read, so the plan is written two steps ahead (the real
     * picture of the next step is expected a step after b, and its in-between pictures follow it); the slot its number names keeps it until the step that uses it */
    const uint32_t nvb = vb + 4u, slot = (nvb >> 1) & 1u;
    hfr_predict(b + 2.0 * HFR_VBLANK_NS, ref, rd, dv, gs.rate <= 60, &gs.pred[slot]);
    gs.pred_vb[slot] = nvb;
    uint32_t al[MH2IPC_LIVE_MAX];
    for (int i = 0; i < gs.pred[slot].n && i < (int)MH2IPC_LIVE_MAX; i++)
        al[i] = gs.pred[slot].alpha[i];
    /* MH2_PASS_BNOWIN=N: no windows in B's phase. The plan's step is two steps after this anchor's, so it falls in B's phase when the phase has three steps or more left before this anchor's step is
     * made (the library counts a step when it has made it); such a step's plan goes to the core empty, with no fallback, and the core makes no moments for it (it still keeps the step's draws, so
     * the first step back has its partners). N - 1 more steps at the phase's end keep their plans. The host's own copy of the plan is untouched: B's pictures take the refreshes' alphas. */
    uint32_t pn = (uint32_t)gs.pred[slot].n, pfb = gs.rate <= 60 ? 1u : 0u;
    mh2ipc_liveplan_write(&g_ipc->live[slot], nvb, pn, al, pfb);
}

/* Before a VSync: the clock, and the alphas of the refreshes of the frame whose real picture this VSync ends (ANCHOR: it flips). */
static void hf_before_vsync(uint64_t vblank, int anchor, double arrival, unsigned state)
{
    double t = hfr_clock_timeline(&gs.clk, arrival, vblank, state, 0);
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
        if (gs.live)
            hf_live(vblank, b);
    }
    double al[HFR_MAX];
    for (int i = 0; i < gs.hf_n; i++)
        al[i] = (double)gs.hf_alpha[i] / 256.0;
    gs.set_alphas(al, (uint32_t)gs.hf_n);
    if (gs.pass_steady)
        gs.pass_steady(gs.clk.steady >= 32);
}

static void hf_add(uint64_t id, uint64_t vblank, double tau, int real, unsigned alpha)
{
    if (!id || gs.npend >= (int)(sizeof(gs.pend) / sizeof(gs.pend[0]))) {
        return;
    }
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
static void pass_step_wait(void);
static void pass_pic_wait(int k);

/* The library tells of a picture while mh2gs_vsync is still making the step's others (mh2gs_set_notify): it is entered now, and the copies
 * started before are looked at, so that a finished one reaches the window at once and not when the call returns. */
static void gs_notify(void *user, uint64_t id, int index)
{
    (void)user;
    if (index <= -16) {   /* the library is about to make picture k: its moment is waited for (MH2_PASS_PICWAIT) */
        pass_pic_wait(-16 - index);
        return;
    }
    if (index == -4) {   /* the library has started the real picture: the pass thread's windows are waited for now */
        if (!gs.pass_waitfirst)
            pass_step_wait();
        return;
    }
    if (id && index == -1) {
        {
            const double d = ((double)SDL_GetTicksNS() - gs.vs_arrival) / 1e6;
            gs.real_started = 1;
            gs.st_rs_n++;
            gs.st_rs_sum += d;
            gs.st_rs_over4 += d > 4.0;
            if (d > gs.st_rs_max)
                gs.st_rs_max = d;
        }
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
    double arrival = gs.hf_on ? (double)SDL_GetTicksNS() : 0;
    unsigned state = 0;   /* the core's timeline at this VBlank, when a stamp tells it */
    if (gs.stamp_have && gs.hf_on && !gs.nostamp) {
        const double lag = (arrival - gs.stamp_t) / 1e6;
        gs.st_stamp_n++;
        gs.st_stamp_sum += lag;
        gs.st_stamp_over1 += lag > 1.0;
        gs.st_stamp_over8 += lag > 8.0;
        if (lag > gs.st_stamp_max)
            gs.st_stamp_max = lag;
        arrival = gs.stamp_t;
        state = gs.notimeline ? 0 : gs.stamp_state;
    }
    gs.stamp_have = 0;
    gs.vs_plan = gs.stamp_plan;
    gs.stamp_plan = 0;
    const int flip_now = gs.flip;
    SDL_AddAtomicU32(&gs.vsyncs, 1);
    SDL_SetAtomicU32(&gs.last_vblank, (Uint32)vblank);
    gs.vs_arrival = (double)SDL_GetTicksNS();
    gs.real_started = 0;
    /* a movie playing (movie.c) replaces the GS's picture */
    const uint8_t *mpix = NULL;
    uint32_t mw = 0, mh = 0;
    int movie = movie_vsync(rec->arg1, gs.flip, &mpix, &mw, &mh);
    gs.flip = 0;
    if (!gs.ok)
        return;
    gs.real_due = gs.hf_on && !g_cfg.no_window && flip_now && !movie;
    if (gs.pass_waitfirst)
        pass_step_wait();
    interp_note();
    int show = !g_cfg.no_window;
    const int hfr = gs.hf_on;   /* the timed presenter shows the pictures: only the pictures of flips are copied back here, and the in-between ones with them */
    const int want = hfr ? show && flip_now && !movie : show;
    if (hfr)
        hf_before_vsync(vblank, flip_now && !movie, arrival, state);
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
    } else {
        uint64_t hid = id;
        if (!gs.get_frame(hid, &f))
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
    case MH2IPC_REC_STAMP:
        if (rec->bytes >= 8) {   /* the core's clock in this clock's time: the two are read together, and the pair with the shortest gap between its reads is kept */
            uint64_t t;
            memcpy(&t, p, 8);
            const Uint64 a = SDL_GetTicksNS(), c = mh2ipc_time_ns(), b = SDL_GetTicksNS();
            if (!gs.stamp_off_have || b - a < 50000u) {
                gs.stamp_off = (double)(a / 2 + b / 2) - (double)c;
                gs.stamp_off_have = 1;
            }
            gs.stamp_t = (double)t + gs.stamp_off;
            gs.stamp_state = 0;
            if (rec->bytes >= 16) {
                uint64_t w;
                memcpy(&w, (const unsigned char *)p + 8, 8);
                gs.stamp_state = (unsigned)w;
            }
            gs.stamp_have = 1;
            gs.stamp_plan = rec->arg0;
        }
        break;
    case MH2IPC_REC_CAMERA:
        if (gs.ok && gs.camera && rec->bytes >= 32 * sizeof(float))
            gs.camera((const float *)p);
        break;
    case MH2IPC_REC_MODEL:
        if (gs.ok && gs.model) {
            if (rec->bytes >= sizeof(Mh2IpcModel)) {
                const Mh2IpcModel *m = (const Mh2IpcModel *)p;
                gs.model(rec->arg0, rec->arg1, m->prog, m->world, m->cam, m->site);
            } else {
                gs.model(0, 0, 0, NULL, NULL, 0);   /* the end of a labelled run */
            }
        }
        break;
    case MH2IPC_REC_ANCHOR:
        if (gs.ok && gs.anchor) {
            if (rec->bytes >= sizeof(Mh2IpcAnchor)) {
                const Mh2IpcAnchor *an = (const Mh2IpcAnchor *)p;
                gs.anchor(rec->arg0, an->point, an->cam);
            } else {
                gs.anchor(0, NULL, NULL);   /* the end of the packet */
            }
        }
        break;
    case MH2IPC_REC_PASS:
        if (gs.ok && gs.pass)
            gs.pass(rec->arg0, rec->arg1, p, rec->bytes & ~15u);
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

/* The reader of the pass thread's ring (the in-between pictures' pass): it hands the records to the renderer, which keeps them by the window's serial number. It never waits for the
 * main reader, nor the other way round: the VSync waits in pass_step_wait, and the pass thread's records are taken here meanwhile. */
static int SDLCALL gs_pass_main(void *arg)
{
    (void)arg;
    host_realtime_thread();
    Mh2IpcRing *r = &g_ipc->pass;
    unsigned long long rd_n = 0, rd_ns = 0, rd_max = 0, rd_over1 = 0, rd_over5 = 0, rd_sleeps = 0, rd_bytes = 0;   /* the reader's own count: each record's intake */
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
            if (!mh2ipc_rec_peek(r, g_block)) {
                rd_sleeps++;
                mh2ipc_wait(&g_map, MH2IPC_EV_PASS_DATA, &r->head, head, 20);
            }
            mh2ipc_store(&r->consumer_waiting, 0);
            continue;
        }
        if (rec->type == MH2IPC_REC_PASS && gs.ok && gs.pass2) {
            unsigned long long t0 = mh2ipc_time_ns(), d;
            gs.pass2(rec->arg0, rec->arg1, (const unsigned char *)(rec + 1), rec->bytes & ~15u);
            d = mh2ipc_time_ns() - t0;
            rd_n++;
            rd_ns += d;
            rd_bytes += rec->bytes;
            if (d > rd_max)
                rd_max = d;
            rd_over1 += d > 1000000ull;
            rd_over5 += d > 5000000ull;
        }
        if (mh2ipc_rec_release(r, rec))
            mh2ipc_wake(&g_map, MH2IPC_EV_PASS_SPACE, &r->tail);
    }
    if (rd_n)
        host_log("pass reader: %llu records taken (%.1f MB), their intake %.1f ms in all, %.3f ms a record, %.2f ms at worst, %llu over 1 ms, %llu over 5 ms; the reader slept %llu times on an empty ring",
                 rd_n, rd_bytes / 1048576.0, rd_ns / 1e6, rd_ns / 1e6 / (double)rd_n, rd_max / 1e6, rd_over1, rd_over5, rd_sleeps);
    return 0;
}

/* A wait that slept is counted: how long, and whether the step's real picture was still to start when it did (the old order: it was). */
static void pass_wait_note(Uint64 t0)
{
    gs.st_pw_slept++;
    gs.st_pw_ms += (double)(SDL_GetTicksNS() - t0) / 1e6;
    if (gs.real_due && !gs.real_started) {
        gs.st_pw_real++;
        if (!gs.st_pw_first)
            gs.st_pw_first = SDL_GetAtomicU32(&gs.vsyncs);
        gs.st_pw_last = SDL_GetAtomicU32(&gs.vsyncs);
    }
}

/* Before a VSync is given to the renderer: wait for the pass thread's data of the step, up to the limit (no limit with MH2_PASS_SYNC=1), as long as the core runs. */
static void pass_step_wait(void)
{
    if (!gs.pass_thread || !gs.pass_wait || !gs.ok)
        return;
    const Uint64 t0 = SDL_GetTicksNS();
    int slept = 0;
    for (;;) {
        const Uint64 used = (SDL_GetTicksNS() - t0) / 1000000ull;
        const unsigned left = gs.pass_sync || used >= gs.pass_wait_ms ? 5u : gs.pass_wait_ms - (unsigned)used;   /* the limit is the limit: the last slice is what is left of it */
        if (gs.pass_wait(left < 5u ? (left ? left : 1u) : 5u)) {
            if (slept)
                pass_wait_note(t0);
            return;
        }
        slept = 1;
        unsigned int st = mh2ipc_load(&g_ipc->core_state);
        if (SDL_GetAtomicInt(&gs.stop) || (st != MH2IPC_STATE_NONE && st != MH2IPC_STATE_RUNNING))
            return;
        if (!gs.pass_sync && (SDL_GetTicksNS() - t0) / 1000000ull >= gs.pass_wait_ms) {
            pass_wait_note(t0);
            return;
        }
    }
}

/* The wait by picture (plan 11's step 5a; the library's `mh2gs_pass_moment_wait`): picture k of the step, whose refresh is at hf_tau[k], waits for its moment until its time less a margin, and never past the cap
 * after the VSync record (the next step's chain can arrive then), looking at the core every half millisecond; it stops when the core waits for a state or a readback. The library takes the moment
 * if it is there and makes B's picture if not. MH2_PASS_SYNC=1 (tests): no deadline. */
static void pass_pic_wait(int k)
{
    if (!gs.pass_thread || !gs.pass_mom_wait || !gs.ok || k < 0 || k >= gs.hf_n)
        return;
    const uint32_t al = gs.hf_alpha[k];
    double dl = gs.hf_tau[k] - gs.pw_lead_ns;
    if (dl > gs.vs_arrival + gs.pw_cap_ns)
        dl = gs.vs_arrival + gs.pw_cap_ns;
    gs.pw_waits++;
    for (;;) {
        const double now = (double)SDL_GetTicksNS();
        if (!gs.pass_sync && now >= dl) {
            if (!gs.pass_mom_wait(al, 0))
                gs.pw_late++;
            else
                gs.pw_ready++;
            return;
        }
        double left_us = gs.pass_sync ? 500.0 : (dl - now) / 1000.0;
        if (left_us > 500.0)
            left_us = 500.0;
        if (gs.pass_mom_wait(al, (uint32_t)left_us)) {
            gs.pw_ready++;
            return;
        }
        const unsigned int st = mh2ipc_load(&g_ipc->core_state);
        if (SDL_GetAtomicInt(&gs.stop) || (st != MH2IPC_STATE_NONE && st != MH2IPC_STATE_RUNNING)) {
            gs.pw_late++;
            return;
        }
    }
}

static int SDLCALL gs_main(void *arg)
{
    (void)arg;
    host_realtime_thread();
    gs_make_renderer();
    gs.live = g_ipc->pass.size != 0;   /* the core makes its moments at the host's plans exactly when the ring exists */
    if (g_ipc->pass.size) {   /* the ring is read (and its records dropped when there is no renderer) whenever the block has one: the pass thread would wait on a full ring */
        const char *w = GS_ENV("MH2_PASS_WAITMS");
        gs.pass_wait_ms = w ? (unsigned)atoi(w) : 15u;
        w = GS_ENV("MH2_PASS_SYNC");
        gs.pass_sync = w && w[0] == '1';
        w = GS_ENV("MH2_PASS_WAITFIRST");
        gs.pass_waitfirst = w && w[0] == '1';
        w = GS_ENV("MH2_PASS_PICLEAD");
        gs.pw_lead_ns = (w ? atof(w) : 4.0) * 1e6;   /* 4 ms: at 144 it gave the first picture its moment more often than 6 and 8, and C its higher share */
        w = GS_ENV("MH2_PASS_PICCAP");
        gs.pw_cap_ns = (w ? atof(w) : 31.0) * 1e6;
        gs.pass_thread = SDL_CreateThread(gs_pass_main, "gs-pass", NULL);
        if (!gs.pass_thread)
            host_log("the pass thread's reader: %s", SDL_GetError());
    }
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
    if (gs.pass_thread) {   /* the reader of the pass thread's ring ends with the core, before the renderer goes */
        SDL_WaitThread(gs.pass_thread, NULL);
        gs.pass_thread = NULL;
    }
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
    mh2ipc_wake(&g_map, MH2IPC_EV_PASS_DATA, &g_ipc->pass.head);
    if (gs.pass_thread)
        SDL_WaitThread(gs.pass_thread, NULL);
    gs.pass_thread = NULL;
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
