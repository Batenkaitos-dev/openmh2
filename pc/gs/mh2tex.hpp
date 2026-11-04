/* mh2tex: texture dumping and replacement for the GS backend (mh2gs).
 *
 * This file is part of the port (CC0 1.0). It is built with paraLLEl-GS (LGPL-3.0-or-later) into the
 * mh2gs library and listens to it through ParallelGS::TextureInterface.
 *
 * What a texture is. The game uploads texture data to the GS's local memory and later draws with a TEX0
 * that names a region of it. The key of a texture is what the game means by it, never where it sits:
 *   - the format class, the size, and the texel values read back out of the region (indices for the 8-
 *     and 4-bit formats, pixels for the others), and for the 16- and 24-bit formats the TEXA fields;
 *   - for a palettised texture, also the palette entries the draw can reach (as RGBA).
 * It is named  <FMT>_<W>x<H>_<texels>[_<palette>]  with 16 hex digits for each hash (FMT: C32, C24,
 * C16, T8, T4, T8H, T4HL or T4HH). A texture with a palette has the exact name and the name without the
 * palette part, which a pack can use for every palette of the texture.
 *
 * The texels come from a copy of local memory that this code keeps itself from the uploads the game
 * makes. A page that rendering or a local copy has written is not trusted again until an upload covers
 * it, so textures that were drawn by the GS are never keyed.
 *
 * Dumping writes every key once, as a PNG (RGBA8, the palette applied; alpha 255 stands for the GS's 128),
 * to the dump folder. Replacement reads PNGs from the pack folder (any folder depth, file name = key) and
 * draws a texture from its PNG when one exists. The PNG's size may be any integer multiple of the
 * original's. The GS's coordinates stay those of the original texture; the finer image is read at the
 * same fractions of it. Files are read on a worker; the original is drawn until the file is ready. */
#ifndef MH2TEX_HPP
#define MH2TEX_HPP

#include <stdint.h>
#include <string>
#include <vector>

#include "gs_interface.hpp"

namespace Mh2Tex
{
struct Options
{
    std::string dump_dir;          /* empty: no dumping */
    std::string pack_dir;          /* empty: no replacement */
    uint32_t dump_limit = 20000;   /* files per session */
    uint32_t cache_mb = 1024;      /* decoded pack files kept in memory, and again for pack images kept on the graphics card */
    void (*log)(const char *line, void *user) = nullptr;
    void *log_user = nullptr;
};

struct Counters
{
    uint64_t uploads = 0, upload_ns = 0;       /* host uploads (all of them: the time is of the whole call) */
    uint64_t uploads_skipped = 0;              /* of them: the same data again where it already was */
    uint64_t textures = 0, texture_ns = 0;     /* new cached images looked at */
    uint64_t keyed = 0, unkeyed = 0, mipmapped = 0;
    uint64_t hashed = 0;                       /* texel regions read and hashed (the rest came from the cache) */
    uint64_t dumped = 0, dump_dropped = 0, dump_existing = 0, dump_errors = 0;
    uint64_t pack_files = 0, pack_misnamed = 0;   /* files indexed, files not named like a key */
    uint64_t pack_hits = 0, pack_loaded = 0, pack_bad = 0, replaced_images = 0;   /* pack_bad: files that could not be used */
    uint64_t lookups = 0, lookup_ns = 0;       /* replacement_for calls */
    uint64_t upload_max_ns = 0, texture_max_ns = 0, lookup_max_ns = 0;   /* the longest single call of each */
};

class Textures final : public ParallelGS::TextureInterface
{
public:
    /* device may be null: keys and dumps work, nothing is replaced (the tests). */
    Textures(Vulkan::Device *device, uint32_t vram_size, const Options &options);
    ~Textures() override;

    void on_host_upload(const ParallelGS::CopyDescriptor &copy) override;
    void on_vram_damage(const ParallelGS::PageRect &rect) override;
    void on_clut_load(const ParallelGS::TEX0Bits &tex0, const ParallelGS::TEXCLUTBits &texclut) override;
    void on_new_texture(const ParallelGS::TextureDescriptor &desc) override;
    void on_texture_dead(Util::Hash hash) override;
    Vulkan::ImageHandle replacement_for(Util::Hash hash) override;

    /* Save states (mh2gs_state_get and _set). What the copy of local memory holds besides the memory itself:
     * the page states (one byte per 8 KB page) and the copy of the CLUT. state_get appends them to `out`;
     * state_set takes them back with the local memory they go with (the restored image), and makes the copy
     * from it: a page the game uploaded and nothing has written since holds what local memory holds. With
     * no state (a state made without textures), nothing is trusted until the game uploads it again. */
    static size_t state_bytes(uint32_t vram_size);
    void state_get(std::vector<uint8_t> &out) const;
    void state_set(const uint8_t *state, const uint8_t *vram);   /* state: null, or state_bytes() of them */

    /* Waits until the dump worker has written what it was given (the tests). */
    void flush_dumps();
    Counters counters() const;

private:
    struct Impl;
    Impl *impl;
};
}

#endif
