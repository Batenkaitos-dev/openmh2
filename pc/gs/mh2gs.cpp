/* mh2gs: the port's GS backend, a thin C bridge over paraLLEl-GS.
 *
 * This file is part of the port (CC0 1.0). It is built together with
 * paraLLEl-GS (LGPL-3.0-or-later, Arntzen Software AS) and Granite (MIT)
 * into the shared library mh2gs, which the port loads at run time; that
 * library as a whole is therefore distributed under the LGPL-3.0-or-later.
 *
 * It runs paraLLEl-GS on a headless Vulkan device and reads scanouts back
 * to host memory for the port to present. A readback is not waited for when
 * it starts but when the picture is fetched, normally a VBlank later, so
 * the CPU does not wait for the GPU to finish each frame. */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"

#include "mh2gs.h"
#include "mh2interp.hpp"
#include "mh2tex.hpp"

using namespace ParallelGS;
using namespace Vulkan;

/* The development switches (tests, tuning, traces) are read from the environment in the
 * development builds only. A release build (MH2_RELEASE) does not read them and the library does not name them. */
#define DEV_ENV(name) (static_cast<const char *>(nullptr))

extern "C" MH2GS_API size_t mh2gs_state_size(void);
extern "C" MH2GS_API int64_t mh2gs_state_get(void *dst, size_t cap);
extern "C" MH2GS_API int mh2gs_state_set(const void *src, size_t len);

namespace
{
struct Signals final : SignalInterface
{
    mh2gs_event_fn fn = nullptr;
    void *user = nullptr;
    bool on_signal(uint64_t payload) override { if (fn) fn(MH2GS_EVENT_SIGNAL, payload, user); return false; }
    bool on_finish(uint64_t payload) override { if (fn) fn(MH2GS_EVENT_FINISH, payload, user); return false; }
    bool on_label(uint64_t payload) override { if (fn) fn(MH2GS_EVENT_LABEL, payload, user); return false; }
};

void notify(uint64_t id, int index);
double now_ms();
/* While the library works inside mh2gs_vsync, the host is asked to look at the copies at least this often (ms; 0: not between). */
double s_look_ms = 0.5;
double s_look_last = 0;
/* The host looks at the copies now if it is time. */
inline void look_if_due()
{
    if (s_look_ms > 0)
    {
        double t = now_ms();
        if (t - s_look_last >= s_look_ms)
        {
            s_look_last = t;
            notify(0, -3);
        }
    }
}

/* A scanout being copied to host memory, or copied (fence waited for). */
struct Readback
{
    uint64_t id = 0;
    BufferHandle buffer;
    Fence fence;                       /* until the copy has been waited for */
    const void *pixels = nullptr;      /* once it has */
    uint32_t width = 0, height = 0, mode_width = 0, mode_height = 0;
    /* The in-between picture's: if the copy is not finished when the picture is fetched, the picture of id
     * `fallback` (the real one of the step before) is given instead and the copy is left to finish. */
    bool soft = false, judged = false, late_forced = false;
    uint64_t fallback = 0;
};

/* The vertices of the in-between pictures of a step (design B, vsync_single). One persistent thread makes the plan of the
 * step (Mh2Interp::plan_interp) when asked to, then copies the step's data and writes the vertices at each picture's
 * alpha (Mh2Interp::apply_interp, which only reads the plan and the steps), one picture after the other, while the GS
 * thread feeds and scans the earlier ones. It touches no library object and no game memory: the buffers it writes are
 * the Mid's own, the GS thread takes a picture's buffer after wait(k) returned, and the result does not depend on
 * timing (one thread, a fixed order). */
struct VertexWorker
{
    struct Batch
    {
        const Mh2Interp::Step *base = nullptr;   /* the step being drawn (kicks and vertices) */
        const Mh2Interp::Step *prev = nullptr;   /* the step before */
        const Mh2Interp::Index *prev_idx = nullptr;
        Mh2Interp::Plan *plan = nullptr;
        const std::vector<uint8_t> *src = nullptr;   /* the step's data as the game wrote it */
        std::vector<std::vector<uint8_t>> *out = nullptr;
        Mh2Interp::Params params;
        std::vector<uint32_t> alphas;   /* in 1/256, one for each picture */
        size_t first = 0;               /* the first picture the worker makes (the GS thread made the plan and the pictures before) */
        bool make_plan = false;         /* the worker makes the plan too (and makes no picture when it is a cut) */
    };

    ~VertexWorker()
    {
        if (th.joinable())
        {
            {
                std::lock_guard<std::mutex> lk(mu);
                quit = true;
            }
            cv.notify_all();
            th.join();
        }
    }

    void start(const Batch &b)
    {
        if (!th.joinable())
            th = std::thread([this] { run(); });
        {
            std::lock_guard<std::mutex> lk(mu);
            batch = b;
            done = b.first;
            plan_ready = !b.make_plan;
            plan_ok = !b.make_plan;
            pending = true;
        }
        cv.notify_all();
    }

    /* Waits for the plan; false: a cut (no pictures will be made). */
    bool wait_plan()
    {
        std::unique_lock<std::mutex> lk(mu);
        wait_looking(lk, [&] { return plan_ready; });
        return plan_ok;
    }

    /* Waits until picture k is made. */
    void wait(size_t k)
    {
        std::unique_lock<std::mutex> lk(mu);
        wait_looking(lk, [&] { return done > k; });
    }

private:
    template <class P> void wait_looking(std::unique_lock<std::mutex> &lk, P pred)
    {
        if (s_look_ms <= 0)
        {
            cv.wait(lk, pred);
            return;
        }
        while (!cv.wait_for(lk, std::chrono::duration<double, std::milli>(s_look_ms), pred))
        {
            lk.unlock();
            look_if_due();
            lk.lock();
        }
    }

    void run()
    {
        std::unique_lock<std::mutex> lk(mu);
        for (;;)
        {
            cv.wait(lk, [&] { return quit || pending; });
            if (quit)
                return;
            pending = false;
            const Batch b = batch;
            lk.unlock();
            bool ok = true;
            if (b.make_plan)
            {
                ok = Mh2Interp::plan_interp(*b.base, *b.prev, *b.prev_idx, *b.base, *b.prev_idx, b.params, *b.plan);
                lk.lock();
                plan_ok = ok;
                plan_ready = true;
                lk.unlock();
                cv.notify_all();
            }
            for (size_t k = b.first; ok && k < b.alphas.size(); k++)
            {
                std::vector<uint8_t> &d = (*b.out)[k];
                d = *b.src;
                Mh2Interp::apply_interp(*b.base, d.data(), *b.prev, *b.base, *b.plan, b.params, b.alphas[k], &scratch);
                lk.lock();
                done = k + 1;
                lk.unlock();
                cv.notify_all();
            }
            lk.lock();
        }
    }

    std::thread th;
    std::mutex mu;
    std::condition_variable cv;
    Batch batch;
    bool pending = false, quit = false, plan_ready = true, plan_ok = true;
    size_t done = 0;
    Mh2Interp::Scratch scratch;
};

/* The second renderer (the in-between picture), the parser of the stream it is given and its state. See
 * mh2interp.hpp. */
struct Mid
{
    GSInterface iface;
    GSInterface *shared = nullptr;        /* design B: the first renderer is the only one; the chain is drawn once before the real one, and its traces are taken out again */
    std::vector<double> alphas{0.5};      /* where the in-between pictures of a step lie (mh2gs_interp_set_alphas) */
    std::vector<uint64_t> mid_ids;        /* the readback ids of the last step's in-between pictures, in the order of alphas (0: none) */
    Mh2Interp::Plan plan;                 /* the last mid_plan's plan (valid when planned) */
    bool planned = false;
    std::vector<std::vector<uint8_t>> pic_data;   /* the step's data at each picture's alpha (the pictures after the first: the vertex worker's) */
    std::unique_ptr<VertexWorker> vw;
    bool tainted = false;                 /* B: the interval held a read of local memory: it was fed as it was, no in-between picture */
    GSInterface &I() { return shared ? *shared : iface; }
    Mh2Interp::Capture cap;
    Mh2Interp::Step seg;                  /* what came since the last VSync */
    Mh2Interp::Step prev;                 /* the last chain's kicks and vertices (no data) */
    Mh2Interp::Index prev_idx;
    bool have_prev = false;
    uint32_t mode = MH2GS_INTERP_ON;
    bool active = true;                   /* false: given up (too slow), nothing is fed any more */
    bool ready = false;                   /* the last chain has an in-between picture to show until the next flip */
    uint64_t mid_id = 0;                  /* its readback id once read back */
    uint32_t draw_ctx = 0;
    uint64_t last_p = 0;                  /* the last readback id of the first renderer's picture */
    uint32_t late_hist = 0, late_n = 0;   /* the last 8 in-between pictures fetched: bit set = late */
    bool wait = false;                    /* (MH2GS_MID_WAIT=1, tests: the in-between picture is waited for, as the hash and screenshot runs fetch it at once) */
    uint32_t force_late = 0, fetches = 0; /* (MH2GS_TEST_MID_LATE=N: every Nth in-between picture counts as late) */
    mh2gs_interp_status st;
    Mh2Interp::Params params;
    /* the tests */
    std::deque<Mh2Interp::Step> q;        /* skip: the segments not yet given to the second renderer */
    uint64_t test_mid = 0;                /* identity: the in-between picture waiting for the flip's picture */
    std::vector<uint8_t> prevpic;         /* the real picture shown before the one now compared (skip, for cuts) */
    struct Pending
    {
        uint64_t real = 0, mid = 0, step = 0, vs = 0;   /* readback ids: the real picture, the in-between one */
        bool cmp = false, cut = false;
        double gf = 0, uf = 0, bf = 0;   /* percent of the path 1 vertices gated, unpaired; of those in the picture, in gated or unpaired kicks */
    };
    std::vector<Pending> pending;
    uint32_t pw = 0, ph = 0;
    uint64_t nvs = 0, nvs_cmp = 0;        /* VSyncs seen; the VSync of the chain a comparison is of */
    uint64_t t_steps = 0, t_cuts = 0, t_equal = 0, t_any = 0, t_over16 = 0, t_pixels = 0, t_cut_steps = 0;
    double last_gf = 0, last_uf = 0, last_bf = 0;
    double t_psnr_sum = 0, t_psnr_min = 1e9, t_psnr_cut_sum = 0;
};

struct Backend
{
    Context ctx;
    Device device;
    GSInterface iface;
    Signals signals;
    Readback readbacks[MH2GS_READBACKS];   /* id % MH2GS_READBACKS */
    uint64_t readback_count = 0;
    bool high_resolution_scanout = false;
    std::unique_ptr<Mh2Tex::Textures> textures;   /* dumping and replacement; null when both are off */
    uint32_t vram_size = 0;
    std::unique_ptr<Mid> mid;                     /* null unless the in-between picture is on */
    uint32_t interp_state = 0;                    /* mh2gs_interp_status::state when mid is null or given up */
    void (*log)(const char *line, void *user) = nullptr;
    void *log_user = nullptr;
    /* MH2GS_COSTS=1: the CPU time of each part and the GPU time of both renderers, in the log at the end */
    bool costs = false;
    double ms_gif1 = 0, ms_gif2 = 0, ms_feed2 = 0, ms_vsync1 = 0, ms_vsync2 = 0;
    uint64_t n_vsyncs = 0;
    double ms_x[6] = {};   /* (costs) the real-first pass: real feed+scan, plan wait, rewinds, picture waits, scans, the committed pass */
    double ms_spec_begin = 0, ms_spec_draw = 0, ms_spec_end = 0;   /* the one renderer's in-between pass: CPU time in begin_speculation and the saves, in the chains, in end_speculation and the restores */
};

double now_ms()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Never destroyed by the C++ runtime: at process exit (a DLL's detach on
 * Windows) the Vulkan driver may be half gone, and the NVIDIA one crashed
 * in ~Context there. mh2gs_shutdown frees it while the driver still runs. */
std::unique_ptr<Backend> &g = *new std::unique_ptr<Backend>();

SuperSampling to_ss(uint32_t n)
{
    switch (n)
    {
    case 2: return SuperSampling::X2;
    case 4: return SuperSampling::X4;
    case 8: return SuperSampling::X8;
    case 16: return SuperSampling::X16;
    default: return SuperSampling::X1;
    }
}
}

