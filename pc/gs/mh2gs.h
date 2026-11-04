/* C interface of the GS backend library (mh2gs), which wraps paraLLEl-GS.
 *
 * The port's executable loads this library at run time (SDL_LoadObject),
 * so paraLLEl-GS (LGPL-3.0-or-later) stays in its own, replaceable shared
 * library. This header and mh2gs.cpp are part of the port (CC0 1.0); the built
 * library links paraLLEl-GS and is distributed under the LGPL-3.0-or-later. */
#ifndef MH2GS_H
#define MH2GS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MH2GS_API __declspec(dllexport)
#else
#define MH2GS_API __attribute__((visibility("default")))
#endif

#define MH2GS_ABI_VERSION 5
/* Pictures kept for mh2gs_get_frame: an id stays valid until this many
 * later readbacks have started. */
#define MH2GS_READBACKS 3

/* GS events the host must turn into CSR bits / INTC_GS interrupts. */
enum { MH2GS_EVENT_SIGNAL = 0, MH2GS_EVENT_FINISH = 1, MH2GS_EVENT_LABEL = 2 };
typedef void (*mh2gs_event_fn)(int event, uint64_t payload, void *user);

typedef struct mh2gs_config {
    uint32_t abi_version;     /* MH2GS_ABI_VERSION */
    uint32_t super_sampling;  /* 1, 2, 4, 8 or 16 (samples per pixel) */
    mh2gs_event_fn on_event;
    void *user;
    /* With super_sampling >= 4: scan out the samples at twice the width and
     * height (paraLLEl-GS's high_resolution_scanout). The guest's view of
     * VRAM stays single-sampled. */
    uint32_t high_resolution_scanout;
    /* Textures (mh2tex.hpp). Folders as UTF-8 paths; empty or NULL: off.
     * texture_dump_dir: every texture the game draws is written there once, as a PNG named by its key.
     * texture_pack_dir: a PNG there named by a texture's key is drawn in its place.
     * texture_dump_limit: files per session (0: 20000); texture_cache_mb: decoded pack files
     * kept in memory, and pack images kept on the graphics card, each up to this many MB (0: 1024). */
    const char *texture_dump_dir;
    const char *texture_pack_dir;
    uint32_t texture_dump_limit;
    uint32_t texture_cache_mb;
    /* Messages of the texture code (one line each, no newline); NULL: stderr. May be called from the
     * renderer's thread only. `user` is passed to it. */
    void (*log)(const char *line, void *user);
} mh2gs_config;

typedef struct mh2gs_frame {
    const void *pixels;       /* RGBA8, valid as long as its id (MH2GS_READBACKS) */
    uint32_t width, height, stride;
    uint32_t mode_width, mode_height;   /* reference size for the aspect ratio */
} mh2gs_frame;

typedef int (*mh2gs_init_fn)(const mh2gs_config *cfg);
typedef void (*mh2gs_gif_fn)(uint32_t path, const void *data, size_t bytes);
typedef void (*mh2gs_priv_write_fn)(uint32_t addr, uint64_t value);
typedef uint64_t (*mh2gs_vsync_fn)(uint32_t field, int readback);
typedef int (*mh2gs_get_frame_fn)(uint64_t id, mh2gs_frame *out);
typedef void (*mh2gs_read_fifo_fn)(void *dst, uint32_t qwords);
typedef void (*mh2gs_shutdown_fn)(void);
typedef int64_t (*mh2gs_state_get_fn)(void *dst, size_t cap);
typedef int (*mh2gs_state_set_fn)(const void *src, size_t len);
typedef size_t (*mh2gs_state_size_fn)(void);

/* Returns 0 on success (a Vulkan device with the features paraLLEl-GS needs);
 * -1 ABI mismatch, -2 no Vulkan loader, -3 no suitable device, -4 paraLLEl-GS
 * init failed, -5 software rasterizer only (see mh2gs.cpp). */
MH2GS_API int mh2gs_init(const mh2gs_config *cfg);
/* GIF data for PATH1-3: whole 128-bit words, GIFtags included. */
MH2GS_API void mh2gs_gif(uint32_t path, const void *data, size_t bytes);
/* Privileged register write (0x12000000-0x120000E0, 0x12001000-0x12001080). */
MH2GS_API void mh2gs_priv_write(uint32_t addr, uint64_t value);
/* Scan out the current display. With `readback`, the picture's copy to host
 * memory is started but not waited for; the result names it for
 * mh2gs_get_frame. Returns 0 when the display is off or no readback was
 * asked for. */
MH2GS_API uint64_t mh2gs_vsync(uint32_t field, int readback);
/* The picture of the mh2gs_vsync call that returned `id`, waiting for its
 * copy if the GPU has not finished it yet. Returns 1 with the frame, 0 if
 * `id` is no longer kept (MH2GS_READBACKS). */
MH2GS_API int mh2gs_get_frame(uint64_t id, mh2gs_frame *out);
/* Local->host transfer data (TRXDIR = 1), as read through the VIF1/GIF FIFO. */
MH2GS_API void mh2gs_read_fifo(void *dst, uint32_t qwords);
MH2GS_API void mh2gs_shutdown(void);

/* Save states. The renderer's part of a state: the GS's local memory (4 MB), the registers (the general and the
 * privileged ones, the GIF paths), the CLUT cache, and the texture code's copy of local memory (mh2tex). One
 * block, written by mh2gs_state_get and read by mh2gs_state_set, which a state file carries as the host's
 * section; its layout is mh2gs.cpp's, and only the
 * same build of mh2gs reads it back (the block says what it was made by). Like every call, both are made from
 * the thread that made the renderer, and the state is that of the ring records handled so far: call them
 * between records, after a VSync for a state at a VBlank. They wait for the GPU. Not saved, and rebuilt by the
 * frames that follow: the pictures kept for mh2gs_get_frame, the vertex queue (a primitive's start resets it),
 * the textures' image caches, the super-sampled samples (a state keeps the single-sample picture).
 * A movie is not part of the block: it is the host's (the core and the host refuse a state while one is open). */
enum {
    MH2GS_STATE_NO_RENDERER = -1,   /* no renderer (mh2gs_init failed or was not called) */
    MH2GS_STATE_BUSY = -2,          /* get: a transfer is half sent or half read; ask again after the next record */
    MH2GS_STATE_TOO_SMALL = -3,     /* get: cap is less than mh2gs_state_size() */
    MH2GS_STATE_BAD = -4,           /* set: not a block of this build of mh2gs, or damaged */
    MH2GS_STATE_MISMATCH = -5       /* set: made with other settings (memory size) */
};
/* The bytes mh2gs_state_get writes: the same every time in a session. 0 without a renderer. */
MH2GS_API size_t mh2gs_state_size(void);
/* Writes the block (at most cap bytes: use mh2gs_state_size). Returns its length, or an MH2GS_STATE_* code. */
MH2GS_API int64_t mh2gs_state_get(void *dst, size_t cap);
/* Makes the renderer's state that of a block. Returns 0, or an MH2GS_STATE_* code (the state is then unchanged
 * when the block was refused before it was applied: BAD and MISMATCH). */
MH2GS_API int mh2gs_state_set(const void *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif
