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

/* 7: the preload (texture_no_preload, texture_threads, texture_progress). 6: the texture packs are a list (texture_pack_dirs), and mh2gs_abi_version is exported, so the host can look
 * before it fills a config. */
#define MH2GS_ABI_VERSION 8
/* Pictures kept for mh2gs_get_frame: an id stays valid until this many
 * later readbacks have started. */
#define MH2GS_READBACKS 16
/* The most in-between pictures of one step (mh2gs_interp_set_alphas). The readbacks above keep the real picture and these. */
#define MH2GS_MAX_MIDS 6

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
     * texture_pack_dirs, texture_pack_count: the enabled packs, in load order (a folder each; the strings are
     * copied during mh2gs_init). A PNG in a pack named by a texture's key is drawn in its place; when two packs
     * have a file for one texture, the earlier pack in the list is the one drawn.
     * texture_dump_limit: files per session (0: 20000); texture_cache_mb: decoded pack files
     * kept in memory (also held to half of the memory the machine has free), and pack images kept on the graphics card,
     * each up to this many MB (0: 1024). */
    const char *texture_dump_dir;
    const char *const *texture_pack_dirs;
    uint32_t texture_dump_limit;
    uint32_t texture_cache_mb;
    uint32_t texture_pack_count;
    /* texture_no_preload: 0 (the default) decodes the enabled packs before mh2gs_init returns, as far as texture_cache_mb
     * goes (the rest loads when first drawn); 1 loads every file on first use. texture_threads: decoding threads (0: the
     * machine's cores). texture_progress: called (on the thread that called mh2gs_init) about once a second during the
     * preload and at its end, with the files done and the files in all; user is cfg.user. */
    uint32_t texture_no_preload;
    uint32_t texture_threads;
    void (*texture_progress)(uint32_t done, uint32_t total, void *user);
    /* Messages of the texture code (one line each, no newline); NULL: stderr. May be called from the
     * renderer's thread only. `user` is passed to it. */
    void (*log)(const char *line, void *user);
    /* The in-between picture (the frame rate's stage 1; mh2interp.hpp). MH2GS_INTERP_OFF: nothing
     * extra exists and nothing extra runs. MH2GS_INTERP_ON: a second renderer is given the same stream with the vertices of
     * the path 1 kicks moved half a step back, and its picture is the one mh2gs_vsync returns for the VSync after a step's
     * chain, in place of the first VBlank's repeat. With MH2GS_INTERP_ON the one renderer draws the in-between chain before
     * the real one and takes its traces out again (mh2gs_interp_set_alphas); the second renderer exists only for the development tests. The two tests make no host difference: MH2GS_INTERP_TEST_IDENTITY
     * gives the second renderer the stream as it is and compares its picture with the real one (the second must be equal
     * pixel for pixel); MH2GS_INTERP_TEST_SKIP interpolates step n from n-1 and n+1 and compares with the real n.
     * Both log one line a step and a summary (log) and stall to compare. */
    uint32_t interpolate;
} mh2gs_config;

enum { MH2GS_INTERP_OFF = 0, MH2GS_INTERP_ON = 1, MH2GS_INTERP_TEST_IDENTITY = 2, MH2GS_INTERP_TEST_SKIP = 3 };

typedef struct mh2gs_interp_status {
    uint32_t state;        /* 0 off (not asked), 1 on, 2 unused (the library no longer refuses texture packs), 3 gave up (the graphics card was too slow), 4 could not be made */
    uint32_t pad;
    uint64_t chains;       /* steps with path 1 kicks seen */
    uint64_t cuts;         /* of them, a cut (or no earlier step): no in-between picture */
    uint64_t mids;         /* in-between pictures made and shown */
    uint64_t late;         /* of them, not ready when they were to be shown: the earlier picture was shown instead */
    uint64_t verts, v_interpolated, v_gated, v_unmatched;   /* path 1 vertices of the steps that were not cuts */
} mh2gs_interp_status;

typedef struct mh2gs_frame {
    const void *pixels;       /* RGBA8, valid as long as its id (MH2GS_READBACKS) */
    uint32_t width, height, stride;
    uint32_t mode_width, mode_height;   /* reference size for the aspect ratio */
} mh2gs_frame;