extern "C" MH2GS_API int mh2gs_init(const mh2gs_config *cfg)
{
    if (!cfg || cfg->abi_version != MH2GS_ABI_VERSION)
        return -1;
    if (!Context::init_loader(nullptr))
        return -2;
    auto b = std::make_unique<Backend>();
    b->ctx.set_num_thread_indices(1);
    if (!b->ctx.init_instance_and_device(nullptr, 0, nullptr, 0,
                                         CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                         CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                         CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
        return -3;
    /* Software Vulkan (llvmpipe) aborts in paraLLEl-GS's scanout; its own
     * sandbox tool does the same. Refuse it rather than crash later. */
    if (b->ctx.get_gpu_props().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !getenv("MH2GS_ALLOW_CPU_DEVICE"))
        return -5;
    b->device.set_context(b->ctx);
    /* paraLLEl-GS starts a frame context in every flush_submit: in flush()
     * and vsync() each VBlank, and on pressure flushes. Starting one waits
     * for the GPU work of its previous use: with 4, vsync() waited for the
     * previous VBlank's rendering. 12, as its stream replayer uses, lets the
     * GPU lag several VBlanks without the CPU waiting. */
    b->device.init_frame_contexts(cfg->interpolate ? 24 : 12);   /* (the second renderer shares them) */
    GSOptions opts = {};
    opts.super_sampling = to_ss(cfg->super_sampling);
    /* The rate is fixed for a session. Allowing changes in flight sizes VRAM
     * for 16 samples whatever the rate: about 330 MB more. */
    opts.dynamic_super_sampling = false;
    if (!b->iface.init(&b->device, opts))
        return -4;
    b->vram_size = opts.vram_size;
    b->high_resolution_scanout = cfg->high_resolution_scanout && cfg->super_sampling >= 4;
    bool any_pack = false;
    for (uint32_t i = 0; cfg->texture_pack_dirs && i < cfg->texture_pack_count; i++)
        any_pack = any_pack || (cfg->texture_pack_dirs[i] && *cfg->texture_pack_dirs[i]);
    if ((cfg->texture_dump_dir && *cfg->texture_dump_dir) || any_pack)
    {
        Mh2Tex::Options to;
        if (cfg->texture_dump_dir)
            to.dump_dir = cfg->texture_dump_dir;
        for (uint32_t i = 0; cfg->texture_pack_dirs && i < cfg->texture_pack_count; i++)
            if (cfg->texture_pack_dirs[i] && *cfg->texture_pack_dirs[i])
                to.pack_dirs.push_back(cfg->texture_pack_dirs[i]);
        if (cfg->texture_dump_limit)
            to.dump_limit = cfg->texture_dump_limit;
        if (cfg->texture_cache_mb)
            to.cache_mb = cfg->texture_cache_mb;
        to.preload = !cfg->texture_no_preload;
        to.threads = cfg->texture_threads;
        to.progress = cfg->texture_progress;
        to.progress_user = cfg->user;
        to.log = cfg->log;
        to.log_user = cfg->user;
        b->textures.reset(new Mh2Tex::Textures(&b->device, opts.vram_size, to));
        b->iface.set_texture_interface(b->textures.get());
    }
    b->signals.fn = cfg->on_event;
    b->signals.user = cfg->user;
    b->iface.set_signal_interface(&b->signals);
    b->log = cfg->log;
    b->log_user = cfg->user;
    b->costs = DEV_ENV("MH2GS_COSTS") != nullptr;
    if (const char *e = DEV_ENV("MH2GS_LOOK_MS"))
        s_look_ms = atof(e);
    /* (tests) MH2GS_TEST_STALL=N: the restore of each speculation waits behind N copies of 128 MB on the GPU, as a busy or shared
     * GPU makes it wait now and then; MH2GS_TEST_NOSYNC=1 also takes the ordering with the async transfer queue away (the race the
     * stall shows) */
    if (const char *e = DEV_ENV("MH2GS_TEST_STALL"))
        b->iface.set_test_knobs(uint32_t(atoi(e)), DEV_ENV("MH2GS_TEST_NOSYNC") != nullptr);
    if (b->costs)
    {
        DebugMode dm = {};
        dm.timestamps = true;
        b->iface.set_debug_mode(dm);
    }
    if (cfg->interpolate)
    {
        {
            auto m = std::make_unique<Mid>();
            /* One renderer: the in-between chain goes through it before the real one (vsync_single). The development
             * tests, and MH2GS_A=1 (the oracle for the in-between pictures), give the stream to a second renderer instead. */
            const bool single = cfg->interpolate == MH2GS_INTERP_ON && !DEV_ENV("MH2GS_A");
            if (single || m->iface.init(&b->device, opts))
            {
                Hacks h = {};
                h.unsynced_readbacks = true;   /* nobody reads the second renderer's transfers: no waiting for them */
                if (single)
                    m->shared = &b->iface;
                else
                    m->iface.set_hacks(h);
                if (b->costs && !single)
                {
                    DebugMode dm = {};
                    dm.timestamps = true;
                    m->iface.set_debug_mode(dm);
                }
                m->mode = cfg->interpolate;
                m->st.state = 1;
                if (const char *e = DEV_ENV("MH2GS_TEST_MID_LATE"))
                    m->force_late = uint32_t(atoi(e));
                m->wait = DEV_ENV("MH2GS_MID_WAIT") != nullptr;
                if (const char *e = DEV_ENV("MH2GS_CUT"))   /* (tuning, tests) */
                    m->params.cut_ratio = atof(e);
                if (const char *e = DEV_ENV("MH2GS_GATE"))
                    m->params.gate_px = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_GATEON"))
                    m->params.gate_on_screen = atoi(e) != 0;
                if (const char *e = DEV_ENV("MH2GS_XYB"))
                    m->params.xy_bias = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_RGBB"))
                    m->params.rgb_bias = atoi(e);
                m->params.stats = DEV_ENV("MH2GS_VTXLOG") != nullptr;
                if (const char *e = DEV_ENV("MH2GS_REPROJ"))   /* (tuning, tests) 0: the camera does not move the kicks the pairing cannot follow */
                    m->params.reproject = atoi(e) != 0;
                if (const char *e = DEV_ENV("MH2GS_CAMCUT"))   /* (tuning) EYE:DEGREES */
                    sscanf(e, "%lf:%lf", &m->params.cam_cut_eye, &m->params.cam_cut_deg);
                if (const char *e = DEV_ENV("MH2GS_RPPAIRED"))   /* (tuning) TOLERANCE in px: paired still kicks go with the camera */
                {
                    m->params.rp_paired = atof(e) > 0;
                    if (atof(e) > 0)
                        m->params.rp_tol = atof(e);
                }
                if (const char *e = DEV_ENV("MH2GS_RPZ"))
                    m->params.rp_z = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_RPSINGLE"))
                    m->params.rp_single = atoi(e) != 0;
                if (const char *e = DEV_ENV("MH2GS_RPW"))
                    m->params.rp_w = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_KRANGE"))
                    sscanf(e, "%u:%u", &m->params.klo, &m->params.khi);
                if (const char *e = DEV_ENV("MH2GS_AMARGIN"))
                    m->params.anchor_margin = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_PERSP"))
                    m->params.persp = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_UNIFY"))
                    m->params.unify_px = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_ABL"))
                    m->params.abl = atoi(e);
                if (const char *e = DEV_ENV("MH2GS_ADC"))
                    m->params.same_adc = atoi(e) != 0;
                b->mid = std::move(m);
                b->interp_state = 1;
            }
            else
                b->interp_state = 4;
        }
    }
    g = std::move(b);
    return 0;
}

extern "C" MH2GS_API int mh2gs_abi_version(void)
{
    return MH2GS_ABI_VERSION;
}

namespace
{
/* The first renderer's scanout, copied to host memory. soft: see Readback. */
uint64_t start_readback(const ScanoutResult &res, bool soft, uint64_t fallback)
{
    uint64_t id = ++g->readback_count;
    auto &rb = g->readbacks[id % MH2GS_READBACKS];
    if (rb.fence)
        rb.fence->wait();   /* never fetched: its copy must finish before the buffer is reused */
    rb.fence.reset();
    rb.pixels = nullptr;
    rb.id = id;
    rb.soft = soft;
    rb.judged = false;
    rb.late_forced = false;
    rb.fallback = fallback;
    rb.width = res.image->get_width();
    rb.height = res.image->get_height();
    rb.mode_width = res.mode_width;
    rb.mode_height = res.mode_height;
    VkDeviceSize size = VkDeviceSize(rb.width) * rb.height * 4;
    if (!rb.buffer || rb.buffer->get_create_info().size < size)
    {
        BufferCreateInfo bi = {};
        bi.size = size;
        bi.domain = BufferDomain::CachedHost;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        rb.buffer = g->device.create_buffer(bi);
    }
    auto cmd = g->device.request_command_buffer();
    cmd->copy_image_to_buffer(*rb.buffer, *res.image, 0, {0, 0, 0}, {rb.width, rb.height, 1}, 0, 0,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    g->device.submit(cmd, &rb.fence);
    if (soft)
        g->device.flush_frame();   /* now, so that the copy is done a VBlank later when it is polled (Granite holds a submission until the next flush) */
    return id;
}

bool readback_format_ok(const ScanoutResult &res)
{
    VkFormat fmt = res.image ? res.image->get_format() : VK_FORMAT_UNDEFINED;
    return fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB;
}

VSyncInfo make_vsync_info(uint32_t field)
{
    VSyncInfo info = {};
    info.phase = field;
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
    info.anti_blur = true;
    info.force_progressive = true;
    info.high_resolution_scanout = g->high_resolution_scanout;
    return info;
}

void priv_write(GSInterface &iface, uint32_t addr, uint64_t value)
{
    auto &priv = iface.get_priv_register_state();
    uint32_t index = (addr & 0xFFFu) >> 3;
    if (index >= 0x200)
        return;
    if (addr & 0x1000u)
        priv.qwords_hi[index] = value;
    else
        priv.qwords_lo[index] = value;
}

/* Gives the second renderer what the first was given, in order. */
void mid_feed(Mid &m, const Mh2Interp::Step &s, const uint8_t *data = nullptr)
{
    if (!data)
        data = s.data.data();
    static uint8_t sink[4096 * 16];
    struct Timer
    {
        double t0 = g->costs ? now_ms() : 0;
        ~Timer() { if (g->costs) g->ms_feed2 += now_ms() - t0; }
    } timer;
    for (const auto &e : s.events)
    {
        switch (e.type)
        {
        case Mh2Interp::EV_GIF:
            m.I().gif_transfer(e.path, data + e.off, e.len);
            look_if_due();
            break;
        case Mh2Interp::EV_PRIV:
            priv_write(m.I(), e.addr, e.value);
            break;
        case Mh2Interp::EV_FIFO:
        {
            /* the game's read of the first renderer's local memory: the second one's FIFO is emptied the same way */
            uint32_t left = e.len;
            while (left)
            {
                uint32_t n = std::min<uint32_t>(left, 4096);
                m.I().read_transfer_fifo(sink, n);
                left -= n;
            }
            break;
        }
        }
    }
}

/* The context the step drew with last. */
uint32_t last_ctx(const Mh2Interp::Step &s)
{
    for (size_t i = s.kicks.size(); i-- > 0;)
        if (s.kicks[i].count)
            return (s.kicks[i].prim >> 9) & 1;
    return 0;
}

/* The second renderer's scanout. early: of the buffer the chain just drew into (the display registers' frame buffer
 * address replaced by the draw environment's FBP, for this scanout only) instead of the buffer the game shows. */
ScanoutResult mid_scan(Mid &m, uint32_t field, bool early, uint32_t ctx)
{
    auto &pv = m.I().get_priv_register_state();
    const uint64_t d1 = pv.qwords_lo[0x70 >> 3], d2 = pv.qwords_lo[0x90 >> 3];
    if (early)
    {
        uint64_t fbp = m.I().get_register_state().ctx[ctx].frame.desc.FBP;
        pv.qwords_lo[0x70 >> 3] = (d1 & ~uint64_t(0x1FF)) | fbp;
        pv.qwords_lo[0x90 >> 3] = (d2 & ~uint64_t(0x1FF)) | fbp;
    }
    m.I().flush();
    ScanoutResult res = m.I().vsync(make_vsync_info(field));
    pv.qwords_lo[0x70 >> 3] = d1;
    pv.qwords_lo[0x90 >> 3] = d2;
    return res;
}

void mid_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mid_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g->log)
        g->log(line, g->log_user);
    else
        fprintf(stderr, "%s\n", line);
}

/* A comparison of two scanouts (tests): the pixels that differ at all, by more than 16 levels (the largest channel
 * difference), the PSNR over RGB. */
struct Score
{
    uint64_t n = 0, any = 0, over16 = 0;
    double psnr = 99;
    bool ok = false;
};

Score compare_pixels(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h)
{
    Score s;
    double se = 0;
    for (size_t i = 0; i < size_t(w) * h; i++)
    {
        int d0 = int(a[4 * i]) - b[4 * i], d1 = int(a[4 * i + 1]) - b[4 * i + 1], d2 = int(a[4 * i + 2]) - b[4 * i + 2];
        if (d0 | d1 | d2)
        {
            s.any++;
            int m = std::max(std::abs(d0), std::max(std::abs(d1), std::abs(d2)));
            s.over16 += m > 16;
            se += double(d0 * d0 + d1 * d1 + d2 * d2);
        }
    }
    s.n = uint64_t(w) * h;
    s.psnr = se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * double(s.n) * 3.0 / se);
    s.ok = true;
    return s;
}

/* The pixels of a readback, waited for (tests). */
bool fetch_wait(uint64_t id, const uint8_t **px, uint32_t *w, uint32_t *h)
{
    auto &rb = g->readbacks[id % MH2GS_READBACKS];
    if (!id || rb.id != id)
        return false;
    if (rb.fence)
    {
        /* a fence of a submission Granite still holds, or of a copy of an image whose rendering a renderer still holds
         * in an open command buffer, cannot be waited for */
        g->iface.flush();
        if (g->mid && !g->mid->shared)
            g->mid->iface.flush();
        g->device.flush_frame();
        rb.fence->wait();
        rb.fence.reset();
        rb.pixels = g->device.map_host_buffer(*rb.buffer, MEMORY_ACCESS_READ_BIT);
    }
    *px = static_cast<const uint8_t *>(rb.pixels);
    *w = rb.width;
    *h = rb.height;
    return *px != nullptr;
}

void test_record(Mid &m, const char *what, uint64_t chain_no, const Score &s, bool cut)
{
    m.t_steps++;
    m.t_pixels += s.n;
    m.t_any += s.any;
    m.t_over16 += s.over16;
    m.t_equal += s.any == 0;
    m.t_psnr_sum += s.psnr;
    m.t_psnr_min = std::min(m.t_psnr_min, s.psnr);
    if (cut)
    {
        m.t_cut_steps++;
        m.t_psnr_cut_sum += s.psnr;
    }
    mid_log("interp %s step %llu at VSync %llu: %llu of %llu pixels differ, %llu by more than 16 levels, PSNR %.2f dB%s [gated %.1f %%, unpaired %.1f %%, in the picture %.1f %%]", what,
            (unsigned long long)chain_no, (unsigned long long)(m.nvs_cmp ? m.nvs_cmp : m.nvs), (unsigned long long)s.any, (unsigned long long)s.n,
            (unsigned long long)s.over16, s.psnr, cut ? " (cut)" : "", m.last_gf, m.last_uf, m.last_bf);
}

void mid_test_summary()
{
    if (!g || !g->mid || g->mid->mode < MH2GS_INTERP_TEST_IDENTITY || !g->mid->t_steps)
        return;
    Mid &m = *g->mid;
    mid_log("interp %s summary: %llu steps compared (%llu cuts), %llu equal; mean PSNR %.2f dB (lowest %.2f), pixels differing "
            "at any level %.3f %%, by more than 16 levels %.3f %%; chains %llu, interpolated vertices %.2f %%, gated %.2f %%, "
            "unmatched %.2f %%",
            m.mode == MH2GS_INTERP_TEST_IDENTITY ? "identity" : "skip", (unsigned long long)m.t_steps,
            (unsigned long long)m.t_cut_steps, (unsigned long long)m.t_equal, m.t_psnr_sum / double(m.t_steps), m.t_psnr_min,
            100.0 * double(m.t_any) / double(m.t_pixels), 100.0 * double(m.t_over16) / double(m.t_pixels),
            (unsigned long long)m.st.chains, 100.0 * double(m.st.v_interpolated) / std::max<double>(1, double(m.st.verts)),
            100.0 * double(m.st.v_gated) / std::max<double>(1, double(m.st.verts)),
            100.0 * double(m.st.v_unmatched) / std::max<double>(1, double(m.st.verts)));
}

/* Is this chain to be interpolated? Counts it. Writes the substituted vertices into c.data. */
/* (tests, MH2GS_VERIFY) What the substitution changed in the step's data: the bytes that differ from `before`, and of them the ones
 * that are not an x, y, z, rgb or st word of a path 1 vertex (the only words the substitution may write), the ones in the data of
 * path 2 and 3 (the HUD, text, fades), and the events (privileged writes, the FIFO reads) that changed (none can). */
void verify_changes(Mid &m, const Mh2Interp::Step &c, const std::vector<uint8_t> &before)
{
    static uint64_t steps = 0, changed = 0, outside = 0, in_2d = 0;
    std::vector<uint16_t> allowed(c.data.size() / 16 + 1, 0);   /* the bytes of each qword the substitution may write */
    for (const auto &v : c.verts)
    {
        if (v.xyz_off != Mh2Interp::NO_OFF)
            allowed[v.xyz_off / 16] |= 0x0033 | 0x0F00 /* bytes 0,1, 4,5, 8..11 */ | 0;
        if (v.rgb_off != Mh2Interp::NO_OFF)
            allowed[v.rgb_off / 16] |= 0x0111;   /* bytes 0, 4, 8 */
        if (v.st_off != Mh2Interp::NO_OFF)
            allowed[v.st_off / 16] |= 0x0FFF;
    }
    allowed.shrink_to_fit();
    std::vector<uint8_t> path(c.data.size() / 16 + 1, 0);
    for (const auto &e : c.events)
        if (e.type == Mh2Interp::EV_GIF)
            for (uint32_t o = e.off; o < e.off + e.len; o += 16)
                path[o / 16] = e.path;
    uint64_t ch = 0, out = 0, d2 = 0;
    for (size_t i = 0; i < c.data.size() && i < before.size(); i++)
    {
        if (c.data[i] == before[i])
            continue;
        ch++;
        if (!((allowed[i / 16] >> (i % 16)) & 1))
            out++;
        if (path[i / 16] != 1)
            d2++;
    }
    steps++;
    changed += ch;
    outside += out;
    in_2d += d2;
    if (steps % 100 == 1 || out || d2)
        mid_log("interp verify: step %llu: %llu bytes changed (all steps so far %llu), outside the vertex words %llu (%llu), in path 2 and 3 data %llu (%llu)",
                (unsigned long long)m.st.chains, (unsigned long long)ch, (unsigned long long)changed, (unsigned long long)out, (unsigned long long)outside,
                (unsigned long long)d2, (unsigned long long)in_2d);
}

uint32_t alpha_256(double a)   /* where an in-between picture lies, in 1/256 (as interpolate() takes it) */
{
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    return uint32_t(a * 256.0 + 0.5);
}

void mid_plan(Mid &m, Mh2Interp::Step &c, const Mh2Interp::Index *next_idx, const Mh2Interp::Step *next, Mh2Interp::Result &r)
{
    static const bool verify = DEV_ENV("MH2GS_VERIFY") != nullptr;
    std::vector<uint8_t> before;
    if (verify)
        before = c.data;
    m.st.chains++;
    r = Mh2Interp::Result();
    r.cut = true;
    static const int skipn = DEV_ENV("MH2GS_SKIPN") ? atoi(DEV_ENV("MH2GS_SKIPN")) : 0;   /* (tests) only every Nth chain is interpolated, the others reach the renderer as they are */
    if (m.mode != MH2GS_INTERP_TEST_IDENTITY && m.have_prev && !(skipn > 1 && (m.st.chains % uint64_t(skipn)) != 0))
    {
        static const bool livepair = DEV_ENV("MH2GS_LIVEPAIR") != nullptr;   /* (tests) the skip test with the live pairing: the mean of the step before and the step itself */
        if (next && !livepair)
            r = Mh2Interp::interpolate(c, m.prev, m.prev_idx, *next, *next_idx, m.params);
        else
        {
            /* the plan is kept: the other in-between pictures of the step (vsync_single) only write their vertices again */
            m.planned = Mh2Interp::plan_interp(c, m.prev, m.prev_idx, c, m.prev_idx, m.params, m.plan);
            r = m.plan.res;
            if (m.planned)
                r = Mh2Interp::apply_interp(c, c.data.data(), m.prev, c, m.plan, m.params, alpha_256(m.params.alpha));
        }
    }
    if (verify && !r.cut)
        verify_changes(m, c, before);
    if (r.cut)
        m.st.cuts++;
    else
    {
        m.st.verts += r.verts;
        m.st.v_interpolated += r.v_interpolated;
        m.st.v_gated += r.v_gated;
        m.st.v_unmatched += r.v_unmatched;
    }
}

/* c's kicks and vertices become the earlier step of the next match. */
void mid_remember(Mid &m, Mh2Interp::Step &c)
{
    std::swap(m.prev.kicks, c.kicks);
    std::swap(m.prev.verts, c.verts);
    memcpy(m.prev.cam, c.cam, sizeof(m.prev.cam));
    memcpy(m.prev.cam_last, c.cam_last, sizeof(m.prev.cam_last));
    m.prev.ncam = c.ncam;
    m.prev_idx.build(m.prev);
    m.have_prev = true;
}

/* Skip test: the comparisons of the earlier VSyncs. Each real picture also becomes the one before the next. */
void resolve_pending(Mid &m)
{
    for (const auto &pd : m.pending)
    {
        const uint8_t *pa = nullptr, *pb = nullptr;
        uint32_t wa = 0, ha = 0, wb = 0, hb = 0;
        if (!pd.real || !fetch_wait(pd.real, &pa, &wa, &ha))
            continue;
        m.last_gf = pd.gf;
        m.last_uf = pd.uf;
        m.last_bf = pd.bf;
        if (pd.cmp && !pd.cut && pd.mid && fetch_wait(pd.mid, &pb, &wb, &hb) && wa == wb && ha == hb)
        {
            m.nvs_cmp = pd.vs;
            /* (tests) MH2GS_TEST_DUMP=FROM:TO:PREFIX: the two pictures of each step from VSync FROM to TO, PREFIX_VSYNC.bin = w, h, real RGBA, in-between RGBA */
            unsigned long long dfrom = 0, dto = 0;
            int dpos = 0;
            const char *dd = DEV_ENV("MH2GS_TEST_DUMP");
            if (dd && sscanf(dd, "%llu:%llu:%n", &dfrom, &dto, &dpos) >= 2 && dpos > 0 && pd.vs >= dfrom && pd.vs <= dto)
            {
                char fn[512];
                snprintf(fn, sizeof(fn), "%s_%llu.bin", dd + dpos, (unsigned long long)pd.vs);
                if (FILE *f = fopen(fn, "wb"))
                {
                    uint32_t wh[2] = { wa, ha };
                    fwrite(wh, 4, 2, f);
                    fwrite(pa, 4, size_t(wa) * ha, f);
                    fwrite(pb, 4, size_t(wa) * ha, f);
                    fclose(f);
                }
            }
            test_record(m, "skip", pd.step, compare_pixels(pa, pb, wa, ha), false);
        }
        else if (pd.cmp && pd.cut && m.prevpic.size() == size_t(wa) * ha * 4)
        {
            m.nvs_cmp = pd.vs;
            test_record(m, "skip", pd.step, compare_pixels(pa, m.prevpic.data(), wa, ha), true);
        }
        m.prevpic.assign(pa, pa + size_t(wa) * ha * 4);
    }
    m.pending.clear();
}


/* Design B: one renderer. The stream of an interval is held (the capture copies it) and given to the renderer at the
 * VSync: first the in-between chains, each scanned out from the buffer the chain draws into, then the real one. The
 * in-between chains leave no trace: before them local memory is copied aside on the GPU, and afterwards the pages they
 * wrote are copied back (GSInterface::begin_speculation and end_speculation), the cached textures made from those pages
 * are dropped, the CLUT cache and the registers are put back, and so is the texture code's copy of local memory. The
 * real chain then starts from exactly the state it would have had without them. A read of local memory in the interval
 * (the core waits for its data) gives what is held on as it is, and the interval has no in-between picture. */
bool s_b_inside = false;   /* vsync_single is running: its own calls do not release the held stream */
void b_release(Mid &m)
{
    if (s_b_inside)
        return;
    Mh2Interp::Step part;
    m.cap.finish(part);
    mid_feed(m, part);
    m.tainted = true;
}

struct SavedRegs
{
    RegisterState regs;
    PrivRegisterState priv;
    GIFPath paths[4];
};

void regs_save(GSInterface &iface, SavedRegs &r)
{
    memcpy(static_cast<void *>(&r.regs), &iface.get_register_state(), sizeof(RegisterState));
    memcpy(static_cast<void *>(&r.priv), &iface.get_priv_register_state(), sizeof(PrivRegisterState));
    for (uint32_t i = 0; i < 4; i++)
        memcpy(static_cast<void *>(&r.paths[i]), &iface.get_gif_path(i), sizeof(GIFPath));
}

/* The registers as they were, then the draw state derived from them (the sequence mh2gs_state_set uses): the library
 * derives most of it from the registers later, when a dirty flag says so (clobber_register_state sets them all); two
 * values it takes when the register is written (XYOFFSET_1 and _2 set the primitive offset of the context PRIM names),
 * so those are written again. A write of PRIM also starts a primitive: an empty vertex queue. */
void regs_restore(GSInterface &iface, const SavedRegs &r)
{
    auto &regs = iface.get_register_state();
    memcpy(static_cast<void *>(&regs), &r.regs, sizeof(RegisterState));
    memcpy(static_cast<void *>(&iface.get_priv_register_state()), &r.priv, sizeof(PrivRegisterState));
    iface.write_register(RegisterAddr::PRIM, regs.prim.bits);
    iface.write_register(RegisterAddr::XYOFFSET_1, regs.ctx[0].xyoffset.bits);
    iface.write_register(RegisterAddr::XYOFFSET_2, regs.ctx[1].xyoffset.bits);
    for (uint32_t i = 0; i < 4; i++)
        memcpy(static_cast<void *>(&iface.get_gif_path(i)), &r.paths[i], sizeof(GIFPath));
    iface.clobber_register_state();
}

/* The real chain first (design B, a step with in-between pictures, at every rate). The real picture of a VSync is due a
 * VBlank after it, the in-between pictures after that; fed after all of them it was the last thing the GS thread and
 * the GPU did, and a step of six pictures made it late whenever the machine was busy. The stream of the step is now fed
 * first as a speculation, scanned out and read back at once (it starts from the state the real chain starts from, so it
 * gives the picture the serial path gives), the pages it wrote are copied back, the in-between pictures follow (their
 * plan and vertices made by the worker meanwhile), and the real chain is fed once more, this time for good (its
 * scan-out is not read: the picture is the first one's). Every picture is as before; the cost is one more drawing of
 * the real chain on the GPU for a step (at 60 as well: with one picture the real picture was a refresh late in one VSync of five). */
mh2gs_notify_fn s_notify = nullptr;
void *s_notify_user = nullptr;
void notify(uint64_t id, int index)
{
    if (s_notify)
        s_notify(s_notify_user, id, index);
}

bool rf_allowed(const Mid &m)
{
    static const bool env_ok = !DEV_ENV("MH2GS_VERIFY") && !DEV_ENV("MH2GS_TEST_DUMP") && !DEV_ENV("MH2GS_NO_RF") &&
                               !(DEV_ENV("MH2GS_SKIPN") && atoi(DEV_ENV("MH2GS_SKIPN")) > 1);
    return env_ok && m.mode == MH2GS_INTERP_ON && m.have_prev && !m.alphas.empty();
}

/* (tests) MH2GS_SERIAL=1: the GPU is waited for after every step of a chain (nothing runs ahead: no race between the queues is
 * possible); MH2GS_MIDHASH=FILE: a line for each picture of a chain, "CHAIN K HASH" (K -1: the real one), the hash of its pixels. */
void idle_if_serial(GSInterface &I)
{
    static const bool serial = DEV_ENV("MH2GS_SERIAL") != nullptr;
    if (serial)
    {
        I.flush();
        g->device.wait_idle();
    }
}

uint64_t hash_readback(uint64_t id)
{
    auto &rb = g->readbacks[id % MH2GS_READBACKS];
    if (!id || rb.id != id || !rb.buffer)
        return 0;
    if (rb.fence)
        rb.fence->wait();
    const uint8_t *p = static_cast<const uint8_t *>(g->device.map_host_buffer(*rb.buffer, MEMORY_ACCESS_READ_BIT));
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0, n = size_t(rb.width) * rb.height * 4; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

uint64_t vsync_rf(Mid &m, uint32_t field, int readback)
{
    Mh2Interp::Step &seg = m.seg;
    GSInterface &I = g->iface;
    const size_t n = m.alphas.size();
    m.planned = false;
    m.draw_ctx = last_ctx(seg);
    if (!m.vw)
        m.vw.reset(new VertexWorker);
    m.pic_data.resize(n);
    {
        VertexWorker::Batch b;
        b.base = &seg;
        b.prev = &m.prev;
        b.prev_idx = &m.prev_idx;
        b.plan = &m.plan;
        b.src = &seg.data;
        b.out = &m.pic_data;
        b.first = 0;
        b.make_plan = true;
        b.params = m.params;
        for (double a : m.alphas)
            b.alphas.push_back(alpha_256(a));
        m.vw->start(b);
    }
    double tr0 = g->costs ? now_ms() : 0;
    s_b_inside = true;
    SavedRegs saved;
    regs_save(I, saved);
    I.begin_speculation();
    if (g->textures)
        g->textures->speculation_begin();
    idle_if_serial(I);
    /* the real chain, for its picture */
    mid_feed(m, seg);
    I.flush();
    ScanoutResult real = I.vsync(make_vsync_info(field));
    idle_if_serial(I);
    uint64_t id = 0;
    if (readback_format_ok(real) && readback)
    {
        id = start_readback(real, false, 0);
        m.last_p = id;
        notify(id, -1);
    }
    double tr1 = g->costs ? now_ms() : 0;
    I.rewind_speculation();
    if (g->textures)
    {
        g->textures->speculation_end();
        g->textures->speculation_begin();
    }
    regs_restore(I, saved);
    idle_if_serial(I);
    double tw0 = g->costs ? now_ms() : 0;
    /* the plan (made by the worker): mid_plan's statistics */
    const bool ok = m.vw->wait_plan();
    if (g->costs)
    {
        g->ms_x[0] += tr1 - tr0;
        g->ms_x[1] += now_ms() - tw0;
        g->ms_x[2] += tw0 - tr1;
    }
    m.st.chains++;
    {
        /* (diagnostics) MH2GS_STEPLOG=FILE: one line a step: the kicks and vertices by what the plan made of them */
        static FILE *sl = DEV_ENV("MH2GS_STEPLOG") ? fopen(DEV_ENV("MH2GS_STEPLOG"), "w") : nullptr;
        if (sl)
        {
            const Mh2Interp::Result &q = m.plan.res;
            fprintf(sl, "%llu vs %llu cut %d kicks %u int %u gat %u gon %u unm %u (key %u cnt %u fmt %u adc %u) v %llu vint %llu vgat %llu vunm %llu (key %llu cnt %llu) von %llu vbad %llu\n",
                    (unsigned long long)m.st.chains, (unsigned long long)m.nvs, ok ? 0 : 1, q.kicks, q.interpolated, q.gated, q.g_on, q.unmatched, q.un_key, q.un_cnt,
                    q.un_fmt, q.un_adc, (unsigned long long)q.verts, (unsigned long long)q.v_interpolated, (unsigned long long)q.v_gated,
                    (unsigned long long)q.v_unmatched, (unsigned long long)q.v_un_key, (unsigned long long)q.v_un_cnt, (unsigned long long)q.v_on, (unsigned long long)q.v_on_bad);
            double dc = 0;
            for (int i = 0; i < 32; i++)
                dc = std::max(dc, double(std::fabs(seg.cam[i] - m.prev.cam[i])));
            fprintf(sl, "   camat %u/%zu cams %u dcam %.4f eye %.2f deg %.2f reproj %u vre %llu rp %llu/%llu/%llu/%llu/%llu/%llu/%llu rpz %llu/%llu/%llu/%llu/%llu/%llu/%llu kok %u kbad %u vok %llu vbad %llu bad %llu", seg.cam_at, seg.data.size(), seg.ncam, dc, q.cam_eye, q.cam_deg, q.reproj,
                    (unsigned long long)q.v_reproj, (unsigned long long)q.rp_err[0], (unsigned long long)q.rp_err[1], (unsigned long long)q.rp_err[2],
                    (unsigned long long)q.rp_err[3], (unsigned long long)q.rp_err[4], (unsigned long long)q.rp_err[5], (unsigned long long)q.rp_err[6],
                    (unsigned long long)q.rpz_err[0], (unsigned long long)q.rpz_err[1], (unsigned long long)q.rpz_err[2], (unsigned long long)q.rpz_err[3],
                    (unsigned long long)q.rpz_err[4], (unsigned long long)q.rpz_err[5], (unsigned long long)q.rpz_err[6],
                    q.rpk_ok, q.rpk_bad, (unsigned long long)q.rpv_ok, (unsigned long long)q.rpv_bad, (unsigned long long)q.rp_bad);
            fprintf(sl, "\n");
            fflush(sl);
        }
    }
    if (ok)
    {
        const Mh2Interp::Result &r = m.plan.res;
        m.planned = true;
        m.st.verts += r.verts;
        m.st.v_interpolated += r.v_interpolated;
        m.st.v_gated += r.v_gated;
        m.st.v_unmatched += r.v_unmatched;
    }
    else
        m.st.cuts++;
    m.ready = ok;
    std::vector<ScanoutResult> mids;
    m.mid_ids.assign(n, 0);
    if (ok)
        for (size_t k = 0; k < n; k++)
        {
            double t0 = g->costs ? now_ms() : 0;
            if (k)
            {
                I.rewind_speculation();
                if (g->textures)
                {
                    g->textures->speculation_end();
                    g->textures->speculation_begin();
                }
                regs_restore(I, saved);
                idle_if_serial(I);
            }
            double t1 = g->costs ? now_ms() : 0;
            m.vw->wait(k);
            double t2 = g->costs ? now_ms() : 0;
            mid_feed(m, seg, m.pic_data[k].data());
            notify(0, -2);
            double t3 = g->costs ? now_ms() : 0;
            mids.push_back(mid_scan(m, field, true, m.draw_ctx));
            idle_if_serial(I);
            if (readback_format_ok(mids[k]))
            {
                /* the copy is started at once (the scan-out image is the picture's own), and the host told */
                m.mid_ids[k] = start_readback(mids[k], !m.wait, m.last_p);
                m.st.mids++;
                notify(m.mid_ids[k], int(k));
            }
            if (g->costs)
            {
                g->ms_x[2] += t1 - t0;
                g->ms_x[3] += t2 - t1;
                g->ms_x[4] += now_ms() - t3;
            }
        }
    double tr2 = g->costs ? now_ms() : 0;
    I.end_speculation();
    if (g->textures)
        g->textures->speculation_end();
    regs_restore(I, saved);
    idle_if_serial(I);
    s_b_inside = false;
    if (g->costs)
    {
        g->ms_spec_begin += tr1 - tr0;
        g->ms_spec_draw += tr2 - tr1;
        g->ms_spec_end += now_ms() - tr2;
    }
    mid_remember(m, seg);   /* the worker has made every picture it was to: the plan is not read again */
    m.tainted = false;
    double tc0 = g->costs ? now_ms() : 0;
    mid_feed(m, seg);   /* the real chain, for good */
    I.flush();
    I.vsync(make_vsync_info(field));
    idle_if_serial(I);
    {
        static FILE *mh = DEV_ENV("MH2GS_MIDHASH") ? fopen(DEV_ENV("MH2GS_MIDHASH"), "w") : nullptr;
        static uint64_t chain_no = 0;
        if (mh)
        {
            chain_no++;
            if (id)
                fprintf(mh, "%llu -1 %016llx\n", (unsigned long long)chain_no, (unsigned long long)hash_readback(id));
            for (size_t k = 0; k < m.mid_ids.size(); k++)
                if (m.mid_ids[k])
                    fprintf(mh, "%llu %zu %016llx\n", (unsigned long long)chain_no, k, (unsigned long long)hash_readback(m.mid_ids[k]));
            fflush(mh);
        }
    }
    if (g->costs)
        g->ms_x[5] += now_ms() - tc0;
    if (!mids.empty())
        m.mid_id = m.mid_ids[0];
    else
        m.mid_ids.clear();
    return id;
}

uint64_t vsync_single(Mid &m, uint32_t field, int readback)
{
    m.nvs++;
    m.cap.finish(m.seg);
    Mh2Interp::Step &seg = m.seg;
    const bool chain = seg.p1_kicks > 0 && !m.tainted;
    m.mid_ids.clear();   /* mh2gs_interp_mids answers for the step this call gives, and 0 for a call that gives no chain */
    if (seg.flip)
    {
        m.ready = false;
        m.mid_id = 0;
    }
    std::vector<ScanoutResult> mids;
    if (chain && rf_allowed(m))
    {
        m.params.alpha = m.alphas[0];
        return vsync_rf(m, field, readback);
    }
    if (chain)
    {
        Mh2Interp::Step c = seg;   /* the in-between chain is made from a copy */
        Mh2Interp::Result r;
        m.params.alpha = m.alphas.empty() ? 0.5 : m.alphas[0];
        m.planned = false;
        if (m.alphas.empty())
        {
            /* no in-between picture asked for (the host gives none to a step it does not show): the step is only remembered */
            m.st.chains++;
            m.st.cuts++;
            r.cut = true;
        }
        else
            mid_plan(m, c, nullptr, nullptr, r);
        m.ready = !r.cut;
        m.draw_ctx = last_ctx(seg);
        if (!r.cut)
        {
            GSInterface &I = g->iface;
            if (m.alphas.size() > 1)
            {
                /* the vertices of the pictures after the first, made while the earlier ones are fed */
                if (!m.vw)
                    m.vw.reset(new VertexWorker);
                m.pic_data.resize(m.alphas.size());
                VertexWorker::Batch b;
                b.base = &c;
                b.prev = &m.prev;
                b.plan = &m.plan;
                b.src = &seg.data;
                b.out = &m.pic_data;
                b.first = 1;
                b.params = m.params;
                for (double a : m.alphas)
                    b.alphas.push_back(alpha_256(a));
                m.vw->start(b);
            }
            double tr0 = g->costs ? now_ms() : 0;
            s_b_inside = true;
            SavedRegs saved;
            regs_save(I, saved);
            I.begin_speculation();
            if (g->textures)
                g->textures->speculation_begin();
            double tr1 = g->costs ? now_ms() : 0;
            for (size_t k = 0; k < m.alphas.size(); k++)
            {
                if (k)
                {
                    /* the next picture of the step: the same chain, the vertices at another position (made meanwhile by the
                     * worker); what the earlier one left in memory is put back first (the copy made aside stays). */
                    I.rewind_speculation();
                    if (g->textures)
                    {
                        g->textures->speculation_end();
                        g->textures->speculation_begin();
                    }
                    regs_restore(I, saved);
                    m.vw->wait(k);
                    static const bool vw_verify = DEV_ENV("MH2GS_VERIFY_VW") != nullptr;
                    if (vw_verify)
                    {
                        /* (tests) the worker's data against the serial path's, byte for byte */
                        std::vector<uint8_t> ref = seg.data;
                        Mh2Interp::apply_interp(c, ref.data(), m.prev, c, m.plan, m.params, alpha_256(m.alphas[k]));
                        static uint64_t n_checked = 0, n_bad = 0;
                        n_checked++;
                        if (ref != m.pic_data[k])
                            n_bad++;
                        if (n_checked % 500 == 1 || ref != m.pic_data[k])
                            mid_log("vertex worker verify: %llu pictures checked, %llu differ", (unsigned long long)n_checked, (unsigned long long)n_bad);
                    }
                    mid_feed(m, c, m.pic_data[k].data());
                }
                else
                    mid_feed(m, c);
                mids.push_back(mid_scan(m, field, true, m.draw_ctx));
            }
            double tr2 = g->costs ? now_ms() : 0;
            I.end_speculation();
            if (g->textures)
                g->textures->speculation_end();
            regs_restore(I, saved);
            s_b_inside = false;
            if (g->costs)
            {
                g->ms_spec_begin += tr1 - tr0;
                g->ms_spec_draw += tr2 - tr1;
                g->ms_spec_end += now_ms() - tr2;
            }
        }
        mid_remember(m, c);
    }
    else
    {
        if (m.tainted)
            m.have_prev = false;
        m.ready = m.ready && !seg.flip;
    }
    m.tainted = false;
    mid_feed(m, seg);   /* the real chain, after the in-between ones */
    g->iface.flush();
    auto res = g->iface.vsync(make_vsync_info(field));
    const bool fmt_ok = readback_format_ok(res);
    const bool have_mid = !mids.empty();
    const bool use_mid = m.ready && !chain;
    uint64_t id = 0;
    if (fmt_ok && readback && !use_mid)
    {
        id = start_readback(res, false, 0);
        m.last_p = id;
    }
    if (have_mid)
    {
        m.mid_ids.assign(mids.size(), 0);
        for (size_t k = 0; k < mids.size(); k++)
            if (readback_format_ok(mids[k]))
            {
                m.mid_ids[k] = start_readback(mids[k], !m.wait, m.last_p);
                m.st.mids++;
            }
        m.mid_id = m.mid_ids[0];
        /* (tests) MH2GS_TEST_DUMP=FROM:TO:PREFIX: the real picture and the first in-between picture of each step from VSync FROM to TO,
         * PREFIX_VSYNC.bin = w, h, real RGBA, in-between RGBA (the format of the skip test's dump) */
        unsigned long long dfrom = 0, dto = 0;
        int dpos = 0;
        const char *dd = DEV_ENV("MH2GS_TEST_DUMP");
        if (dd && readback && m.mid_id && m.last_p && sscanf(dd, "%llu:%llu:%n", &dfrom, &dto, &dpos) >= 2 && dpos > 0 &&
            m.nvs >= dfrom && m.nvs <= dto)
        {
            const uint8_t *pa, *pb;
            uint32_t wa, ha, wb, hb;
            if (fetch_wait(m.last_p, &pa, &wa, &ha) && fetch_wait(m.mid_id, &pb, &wb, &hb) && wa == wb && ha == hb)
            {
                char fn[512];
                snprintf(fn, sizeof(fn), "%s_%llu.bin", dd + dpos, (unsigned long long)m.nvs);
                if (FILE *f = fopen(fn, "wb"))
                {
                    uint32_t wh[2] = { wa, ha };
                    fwrite(wh, 4, 2, f);
                    fwrite(pa, 4, size_t(wa) * ha, f);
                    fwrite(pb, 4, size_t(wa) * ha, f);
                    fclose(f);
                }
            }
        }
    }
    else if (chain)
        m.mid_ids.clear();
    if (use_mid && readback)
        id = m.mid_id;
    return id;
}

/* A VSync with the second renderer. */
uint64_t vsync_mid(Mid &m, uint32_t field, int readback)
{
    if (m.shared && m.mode == MH2GS_INTERP_ON)
        return vsync_single(m, field, readback);
    const bool test = m.mode >= MH2GS_INTERP_TEST_IDENTITY;
    m.nvs++;
    m.cap.finish(m.seg);
    Mh2Interp::Step &seg = m.seg;
    const bool chain = seg.p1_kicks > 0;
    static const bool trace = DEV_ENV("MH2GS_TRACE") != nullptr;
#define TR(...) do { if (trace) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)
    TR("vs %llu chain %d flip %d events %zu q %zu", (unsigned long long)m.nvs, (int)chain, (int)seg.flip, seg.events.size(), m.q.size());
    if (seg.flip)
    {
        m.ready = false;
        m.mid_id = 0;
    }

    /* the first renderer: the picture the game shows */
    g->iface.flush();   /* submit the frame's pending rendering first (as paraLLEl-GS's dump replayer does) */
    auto res = g->iface.vsync(make_vsync_info(field));
    TR("after vsync1");
    const bool fmt_ok = readback_format_ok(res);
    const bool use_mid = !test && m.ready && !chain;
    uint64_t id = 0, p_id = 0;
    if (fmt_ok && ((readback && !use_mid) || test))
    {
        p_id = start_readback(res, false, 0);
        m.last_p = p_id;
        if (!use_mid)
            id = p_id;
    }

    /* the second renderer: given the stream, and scanned out every VSync */
    uint32_t ctx = 0;
    bool scanned = false;
    ScanoutResult res2;
    if (test && m.mode == MH2GS_INTERP_TEST_IDENTITY)
    {
        /* the picture the real chain made (waiting at the flip), against the second renderer's earlier scan */
        if (seg.flip && m.test_mid && p_id)
        {
            const uint8_t *pa, *pb;
            uint32_t wa, ha, wb, hb;
            if (fetch_wait(p_id, &pa, &wa, &ha) && fetch_wait(m.test_mid, &pb, &wb, &hb) && wa == wb && ha == hb)
                test_record(m, "identity", m.st.chains, compare_pixels(pa, pb, wa, ha), false);
            m.test_mid = 0;
        }
        if (chain)
        {
            Mh2Interp::Result r;
            mid_plan(m, seg, nullptr, nullptr, r);
            mid_feed(m, seg);
            ctx = last_ctx(seg);
            res2 = mid_scan(m, field, true, ctx);
            scanned = true;
            if (readback_format_ok(res2))
                m.test_mid = start_readback(res2, false, 0);
            mid_remember(m, seg);
        }
        else
            mid_feed(m, seg);
    }
    else if (test)
    {
        /* skip: step n is interpolated from n-1 and n+1 once n+1 has come, and compared with the real n, which the
         * first renderer's scan shows at the VSync with the flip that shows n. The pictures are compared at the next
         * VSync (their copies cannot be waited for before the renderers have submitted what they hold). */
        TR("resolve");
        resolve_pending(m);
        TR("resolved");
        if (chain)
        {
            Mh2Interp::Index next_idx;
            next_idx.build(seg);
            while (!m.q.empty())
            {
                Mh2Interp::Step &h = m.q.front();
                if (h.p1_kicks > 0)
                {
                    Mh2Interp::Result r;
                    mid_plan(m, h, &next_idx, &seg, r);
                    if (m.params.stats && !r.cut)
                    {
                        uint64_t t = r.err[0] + r.err[1] + r.err[2] + r.err[3] + r.err[4] + r.err[5];
                        uint64_t tm = r.merr[0] + r.merr[1] + r.merr[2] + r.merr[3] + r.merr[4] + r.merr[5];
                        mid_log("interp kicks step %llu at VSync %llu: %u kicks, %u interpolated, %u gated, %u unmatched", (unsigned long long)m.st.chains, (unsigned long long)m.nvs,
                                r.kicks, r.interpolated, r.gated, r.unmatched);
                        mid_log("interp mov step %llu at VSync %llu: %llu moving vertices; error within 1/16 px %.2f %%, 1/4 %.2f, 1/2 %.2f, 1 %.2f, 2 %.2f, more %.2f", (unsigned long long)m.st.chains, (unsigned long long)m.nvs,
                                (unsigned long long)tm, 100.0 * r.merr[0] / std::max<double>(1, tm), 100.0 * r.merr[1] / std::max<double>(1, tm), 100.0 * r.merr[2] / std::max<double>(1, tm),
                                100.0 * r.merr[3] / std::max<double>(1, tm), 100.0 * r.merr[4] / std::max<double>(1, tm), 100.0 * r.merr[5] / std::max<double>(1, tm));
                        mid_log("interp vtx step %llu at VSync %llu: %llu vertices in the picture; error within 1/16 px %.2f %%, 1/4 %.2f, 1/2 %.2f, 1 %.2f, 2 %.2f, more %.2f", (unsigned long long)m.st.chains, (unsigned long long)m.nvs,
                                (unsigned long long)t, 100.0 * r.err[0] / std::max<double>(1, t), 100.0 * r.err[1] / std::max<double>(1, t), 100.0 * r.err[2] / std::max<double>(1, t),
                                100.0 * r.err[3] / std::max<double>(1, t), 100.0 * r.err[4] / std::max<double>(1, t), 100.0 * r.err[5] / std::max<double>(1, t));
                    }
                    mid_feed(m, h);
                    ctx = last_ctx(h);
                    ScanoutResult r2 = mid_scan(m, field, true, ctx);
                    Mid::Pending pd;
                    pd.real = seg.flip ? p_id : 0;
                    pd.cmp = seg.flip && p_id;
                    pd.cut = r.cut;
                    pd.step = m.st.chains;
                    pd.bf = 100.0 * double(r.v_on_bad) / std::max<double>(1, double(r.v_on));
                    pd.gf = 100.0 * double(r.v_gated) / std::max<double>(1, double(r.verts));
                    pd.uf = 100.0 * double(r.v_unmatched) / std::max<double>(1, double(r.verts));
                    pd.vs = m.nvs;
                    if (!r.cut && readback_format_ok(r2))
                        pd.mid = start_readback(r2, false, 0);
                    else if (!r.cut)
                        pd.cmp = false;
                    m.pending.push_back(pd);
                    mid_remember(m, h);
                }
                else
                    mid_feed(m, h);
                m.q.pop_front();
            }
        }
        if (seg.flip && p_id && (m.pending.empty() || m.pending.back().real != p_id))
        {
            Mid::Pending pd;
            pd.real = p_id;
            m.pending.push_back(pd);
        }
        m.q.push_back(std::move(m.seg));
        m.seg = Mh2Interp::Step();
        /* a held chain whose next one is more than a few VSyncs away (a load, a movie): a scene boundary, no comparison */
        if (m.q.size() > 6)
        {
            for (auto &h : m.q)
            {
                mid_feed(m, h);
                mid_scan(m, field, false, 0);
            }
            m.q.clear();
            m.have_prev = false;
        }
        /* what comes before the first chain not yet interpolated is given on as it comes, and scanned as the game shows it */
        while (!m.q.empty() && m.q.front().p1_kicks == 0)
        {
            mid_feed(m, m.q.front());
            mid_scan(m, field, false, 0);
            m.q.pop_front();
        }
        return id;   /* (the tests make no picture for the host) */
    }
    else
    {
        /* live */
        if (chain)
        {
            Mh2Interp::Result r;
            mid_plan(m, seg, nullptr, nullptr, r);
            m.ready = !r.cut;
            m.mid_id = 0;
            m.draw_ctx = ctx = last_ctx(seg);
            mid_feed(m, seg);
            mid_remember(m, seg);
        }
        else
            mid_feed(m, seg);
        if (use_mid)
        {
            res2 = mid_scan(m, field, true, m.draw_ctx);
            scanned = true;
            if (readback && !m.mid_id && readback_format_ok(res2))
            {
                m.mid_id = start_readback(res2, !m.wait, m.last_p);
                m.st.mids++;
            }
            if (readback)
                id = m.mid_id;
        }
    }
    if (!scanned)
        mid_scan(m, field, false, 0);
    return id;
}
}

extern "C" MH2GS_API void mh2gs_gif(uint32_t path, const void *data, size_t bytes)
{
    if (g && bytes)
    {
        double t0 = g->costs ? now_ms() : 0;
        if (!(g->mid && g->mid->active && g->mid->shared))
            g->iface.gif_transfer(path, data, bytes);
        double t1 = g->costs ? now_ms() : 0;
        if (g->mid && g->mid->active)
            g->mid->cap.gif(path, data, bytes);
        if (g->costs)
        {
            g->ms_gif1 += t1 - t0;
            g->ms_gif2 += now_ms() - t1;
        }
    }
}

extern "C" MH2GS_API void mh2gs_priv_write(uint32_t addr, uint64_t value)
{
    if (!g)
        return;
    if (!(g->mid && g->mid->active && g->mid->shared))
        priv_write(g->iface, addr, value);
    if (g->mid && g->mid->active)
        g->mid->cap.priv(addr, value);
}

/* The scanout itself runs every VBlank, even when nobody looks at the
 * picture: paraLLEl-GS keeps state across vsyncs (deinterlacing history,
 * the complex-SCANMSK timeout), so skipping some would change later ones.
 *
 * No next_frame_context() of our own: paraLLEl-GS already starts one in
 * flush() and vsync(), and each extra one shortens how far the GPU may lag
 * (see mh2gs_init). */
extern "C" MH2GS_API uint64_t mh2gs_vsync(uint32_t field, int readback)
{
    if (!g)
        return 0;
    if (g->mid && g->mid->active)
    {
        double t0 = g->costs ? now_ms() : 0;
        uint64_t r = vsync_mid(*g->mid, field, readback);
        if (g->costs)
        {
            g->ms_vsync2 += now_ms() - t0;
            g->n_vsyncs++;
        }
        return r;
    }
    g->iface.flush();   /* submit the frame's pending rendering first (as paraLLEl-GS's dump replayer does) */
    auto res = g->iface.vsync(make_vsync_info(field));
    if (!readback || !readback_format_ok(res))
        return 0;   /* not wanted, no display, or a format the copy below does not expect */
    return start_readback(res, false, 0);
}

extern "C" MH2GS_API int mh2gs_get_frame(uint64_t id, mh2gs_frame *out)
{
    if (!g || !out || !id)
        return 0;
    auto &rb = g->readbacks[id % MH2GS_READBACKS];
    if (rb.id != id)
        return 0;
    if (rb.fence && rb.soft)
    {
        /* the in-between picture: never waited for. If the card has not finished it, the picture of the step before is shown instead. */
        Mid *m = g->mid.get();
        bool late = !rb.fence->wait_timeout(0);
        if (!rb.judged)
        {
            rb.judged = true;
            if (m && m->force_late && (++m->fetches % m->force_late) == 0)
                late = rb.late_forced = true;
            if (m)
            {
                m->late_hist = (m->late_hist << 1) | (late ? 1u : 0u);
                if (m->late_n < 8)
                    m->late_n++;
                if (late)
                    m->st.late++;
                /* four of the last eight late: the card cannot keep up with two renderers; give it up */
                if (m->active && m->late_n >= 8 && __builtin_popcount(m->late_hist & 0xFF) >= 4)
                {
                    if (m->shared)
                        b_release(*m);   /* (design B) what the interval holds goes to the renderer first */
                    m->active = false;
                    g->interp_state = 3;
                    m->ready = false;
                }
            }
        }
        else
            late = late || rb.late_forced;
        if (late)
        {
            auto &fb = g->readbacks[rb.fallback % MH2GS_READBACKS];
            if (rb.fallback && fb.id == rb.fallback && !fb.soft)
                return mh2gs_get_frame(rb.fallback, out);
        }
    }
    if (rb.fence)
    {
        rb.fence->wait();
        rb.fence.reset();
        /* Persistently mapped; mapping for reading invalidates non-coherent memory. */
        rb.pixels = g->device.map_host_buffer(*rb.buffer, MEMORY_ACCESS_READ_BIT);
    }
    rb.soft = false;
    out->pixels = rb.pixels;
    out->width = rb.width;
    out->height = rb.height;
    out->stride = rb.width * 4;
    out->mode_width = rb.mode_width;
    out->mode_height = rb.mode_height;
    return rb.pixels != nullptr;
}

extern "C" MH2GS_API void mh2gs_read_fifo(void *dst, uint32_t qwords)
{
    if (g)
    {
        if (g->mid && g->mid->active && g->mid->shared)
            b_release(*g->mid);
        g->iface.read_transfer_fifo(dst, qwords);
        if (g->mid && g->mid->active && !g->mid->shared)
            g->mid->cap.fifo(qwords);
    }
    else
        std::memset(dst, 0, size_t(qwords) * 16);
}

extern "C" MH2GS_API void mh2gs_interp_set_alphas(const double *alphas, uint32_t n)
{
    if (!g || !g->mid || (n && !alphas))
        return;
    if (n > MH2GS_MAX_MIDS)
        n = MH2GS_MAX_MIDS;
    g->mid->alphas.assign(alphas, alphas + n);   /* n = 0: the next step has no in-between picture */
}

extern "C" MH2GS_API void mh2gs_interp_stop(void)
{
    if (!g || !g->mid || !g->mid->active)
        return;
    Mid &m = *g->mid;
    if (m.shared)
        b_release(m);   /* what the interval holds goes to the renderer first */
    m.active = false;
    m.ready = false;
    g->interp_state = 3;
}

extern "C" MH2GS_API void mh2gs_camera(const float *m)
{
    if (g && m && g->mid && g->mid->active)
        g->mid->cap.camera(m);
}

extern "C" MH2GS_API void mh2gs_set_notify(mh2gs_notify_fn fn, void *user)
{
    s_notify = fn;
    s_notify_user = user;
}

extern "C" MH2GS_API int mh2gs_frame_poll(uint64_t id, mh2gs_frame *out)
{
    if (!g || !out || !id)
        return -1;
    auto &rb = g->readbacks[id % MH2GS_READBACKS];
    if (rb.id != id)
        return -1;
    if (rb.fence && !rb.fence->wait_timeout(0))
        return 0;
    rb.soft = false;
    return mh2gs_get_frame(id, out) ? 1 : -1;
}

extern "C" MH2GS_API uint32_t mh2gs_interp_mids(uint64_t *ids, uint32_t cap)
{
    if (!g || !g->mid || !ids)
        return 0;
    uint32_t n = 0;
    for (uint64_t id : g->mid->mid_ids)
    {
        if (n >= cap)
            break;
        ids[n++] = id;
    }
    return n;
}

extern "C" MH2GS_API void mh2gs_interp_status_get(mh2gs_interp_status *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!g)
        return;
    if (g->mid)
        *out = g->mid->st;
    out->state = g->mid && g->mid->active ? 1 : g->interp_state;
}

