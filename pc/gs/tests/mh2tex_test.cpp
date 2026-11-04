/* The texture keys and the dump (mh2tex.cpp), without a GPU: uploads are fed to the listener as the renderer
 * would feed them, textures are made up in the test, and the PNGs the dump writes are read back and compared with
 * the pixels the GS would sample. Part of the port (CC0 1.0).
 *
 *   mh2tex_test [TEMPORARY_FOLDER]     (default: $TMPDIR, $TEMP or /tmp) */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define getpid _getpid
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STBI_ONLY_PNG
#include "../third_party/stb/stb_image.h"
#include "../third_party/stb/stb_image_write.h"

#include "../mh2tex.hpp"
#include "gs_util.hpp"

using namespace ParallelGS;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static std::vector<std::string> list_dir(const std::string &dir)
{
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    if (DIR *d = opendir(dir.c_str()))
    {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.')
                out.push_back(e->d_name);
        closedir(d);
    }
#endif
    return out;
}

static void remove_tree(const std::string &dir)
{
    for (auto &f : list_dir(dir))
    {
        std::string p = dir + "/" + f;
        remove(p.c_str());
    }
#ifdef _WIN32
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
}

static std::vector<uint8_t> g_idx;   /* the first texture, for the pack part */
static uint32_t rng_state = 12345;
static uint32_t rnd() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

struct Gs
{
    Mh2Tex::Textures *t;

    /* a HOST -> LOCAL transfer of w x h pixels of psm, in the game's linear order */
    void upload(uint32_t psm, uint32_t dbp, uint32_t dbw, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                const std::vector<uint8_t> &data)
    {
        CopyDescriptor c = {};
        c.bitbltbuf.desc.DBP = dbp;
        c.bitbltbuf.desc.DBW = dbw;
        c.bitbltbuf.desc.DPSM = psm;
        c.trxpos.desc.DSAX = x;
        c.trxpos.desc.DSAY = y;
        c.trxreg.desc.RRW = w;
        c.trxreg.desc.RRH = h;
        c.host_data = data.data();
        c.host_data_size = c.host_data_size_required = data.size();
        t->on_host_upload(c);
    }

    void load_clut(uint32_t psm, uint32_t cbp, uint32_t cpsm, uint32_t csa)
    {
        TEX0Bits tex0 = {};
        tex0.PSM = psm;
        tex0.CBP = cbp;
        tex0.CPSM = cpsm;
        tex0.CSA = csa;
        tex0.CSM = 0;
        tex0.CLD = 1;
        TEXCLUTBits cl = {};
        t->on_clut_load(tex0, cl);
    }

    void new_texture(uint32_t psm, uint32_t tbp, uint32_t tbw, uint32_t tw, uint32_t th, uint32_t cpsm, uint32_t csa,
                     uint64_t hash, uint32_t ta0 = 0x80, uint32_t ta1 = 0x80, uint32_t aem = 0)
    {
        TextureDescriptor d = {};
        d.tex0.desc.PSM = psm;
        d.tex0.desc.TBP0 = tbp;
        d.tex0.desc.TBW = tbw;
        d.tex0.desc.TW = tw;
        d.tex0.desc.TH = th;
        d.tex0.desc.CPSM = cpsm;
        d.tex0.desc.CSA = csa;
        d.texa.desc.TA0 = ta0;
        d.texa.desc.TA1 = ta1;
        d.texa.desc.AEM = aem;
        d.samples = 1;
        d.rect.x = 0;
        d.rect.y = 0;
        d.rect.width = 1u << tw;
        d.rect.height = 1u << th;
        d.rect.levels = 1;
        d.hash = hash;
        t->on_new_texture(d);
    }
};

/* the palette entry n of a 256-colour CSM1 palette is at raster position n with bits 3 and 4 swapped */
static uint32_t raster_of_entry(uint32_t n) { return (n & ~0x18u) | ((n & 8) << 1) | ((n & 16) >> 1); }

static std::vector<uint8_t> png_file(const std::string &path, int &w, int &h)
{
    int n;
    uint8_t *px = stbi_load(path.c_str(), &w, &h, &n, 4);
    std::vector<uint8_t> v;
    if (px)
    {
        v.assign(px, px + size_t(w) * h * 4);
        stbi_image_free(px);
    }
    return v;
}

