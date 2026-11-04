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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"

#include "mh2gs.h"
#include "mh2tex.hpp"

using namespace ParallelGS;
using namespace Vulkan;

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

/* A scanout being copied to host memory, or copied (fence waited for). */
struct Readback
{
    uint64_t id = 0;
    BufferHandle buffer;
    Fence fence;                       /* until the copy has been waited for */
    const void *pixels = nullptr;      /* once it has */
    uint32_t width = 0, height = 0, mode_width = 0, mode_height = 0;
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
};

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
    b->device.init_frame_contexts(12);
    GSOptions opts = {};
    opts.super_sampling = to_ss(cfg->super_sampling);
    /* The rate is fixed for a session. Allowing changes in flight sizes VRAM
     * for 16 samples whatever the rate: about 330 MB more. */
    opts.dynamic_super_sampling = false;
    if (!b->iface.init(&b->device, opts))
        return -4;
    b->vram_size = opts.vram_size;
    b->high_resolution_scanout = cfg->high_resolution_scanout && cfg->super_sampling >= 4;
    if ((cfg->texture_dump_dir && *cfg->texture_dump_dir) || (cfg->texture_pack_dir && *cfg->texture_pack_dir))
    {
        Mh2Tex::Options to;
        if (cfg->texture_dump_dir)
            to.dump_dir = cfg->texture_dump_dir;
        if (cfg->texture_pack_dir)
            to.pack_dir = cfg->texture_pack_dir;
        if (cfg->texture_dump_limit)
            to.dump_limit = cfg->texture_dump_limit;
        if (cfg->texture_cache_mb)
            to.cache_mb = cfg->texture_cache_mb;
        to.log = cfg->log;
        to.log_user = cfg->user;
        b->textures.reset(new Mh2Tex::Textures(&b->device, opts.vram_size, to));
        b->iface.set_texture_interface(b->textures.get());
    }
    b->signals.fn = cfg->on_event;
    b->signals.user = cfg->user;
    b->iface.set_signal_interface(&b->signals);
    g = std::move(b);
    return 0;
}

extern "C" MH2GS_API void mh2gs_gif(uint32_t path, const void *data, size_t bytes)
{
    if (g && bytes)
        g->iface.gif_transfer(path, data, bytes);
}

extern "C" MH2GS_API void mh2gs_priv_write(uint32_t addr, uint64_t value)
{
    if (!g)
        return;
    auto &priv = g->iface.get_priv_register_state();
    uint32_t index = (addr & 0xFFFu) >> 3;
    if (index >= 0x200)
        return;
    if (addr & 0x1000u)
        priv.qwords_hi[index] = value;
    else
        priv.qwords_lo[index] = value;
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
    VSyncInfo info = {};
    info.phase = field;
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
    info.anti_blur = true;
    info.force_progressive = true;
    info.high_resolution_scanout = g->high_resolution_scanout;
    g->iface.flush();   /* submit the frame's pending rendering first (as paraLLEl-GS's dump replayer does) */
    auto res = g->iface.vsync(info);
    VkFormat fmt = res.image ? res.image->get_format() : VK_FORMAT_UNDEFINED;
    if (!readback || (fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_SRGB))
        return 0;   /* not wanted, no display, or a format the copy below does not expect */

    uint64_t id = ++g->readback_count;
    auto &rb = g->readbacks[id % MH2GS_READBACKS];
    if (rb.fence)
        rb.fence->wait();   /* never fetched: its copy must finish before the buffer is reused */
    rb.fence.reset();
    rb.pixels = nullptr;
    rb.id = id;
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
    return id;
}

extern "C" MH2GS_API int mh2gs_get_frame(uint64_t id, mh2gs_frame *out)
{
    if (!g || !out || !id)
        return 0;
    auto &rb = g->readbacks[id % MH2GS_READBACKS];
    if (rb.id != id)
        return 0;
    if (rb.fence)
    {
        rb.fence->wait();
        rb.fence.reset();
        /* Persistently mapped; mapping for reading invalidates non-coherent memory. */
        rb.pixels = g->device.map_host_buffer(*rb.buffer, MEMORY_ACCESS_READ_BIT);
    }
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
        g->iface.read_transfer_fifo(dst, qwords);
    else
        std::memset(dst, 0, size_t(qwords) * 16);
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

    /* Whatever is pending (junk, for a state loaded into a running session) is submitted and forgotten:
     * the local memory below replaces it. */
    g->iface.flush();
    void *vram = g->iface.map_vram_write(0, g->vram_size);
    memcpy(vram, in + STATE_META, g->vram_size);
    g->iface.end_vram_write(0, g->vram_size);

    /* The registers as they were, then the draw state derived from them. The library derives most of it from the
     * registers later, when a dirty flag says so (clobber_register_state sets them all); two values it takes
     * when the register is written, the primitive offset of the current context (XYOFFSET_1 and _2, which set it
     * only for the context PRIM names), so those are written again. A write of PRIM also starts a primitive: an
     * empty vertex queue. */
    auto &regs = g->iface.get_register_state();
    memcpy(static_cast<void *>(&regs), in + l.regs, sizeof(RegisterState));
    memcpy(static_cast<void *>(&g->iface.get_priv_register_state()), in + l.priv, sizeof(PrivRegisterState));
    g->iface.write_register(RegisterAddr::PRIM, regs.prim.bits);
    g->iface.write_register(RegisterAddr::XYOFFSET_1, regs.ctx[0].xyoffset.bits);
    g->iface.write_register(RegisterAddr::XYOFFSET_2, regs.ctx[1].xyoffset.bits);
    for (uint32_t i = 0; i < 4; i++)
        memcpy(static_cast<void *>(&g->iface.get_gif_path(i)), in + l.paths + i * sizeof(GIFPath), sizeof(GIFPath));
    g->iface.clobber_register_state();
    g->iface.set_clut_snapshot(in + l.clut);
    if (g->textures)
        g->textures->state_set((h.flags & STATE_FLAG_TEXTURES) ? in + l.tex : nullptr, in + STATE_META);
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
        g->iface.set_texture_interface(nullptr);
        g->textures.reset();   /* its images go before the device does */
    }
    g.reset();
}