/* ---- Save states (mh2gs.h has the layout).
 *
 * The block: STATE_META bytes of header and small sections, then local memory (vram_size bytes) at STATE_META.
 *   header (128 bytes)  magic, version, ABI, sizes of the sections and of the structs, flags, total, hash
 *   registers           ParallelGS::RegisterState, as the library holds it
 *   privileged          PrivRegisterState (the general ones in the first half, the others in the second)
 *   paths               the four GIFPaths (a tag may be half read)
 *   clut                the CLUT cache's current instance (CLUT_SNAPSHOT_BYTES)
 *   textures            Mh2Tex::Textures::state_get (flag 1), when textures are on
 * The hash is over everything after the header. The block is only good for the build that wrote it: the
 * structs are the library's, so their sizes are in the header and the version is checked. */
namespace
{
constexpr char STATE_MAGIC[8] = {'M', 'H', '2', 'G', 'S', 'S', 'T', '1'};
constexpr uint32_t STATE_VERSION = 1;
constexpr size_t STATE_HEADER = 128;
constexpr size_t STATE_META = 16384;
constexpr uint32_t STATE_FLAG_TEXTURES = 1;

struct StateHeader
{
    char magic[8];
    uint32_t version, abi;
    uint32_t vram_size, flags;
    uint32_t regs_size, priv_size, path_size, clut_size, tex_size;
    uint32_t total;          /* STATE_META + vram_size */
    uint64_t hash;           /* of the bytes after the header */
    uint8_t pad[72];
};
static_assert(sizeof(StateHeader) == STATE_HEADER, "StateHeader");

struct StateLayout
{
    size_t regs, priv, paths, clut, tex, end;
};

size_t align16(size_t n) { return (n + 15) & ~size_t(15); }

StateLayout state_layout(uint32_t tex_size)
{
    StateLayout l;
    l.regs = STATE_HEADER;
    l.priv = align16(l.regs + sizeof(RegisterState));
    l.paths = align16(l.priv + sizeof(PrivRegisterState));
    l.clut = align16(l.paths + 4 * sizeof(GIFPath));
    l.tex = align16(l.clut + GSInterface::CLUT_SNAPSHOT_BYTES);
    l.end = l.tex + tex_size;
    return l;
}

uint64_t state_hash(const uint8_t *p, size_t n)
{
    /* FNV-1a over 64-bit words (n is a multiple of 8 here) */
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i + 8 <= n; i += 8)
    {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    return h;
}

size_t state_tex_bytes()
{
    return g->textures ? Mh2Tex::Textures::state_bytes(g->vram_size) : 0;
}
}