int main(int argc, char **argv)
{
    std::string base = argc > 1 ? argv[1] : (getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp");
    char sub[64];
    snprintf(sub, sizeof(sub), "/mh2tex-test-%d", int(getpid()));
    std::string root = base + sub;
    std::string dump = root + "/dump", pack = root + "/pack";
#ifdef _WIN32
    _mkdir(root.c_str());
    _mkdir(pack.c_str());
#else
    mkdir(root.c_str(), 0777);
    mkdir(pack.c_str(), 0777);
#endif

    /* ---- an 8-bit texture with a 32-bit palette */
    {
        Mh2Tex::Options o;
        o.dump_dir = dump;
        o.log = [](const char *l, void *) { printf("  log: %s\n", l); };
        Mh2Tex::Textures t(nullptr, 4 << 20, o);
        Gs gs{&t};

        const uint32_t W = 64, H = 32;
        std::vector<uint8_t> idx(W * H);
        for (auto &v : idx)
            v = uint8_t(rnd());
        g_idx = idx;
        gs.upload(PSMT8, 0, 2, 0, 0, W, H, idx);   /* page 0, 128 pixels wide (TBW counts 64s) */

        std::vector<uint32_t> pal_img(256);        /* the raster image the game uploads */
        std::vector<uint32_t> entry(256);          /* the palette it means */
        for (uint32_t n = 0; n < 256; n++)
        {
            entry[n] = 0x01000000u * (rnd() & 0x80) | (rnd() & 0xffffff);
            pal_img[raster_of_entry(n)] = entry[n];
        }
        std::vector<uint8_t> pal_bytes(256 * 4);
        memcpy(pal_bytes.data(), pal_img.data(), pal_bytes.size());
        gs.upload(PSMCT32, 64, 1, 0, 0, 16, 16, pal_bytes);   /* block 64 = page 2 */
        gs.load_clut(PSMT8, 64, PSMCT32, 0);
        gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 1);
        t.flush_dumps();
        auto files = list_dir(dump);
        CHECK(files.size() == 1);
        if (files.size() == 1)
        {
            CHECK(files[0].rfind("T8_64x32_", 0) == 0);
            CHECK(files[0].size() == strlen("T8_64x32_") + 16 + 1 + 16 + 4);
            int w = 0, h = 0;
            auto px = png_file(dump + "/" + files[0], w, h);
            CHECK(w == 64 && h == 32 && px.size() == W * H * 4);
            bool same = px.size() == W * H * 4;
            for (uint32_t i = 0; same && i < W * H; i++)
            {
                uint32_t e = entry[idx[i]];
                uint32_t a = e >> 24, pa = a >= 128 ? 255 : (a * 255 + 64) / 128;
                same = px[i * 4] == (e & 255) && px[i * 4 + 1] == ((e >> 8) & 255) && px[i * 4 + 2] == ((e >> 16) & 255) &&
                       px[i * 4 + 3] == pa;
            }
            CHECK(same);
        }

        /* the same texture again: no second file; another palette: a second file */
        gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 2);
        t.flush_dumps();
        CHECK(list_dir(dump).size() == 1);
        for (auto &v : pal_img)
            v ^= 0x00ff00ffu;
        memcpy(pal_bytes.data(), pal_img.data(), pal_bytes.size());
        gs.upload(PSMCT32, 64, 1, 0, 0, 16, 16, pal_bytes);
        gs.load_clut(PSMT8, 64, PSMCT32, 0);
        gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 3);
        t.flush_dumps();
        auto two = list_dir(dump);
        CHECK(two.size() == 2);
        if (two.size() == 2)
            CHECK(two[0].substr(0, 9 + 16) == two[1].substr(0, 9 + 16));   /* the same texels */

        /* the texture moved to another place in local memory: the same key */
        gs.upload(PSMT8, 96, 2, 0, 0, W, H, idx);   /* block 96 = page 3 */
        gs.new_texture(PSMT8, 96, 2, 6, 5, PSMCT32, 0, 4);
        t.flush_dumps();
        CHECK(list_dir(dump).size() == 2);

        /* a page that rendering has written is not keyed; uploading all of it again keys it */
        auto before = t.counters();
        PageRect pr = compute_page_rect(0, 0, 0, W, H, 2, PSMT8);
        t.on_vram_damage(pr);
        gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 5);
        CHECK(t.counters().unkeyed == before.unkeyed + 1);
        std::vector<uint8_t> full(128 * 64);
        for (auto &v : full)
            v = uint8_t(rnd());
        gs.upload(PSMT8, 0, 2, 0, 0, 128, 64, full);
        before = t.counters();
        gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 6);
        CHECK(t.counters().keyed == before.keyed + 1);
        /* a page never written is not keyed either */
        before = t.counters();
        gs.new_texture(PSMT8, 640, 2, 6, 5, PSMCT32, 0, 7);
        CHECK(t.counters().unkeyed == before.unkeyed + 1);

        /* the same data to the same place again: the copy is not touched (the keys' hashes stay valid) */
        auto up0 = t.counters();
        gs.upload(PSMT8, 0, 2, 0, 0, 128, 64, full);
        CHECK(t.counters().uploads_skipped == up0.uploads_skipped + 1);
        gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 8);
        CHECK(t.counters().hashed == up0.hashed);
        full[5] ^= 1;   /* other data to that place: the copy takes it */
        gs.upload(PSMT8, 0, 2, 0, 0, 128, 64, full);
        CHECK(t.counters().uploads_skipped == up0.uploads_skipped + 1);
        gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 9);
        CHECK(t.counters().hashed == up0.hashed + 1);
    }

    /* ---- a save state: the copy of local memory is made again from the restored memory and the page states */
    {
        std::string d1 = root + "/dump3", d2 = root + "/dump4", d3 = root + "/dump5";
        const uint32_t VRAM = 4 << 20, W = 64, H = 32;
        std::vector<uint8_t> vram(VRAM, 0), idx(W * H), pal(256 * 4);
        for (auto &v : idx)
            v = uint8_t(rnd());
        for (auto &v : pal)
            v = uint8_t(rnd());
        vram_upload<PSMT8>(vram.data(), idx.data(), 0, 2, 0, 0, W, H, VRAM - 1);
        vram_upload<PSMCT32>(vram.data(), pal.data(), 64, 1, 0, 0, 16, 16, VRAM - 1);
        std::vector<uint8_t> blob;
        {
            Mh2Tex::Options o;
            o.dump_dir = d1;
            Mh2Tex::Textures t(nullptr, VRAM, o);
            Gs gs{&t};
            gs.upload(PSMT8, 0, 2, 0, 0, W, H, idx);
            gs.upload(PSMCT32, 64, 1, 0, 0, 16, 16, pal);
            gs.load_clut(PSMT8, 64, PSMCT32, 0);
            t.state_get(blob);
            CHECK(blob.size() == Mh2Tex::Textures::state_bytes(VRAM));
            gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 1);
            t.flush_dumps();
        }
        auto f1 = list_dir(d1);
        CHECK(f1.size() == 1);
        {
            /* a new object, the blob and the memory: the same texture has the same key (and nothing else is keyed) */
            Mh2Tex::Options o;
            o.dump_dir = d2;
            Mh2Tex::Textures t(nullptr, VRAM, o);
            Gs gs{&t};
            t.state_set(blob.data(), vram.data());
            gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 1);
            gs.new_texture(PSMT8, 640, 2, 6, 5, PSMCT32, 0, 2);
            t.flush_dumps();
            auto f2 = list_dir(d2);
            CHECK(f2.size() == 1 && f1.size() == 1 && f2[0] == f1[0]);
            CHECK(t.counters().unkeyed == 1);
            /* the same upload again is not skipped by a memory of the old copy */
            auto c0 = t.counters();
            gs.upload(PSMT8, 0, 2, 0, 0, W, H, idx);
            CHECK(t.counters().uploads_skipped == c0.uploads_skipped);
        }
        {
            /* a state made without textures: nothing is trusted until the game uploads it */
            Mh2Tex::Options o;
            o.dump_dir = d3;
            Mh2Tex::Textures t(nullptr, VRAM, o);
            Gs gs{&t};
            t.state_set(nullptr, vram.data());
            gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 1);
            CHECK(t.counters().unkeyed == 1 && t.counters().keyed == 0);
            std::vector<uint8_t> whole(128 * 64);   /* a page is trusted again when an upload covers all of it */
            for (auto &v : whole)
                v = uint8_t(rnd());
            std::vector<uint8_t> pal_page(64 * 32 * 4);
            for (auto &v : pal_page)
                v = uint8_t(rnd());
            gs.upload(PSMT8, 0, 2, 0, 0, 128, 64, whole);
            gs.upload(PSMCT32, 64, 1, 0, 0, 64, 32, pal_page);
            gs.load_clut(PSMT8, 64, PSMCT32, 0);
            gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 2);
            CHECK(t.counters().keyed == 1);
        }
        remove_tree(d1);
        remove_tree(d2);
        remove_tree(d3);
    }

    /* ---- a 16-bit texture: the alpha comes from TEXA */
    {
        std::string dump2 = root + "/dump2";
        Mh2Tex::Options o;
        o.dump_dir = dump2;
        Mh2Tex::Textures t(nullptr, 4 << 20, o);
        Gs gs{&t};
        const uint32_t W = 32, H = 32;
        std::vector<uint8_t> data(W * H * 2);
        for (auto &v : data)
            v = uint8_t(rnd());
        gs.upload(PSMCT16, 0, 1, 0, 0, W, H, data);
        gs.new_texture(PSMCT16, 0, 1, 5, 5, 0, 0, 1, 0x20, 0x80, 0);
        gs.new_texture(PSMCT16, 0, 1, 5, 5, 0, 0, 2, 0x40, 0x80, 0);   /* other TEXA: another key */
        t.flush_dumps();
        auto files = list_dir(dump2);
        CHECK(files.size() == 2);
        if (files.size() == 2)
        {
            CHECK(files[0].rfind("C16_32x32_", 0) == 0);
            int w = 0, h = 0;
            auto px = png_file(dump2 + "/" + files[0], w, h);
            CHECK(w == 32 && h == 32);
            /* whichever file this is, its alpha is TA0 or TA1 expanded, by the pixel's top bit */
            bool ok = px.size() == W * H * 4;
            const uint16_t *src = reinterpret_cast<const uint16_t *>(data.data());
            for (uint32_t i = 0; ok && i < W * H; i++)
            {
                uint32_t c = src[i];
                ok = px[i * 4] == ((c & 31) << 3) && px[i * 4 + 1] == (((c >> 5) & 31) << 3) &&
                     px[i * 4 + 2] == (((c >> 10) & 31) << 3) && px[i * 4 + 3] == ((c >> 15) ? 255 : (px[i * 4 + 3]));
            }
            CHECK(ok);
        }
        remove_tree(dump2);
    }

    /* ---- the pack: a file by the texels-only name is found, and the exact name wins; the images need a device */
    {
        auto files = list_dir(dump);
        CHECK(files.size() == 4);   /* the first texture with two palettes, and two uploaded after the damage */
        if (files.size() == 4)
        {
            /* the texels two of them share are the first texture's */
            size_t pick = 0;
            for (size_t i = 0; i < files.size(); i++)
                for (size_t j = i + 1; j < files.size(); j++)
                    if (files[i].substr(0, 9 + 16) == files[j].substr(0, 9 + 16))
                        pick = i;
            std::string texels_only = files[pick].substr(0, 9 + 16) + ".png";
            std::vector<uint8_t> blank(4 * 4 * 4, 0x80);
            stbi_write_png((pack + "/" + texels_only).c_str(), 4, 4, 4, blank.data(), 16);
            stbi_write_png((pack + "/T8_64x32_wrong.png").c_str(), 4, 4, 4, blank.data(), 16);
            stbi_write_png((pack + "/T9_64x32_0123456789abcdef.png").c_str(), 4, 4, 4, blank.data(), 16);
            stbi_write_png((pack + "/T8_64x_0123456789abcdef.png").c_str(), 4, 4, 4, blank.data(), 16);
            stbi_write_png((pack + "/T8_64x32_0123456789abcdeg.png").c_str(), 4, 4, 4, blank.data(), 16);
            stbi_write_png((pack + "/T8_64x32_0123456789abcdef_0123456789abcdef.png").c_str(), 4, 4, 4, blank.data(), 16);
            Mh2Tex::Options o;
            o.pack_dir = pack;
            o.log = [](const char *l, void *) { printf("  log: %s\n", l); };
            Mh2Tex::Textures t(nullptr, 4 << 20, o);
            Gs gs{&t};
            const uint32_t W = 64, H = 32;
            gs.upload(PSMT8, 0, 2, 0, 0, W, H, g_idx);
            std::vector<uint8_t> pal(256 * 4, 0x55);
            gs.upload(PSMCT32, 64, 1, 0, 0, 16, 16, pal);
            gs.load_clut(PSMT8, 64, PSMCT32, 0);
            gs.new_texture(PSMT8, 0, 2, 6, 5, PSMCT32, 0, 1);
            CHECK(t.counters().pack_files == 2);      /* the texels-only file and one well-named file; four are misnamed */
            CHECK(t.counters().pack_misnamed == 4);
            CHECK(t.counters().pack_hits == 1);   /* any palette: the texels-only name */
            std::vector<uint8_t> other(W * H, 7);
            gs.upload(PSMT8, 96, 2, 0, 0, W, H, other);
            gs.new_texture(PSMT8, 96, 2, 6, 5, PSMCT32, 0, 2);
            CHECK(t.counters().pack_hits == 1);   /* other texels: no file */
        }
    }

    remove_tree(dump);
    remove_tree(pack);
#ifdef _WIN32
    _rmdir(root.c_str());
#else
    rmdir(root.c_str());
#endif
    printf(failures ? "mh2tex_test: %d FAILED\n" : "mh2tex_test: ok\n", failures);
    return failures ? 1 : 0;
}