typedef int (*mh2gs_abi_version_fn)(void);
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
typedef void (*mh2gs_interp_status_fn)(mh2gs_interp_status *out);
typedef void (*mh2gs_interp_set_alphas_fn)(const double *alphas, uint32_t n);
typedef uint32_t (*mh2gs_interp_mids_fn)(uint64_t *ids, uint32_t cap);
typedef void (*mh2gs_interp_stop_fn)(void);
typedef int (*mh2gs_frame_poll_fn)(uint64_t id, mh2gs_frame *out);
typedef void (*mh2gs_notify_fn)(void *user, uint64_t id, int index);
typedef void (*mh2gs_set_notify_fn)(mh2gs_notify_fn fn, void *user);
typedef void (*mh2gs_camera_fn)(const float *m);

/* MH2GS_ABI_VERSION of the library; the host looks before it fills a config (a library without this function is
 * older than ABI 6). */
MH2GS_API int mh2gs_abi_version(void);
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
MH2GS_API void mh2gs_interp_status_get(mh2gs_interp_status *out);

/* The in-between pictures of one step (MH2GS_INTERP_ON only).
 *
 * mh2gs_interp_set_alphas: where the pictures lie between the earlier step (0) and the step whose chain has just been
 * given (1), in order of drawing: alphas[0] .. alphas[n-1], n at most MH2GS_MAX_MIDS. The default is one picture at 0.5.
 * Takes effect at the next step; may be called at any time on the renderer's thread. n = 0: the next step has no in-between
 * pictures (a step the host does not show). The vertices of a picture lie at its alpha (mh2interp: 1/2 is the mean; a kick
 * the gate holds is at the earlier step's position below 1/2). Each picture is made from the step's own stream alone, one after the other, and
 * each leaves the renderer exactly as it was, so n pictures cost n chains and do not change the real picture, whatever n is.
 *
 * mh2gs_interp_mids: after the mh2gs_vsync call that gave a step's chain, the readback ids (for mh2gs_get_frame) of that
 * step's in-between pictures, in the order of the alphas, at most cap of them. An entry is 0 for a picture that could not
 * be made. Returns the number of entries: 0 when the step had no in-between pictures (a cut, the first step, a chain that
 * read local memory, or readback = 0). The pictures are made at the chain's VSync, as is the real picture; mh2gs_vsync returns
 * the id of the first one at the VSync after, as before. Each id stays valid for MH2GS_READBACKS later readbacks;
 * a picture the graphics card has not finished is replaced by the real picture before (as before, never waited for).
 * The ids come from the same ring as the real picture's, so at most MH2GS_READBACKS - 1 pictures per step can be held at once. */
MH2GS_API void mh2gs_interp_set_alphas(const double *alphas, uint32_t n);
MH2GS_API uint32_t mh2gs_interp_mids(uint64_t *ids, uint32_t cap);
/* For a host that judges lateness itself (the timed presenter): mh2gs_frame_poll is mh2gs_get_frame for a picture whose copy may
 * not be finished: it returns 1 and the picture when it is, 0 while it is not (never waits, and never gives the real picture
 * instead), -1 when the id is no longer kept. mh2gs_interp_stop: no in-between pictures from now on (the card cannot keep up;
 * mh2gs_interp_status says 3). */
MH2GS_API void mh2gs_interp_stop(void);
MH2GS_API int mh2gs_frame_poll(uint64_t id, mh2gs_frame *out);
/* For the same host: mh2gs_set_notify(fn, user) names a function the library calls, on the thread inside mh2gs_vsync, while that call is
 * making a step's pictures (the call can take 10 ms and more with six in-between pictures; the host would otherwise learn of a finished
 * picture only when it returns). fn(user, id, index): the readback of a picture has been started (id; index -1 is the VSync's real picture,
 * k >= 0 the in-between picture of alphas[k]); or (id 0, index -2 or -3) a moment to look at the copies started before (-3: the library looks at least every half a millisecond while it works). It must not call any
 * mh2gs function but mh2gs_frame_poll and mh2gs_get_frame. The ids are those mh2gs_vsync and mh2gs_interp_mids give afterwards. NULL: none. */
MH2GS_API void mh2gs_set_notify(mh2gs_notify_fn fn, void *user);

/* The camera a model of the step is drawn with (m: 16 floats of the view matrix, then 16 of the projection, the game's row-major layout), in
 * the order of the stream: the in-between pictures of the frame rate setting use it (the ground the pairing cannot follow is moved by the
 * camera's own motion); nothing else does. A library without the setting ignores it. */
MH2GS_API void mh2gs_camera(const float *m);

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