extern "C" MH2GS_API size_t mh2gs_state_size(void)
{
    if (!g)
        return 0;
    return STATE_META + g->vram_size;
}

extern "C" MH2GS_API int64_t mh2gs_state_get(void *dst, size_t cap)
{
    if (!g)
        return MH2GS_STATE_NO_RENDERER;
    size_t total = STATE_META + g->vram_size;
    if (cap < total)
        return MH2GS_STATE_TOO_SMALL;
    if (g->mid && g->mid->active && g->mid->shared)
        b_release(*g->mid);
    if (!g->iface.transfer_idle())
        return MH2GS_STATE_BUSY;
    auto *out = static_cast<uint8_t *>(dst);
    memset(out, 0, STATE_META);

    uint32_t tex_size = uint32_t(state_tex_bytes());
    StateLayout l = state_layout(tex_size);
    if (l.end > STATE_META)
        return MH2GS_STATE_TOO_SMALL;   /* (a library whose structs outgrew the block) */

    g->iface.flush();   /* the pending rendering and transfers; map_vram_read waits for the GPU */
    const void *vram = g->iface.map_vram_read(0, g->vram_size);
    memcpy(out + STATE_META, vram, g->vram_size);

    memcpy(out + l.regs, &g->iface.get_register_state(), sizeof(RegisterState));
    memcpy(out + l.priv, &g->iface.get_priv_register_state(), sizeof(PrivRegisterState));
    for (uint32_t i = 0; i < 4; i++)
        memcpy(out + l.paths + i * sizeof(GIFPath), &g->iface.get_gif_path(i), sizeof(GIFPath));
    g->iface.get_clut_snapshot(out + l.clut);
    if (g->textures)
    {
        std::vector<uint8_t> t;
        g->textures->state_get(t);
        memcpy(out + l.tex, t.data(), t.size());
    }

    StateHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, STATE_MAGIC, 8);
    h.version = STATE_VERSION;
    h.abi = MH2GS_ABI_VERSION;
    h.vram_size = g->vram_size;
    h.flags = g->textures ? STATE_FLAG_TEXTURES : 0;
    h.regs_size = sizeof(RegisterState);
    h.priv_size = sizeof(PrivRegisterState);
    h.path_size = sizeof(GIFPath);
    h.clut_size = GSInterface::CLUT_SNAPSHOT_BYTES;
    h.tex_size = tex_size;
    h.total = uint32_t(total);
    h.hash = state_hash(out + STATE_HEADER, total - STATE_HEADER);
    memcpy(out, &h, sizeof(h));
    return int64_t(total);
}

