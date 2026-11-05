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

#include "mh2host.h"
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
    uint8_t *movie;
    size_t movie_cap;
    uint32_t movie_w, movie_h;
} gs;


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
    cfg.on_event = on_event;
    /* the textures (off unless asked; the folders are the textures folder's dump and packs) */
    char tex_dump[1100] = "", tex_packs[1100] = "";
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
            if (g_cfg.texture_packs)
                snprintf(tex_packs, sizeof(tex_packs), "%s/packs", root);
            cfg.texture_dump_dir = tex_dump;
            cfg.texture_pack_dir = tex_packs;
            cfg.log = on_gs_log;
        }
    }
    int r = gs.init(&cfg);
    if (r == -4 && cfg.super_sampling > 1) {
        /* paraLLEl-GS could not be made with these samples (memory, most likely): the same renderer at one sample,
         * and the log and the session summary say so (settings_enhancements) */
        host_log("GS renderer: %ux super-sampling%s could not be made (paraLLEl-GS init failed, %d): trying one sample",
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
        return;
    }
    gs.ok = 1;
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

static void on_vsync(const Mh2IpcRec *rec, const unsigned char *p)
{
    uint64_t vblank;
    memcpy(&vblank, p, 8);
    SDL_AddAtomicU32(&gs.vsyncs, 1);
    SDL_SetAtomicU32(&gs.last_vblank, (Uint32)vblank);
    /* a movie playing (movie.c) replaces the GS's picture */
    const uint8_t *mpix = NULL;
    uint32_t mw = 0, mh = 0;
    int movie = movie_vsync(rec->arg1, &mpix, &mw, &mh);
    if (!gs.ok)
        return;
    int show = !g_cfg.no_window;
    int photo = SDL_GetAtomicInt(&gs.photo_req) && vblank >= gs.photo_after;
    uint64_t id = gs.vsync(rec->arg0, !movie && (show || photo));
    if (show && gs.prev_movie) {
        present_submit(gs.movie, gs.movie_w, gs.movie_h, gs.movie_w * 4, gs.prev_vblank, 1);
        mh2ipc_store(&g_ipc->presented_lo, (unsigned int)gs.prev_vblank);
        mh2ipc_store(&g_ipc->presented_hi, (unsigned int)(gs.prev_vblank >> 32));
    } else if (show && gs.prev_id) {
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
        if (gs.ok) {
            uint64_t v;
            memcpy(&v, p, 8);
            gs.priv_write(rec->arg0, v);
        }
        break;
    case MH2IPC_REC_VSYNC:
        on_vsync(rec, p);
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
            if (!mh2ipc_rec_peek(r, g_block))
                mh2ipc_wait(&g_map, MH2IPC_EV_GS_DATA, &r->head, head, 20);
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
    SDL_WaitSemaphore(gs.ready);
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