extern "C" MH2GS_API int mh2gs_state_set(const void *src, size_t len)
{
    if (!g)
        return MH2GS_STATE_NO_RENDERER;
    if (!src || len < STATE_META)
        return MH2GS_STATE_BAD;
    const auto *in = static_cast<const uint8_t *>(src);
    StateHeader h;
    memcpy(&h, in, sizeof(h));
    if (memcmp(h.magic, STATE_MAGIC, 8) || h.version != STATE_VERSION || h.abi != MH2GS_ABI_VERSION ||
        h.regs_size != sizeof(RegisterState) || h.priv_size != sizeof(PrivRegisterState) ||
        h.path_size != sizeof(GIFPath) || h.clut_size != GSInterface::CLUT_SNAPSHOT_BYTES ||
        h.tex_size != ((h.flags & STATE_FLAG_TEXTURES) ? Mh2Tex::Textures::state_bytes(h.vram_size) : 0) ||
        h.total != len || h.total != STATE_META + size_t(h.vram_size))
        return MH2GS_STATE_BAD;
    if (h.vram_size != g->vram_size)
        return MH2GS_STATE_MISMATCH;
    StateLayout l = state_layout(h.tex_size);
    if (l.end > STATE_META || state_hash(in + STATE_HEADER, len - STATE_HEADER) != h.hash)
        return MH2GS_STATE_BAD;

    auto apply = [&](GSInterface &iface, Mh2Tex::Textures *tex) {
        /* Whatever is pending (junk, for a state loaded into a running session) is submitted and forgotten:
         * the local memory below replaces it. */
        iface.flush();
        void *vram = iface.map_vram_write(0, g->vram_size);
        memcpy(vram, in + STATE_META, g->vram_size);
        iface.end_vram_write(0, g->vram_size);

        /* The registers as they were, then the draw state derived from them. The library derives most of it from the
         * registers later, when a dirty flag says so (clobber_register_state sets them all); two values it takes
         * when the register is written, the primitive offset of the current context (XYOFFSET_1 and _2, which set it
         * only for the context PRIM names), so those are written again. A write of PRIM also starts a primitive: an
         * empty vertex queue. */
        auto &regs = iface.get_register_state();
        memcpy(static_cast<void *>(&regs), in + l.regs, sizeof(RegisterState));
        memcpy(static_cast<void *>(&iface.get_priv_register_state()), in + l.priv, sizeof(PrivRegisterState));
        iface.write_register(RegisterAddr::PRIM, regs.prim.bits);
        iface.write_register(RegisterAddr::XYOFFSET_1, regs.ctx[0].xyoffset.bits);
        iface.write_register(RegisterAddr::XYOFFSET_2, regs.ctx[1].xyoffset.bits);
        for (uint32_t i = 0; i < 4; i++)
            memcpy(static_cast<void *>(&iface.get_gif_path(i)), in + l.paths + i * sizeof(GIFPath), sizeof(GIFPath));
        iface.clobber_register_state();
        iface.set_clut_snapshot(in + l.clut);
        if (tex)
            tex->state_set((h.flags & STATE_FLAG_TEXTURES) ? in + l.tex : nullptr, in + STATE_META);
    };
    apply(g->iface, g->textures.get());
    if (g->mid)
    {
        /* the second renderer is given the same memory and registers, and starts its matching over */
        Mid &m = *g->mid;
        if (!m.shared)
            apply(m.iface, nullptr);
        m.cap.reset();
        m.seg.clear();
        m.prev.clear();
        m.prev_idx.clear();
        m.q.clear();
        m.have_prev = false;
        m.ready = false;
        m.mid_id = 0;
        m.test_mid = 0;
        m.prevpic.clear();
    }
    return 0;
}

extern "C" MH2GS_API void mh2gs_shutdown(void)
{
    /* Submit what the renderer still holds first: Device::wait_idle waits
     * until every command buffer requested has been submitted, and the
     * renderer keeps its open ones until a flush (its destructor flushes,
     * for that reason, but it runs after). Without it a session that ended
     * between a GS transfer and the next VBlank's flush hung at exit
     * (seen on Windows). */
    if (g) {
        g->iface.flush();
        g->device.wait_idle();
        mid_test_summary();
        if (g->mid && g->mid->mode == MH2GS_INTERP_ON)
            mid_log("in-between frames: %llu steps drawn, %llu without an in-between frame (a cut or no step before), %llu in-between frames shown, %llu of them not ready in time (the earlier frame was shown)%s",
                    (unsigned long long)g->mid->st.chains, (unsigned long long)g->mid->st.cuts, (unsigned long long)g->mid->st.mids,
                    (unsigned long long)g->mid->st.late, g->mid->active ? "" : "; given up, 30 frames per second from there");
        if (g->costs)
        {
            g->iface.flush();
            g->device.wait_idle();
            static const char *const names[] = {"SyncHostToVRAM", "CopyVRAM", "PaletteUpdate", "TextureUpload", "TriangleSetup",
                                                "Binning", "Shading", "Readback", "VSync", "Speculation"};
            char l1[600] = "", l2[600] = "";
            double s1 = 0, s2 = 0;
            for (int i = 0; i < 10; i++)
            {
                double a = g->iface.get_accumulated_timestamps(TimestampType(i));
                double b = g->mid && !g->mid->shared ? g->mid->iface.get_accumulated_timestamps(TimestampType(i)) : 0;
                s1 += a;
                s2 += b;
                snprintf(l1 + strlen(l1), sizeof(l1) - strlen(l1), " %s %.1f", names[i], a * 1000.0);
                snprintf(l2 + strlen(l2), sizeof(l2) - strlen(l2), " %s %.1f", names[i], b * 1000.0);
            }
            mid_log("costs: GPU ms in all: first renderer %.1f (%s), second %.1f (%s)", s1 * 1000.0, l1, s2 * 1000.0, l2);
            mid_log("costs: CPU ms in all: gif_transfer first %.1f, parse and copy for the second %.1f, second renderer fed %.1f, the mid VSync work %.1f (%llu VSyncs); in-between pass (one renderer): begin %.1f, chains %.1f, end %.1f",
                    g->ms_gif1, g->ms_gif2, g->ms_feed2, g->ms_vsync2, (unsigned long long)g->n_vsyncs, g->ms_spec_begin, g->ms_spec_draw, g->ms_spec_end);
            mid_log("costs: real-first pass, CPU ms in all: real feed and scan %.1f, wait for the plan %.1f, rewinds %.1f, waits for pictures %.1f, scans %.1f, the committed pass %.1f",
                    g->ms_x[0], g->ms_x[1], g->ms_x[2], g->ms_x[3], g->ms_x[4], g->ms_x[5]);
        }
        if (g->mid)
        {
            if (!g->mid->shared)
                g->mid->iface.flush();
            g->device.wait_idle();
            g->mid.reset();
        }
        g->iface.set_texture_interface(nullptr);
        g->textures.reset();   /* its images go before the device does */
    }
    g.reset();
}
