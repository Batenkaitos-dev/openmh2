/* The renderer's save state (mh2gs_state_get and _set), without the core: it draws, saves, draws something else,
 * restores, draws again and compares the pictures. Needs a Vulkan GPU (it exits 77, "skipped", where mh2gs finds
 * none). Part of the port (CC0 1.0).
 *
 *   mh2gs_state_test [SEED]
 *
 * The scenes are GIF streams made here (A+D packets and image transfers on PATH3): a frame buffer and a Z buffer
 * the next scene depends on, 8-bit textures with CLUTs, translucent sprites that read the frame buffer, and a
 * scene that draws with a CLUT it does not load (CLD = 0) after the CLUT's local memory was overwritten, so only
 * the CLUT cache holds the palette it must use. */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../mh2gs.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t rng_state = 1;
static uint32_t rnd() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static uint32_t rnd_to(uint32_t n) { return rnd() % n; }

/* ---- GIF packets */

enum { PRIM = 0x00, RGBAQ = 0x01, UV = 0x03, XYZ2 = 0x05, TEX0_1 = 0x06, TEX1_1 = 0x14, XYOFFSET_1 = 0x18,
       PRMODECONT = 0x1A, TEXA = 0x3B, SCISSOR_1 = 0x40, ALPHA_1 = 0x42, COLCLAMP = 0x46, TEST_1 = 0x47,
       FRAME_1 = 0x4C, ZBUF_1 = 0x4E, BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52, TRXDIR = 0x53,
       TEXFLUSH = 0x3F };

struct Gif
{
    std::vector<uint64_t> out;
    std::vector<uint64_t> ads;
    void ad(uint32_t reg, uint64_t data) { ads.push_back(data); ads.push_back(reg); }
    void flush_ads()
    {
        if (ads.empty())
            return;
        uint64_t n = ads.size() / 2;
        out.push_back(n | (1ull << 15) | (0ull << 58) | (1ull << 60));   /* NLOOP, EOP, PACKED, NREG 1 */
        out.push_back(0xE);                                                /* REGS: A+D */
        out.insert(out.end(), ads.begin(), ads.end());
        ads.clear();
    }
    void image(const void *data, size_t bytes)   /* bytes a multiple of 16 */
    {
        flush_ads();
        out.push_back((bytes / 16) | (1ull << 15) | (2ull << 58));         /* IMAGE */
        out.push_back(0);
        const uint64_t *p = static_cast<const uint64_t *>(data);
        out.insert(out.end(), p, p + bytes / 8);
    }
    void send()
    {
        flush_ads();
        if (!out.empty())
            mh2gs_gif(3, out.data(), out.size() * 8);
        out.clear();
    }
};

static uint64_t frame_reg(uint32_t fbp, uint32_t fbw, uint32_t psm) { return fbp | (uint64_t(fbw) << 16) | (uint64_t(psm) << 24); }
static uint64_t tex0(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cbp, uint32_t cld)
{
    return tbp | (uint64_t(tbw) << 14) | (uint64_t(psm) << 20) | (uint64_t(tw) << 26) | (uint64_t(th) << 30) | (1ull << 34) |
           (uint64_t(cbp) << 37) | (uint64_t(0) << 51) | (uint64_t(cld) << 61);
}

static const uint32_t FB_W = 640, FB_H = 448, OFS = 1728, OFSY = 1824;   /* the offsets that put (0,0) at the corner */
static const uint32_t ZBP = 140, TEX_A = 9000, TEX_B = 9100, CLUT_A = 12000, CLUT_B = 12100;

static void upload(Gif &g, uint32_t dbp, uint32_t dbw, uint32_t psm, uint32_t w, uint32_t h, const void *data, size_t bytes)
{
    g.ad(BITBLTBUF, (uint64_t(dbp) << 32) | (uint64_t(dbw) << 48) | (uint64_t(psm) << 56));
    g.ad(TRXPOS, 0);
    g.ad(TRXREG, w | (uint64_t(h) << 32));
    g.ad(TRXDIR, 0);
    g.image(data, bytes);
}

static void upload_texture(Gif &g, uint32_t tbp, uint32_t seed)
{
    std::vector<uint8_t> t(128 * 128);
    rng_state = seed;
    for (auto &b : t)
        b = uint8_t(rnd());
    upload(g, tbp, 2, 0x13, 128, 128, t.data(), t.size());
}

static void upload_clut(Gif &g, uint32_t cbp, uint32_t seed)
{
    std::vector<uint32_t> c(256);
    rng_state = seed;
    for (auto &v : c)
        v = (rnd() & 0x00FFFFFF) | (0x20u + rnd_to(0x60)) << 24;
    upload(g, cbp, 1, 0, 16, 16, c.data(), c.size() * 4);
}

static void setup(Gif &g)
{
    g.ad(FRAME_1, frame_reg(0, 10, 0));
    g.ad(ZBUF_1, ZBP);                                 /* Z32, writes on */
    g.ad(XYOFFSET_1, uint64_t(OFS << 4) | (uint64_t(OFSY << 4) << 32));
    g.ad(SCISSOR_1, 0ull | (uint64_t(FB_W - 1) << 16) | (0ull << 32) | (uint64_t(FB_H - 1) << 48));
    g.ad(TEST_1, (1ull << 16) | (2ull << 17));        /* Z test on, GEQUAL */
    g.ad(ALPHA_1, 0 | (1u << 2) | (0u << 4) | (1u << 6));   /* (Cs - Cd) * As + Cd */
    g.ad(COLCLAMP, 1);
    g.ad(PRMODECONT, 1);
    g.ad(TEX1_1, 0);                                   /* nearest */
    g.ad(TEXA, 0x80 | (0x80ull << 32));
}

static void sprite(Gif &g, int x0, int y0, int x1, int y1, uint32_t z, uint32_t rgba, bool tex, int u0, int v0, int u1, int v1)
{
    g.ad(RGBAQ, rgba | (0x3F800000ull << 32));
    if (tex)
        g.ad(UV, uint64_t(u0 << 4) | (uint64_t(v0 << 4) << 16));
    g.ad(XYZ2, uint64_t((x0 + OFS) << 4) | (uint64_t((y0 + OFSY) << 4) << 16) | (uint64_t(z) << 32));
    if (tex)
        g.ad(UV, uint64_t(u1 << 4) | (uint64_t(v1 << 4) << 16));
    g.ad(XYZ2, uint64_t((x1 + OFS) << 4) | (uint64_t((y1 + OFSY) << 4) << 16) | (uint64_t(z) << 32));
}

/* n random sprites. Textured ones read the texture the last TEX0 names. */
static void sprites(Gif &g, uint32_t seed, int n, bool tex, bool blend, uint32_t zlo, uint32_t zhi)
{
    rng_state = seed;
    g.ad(PRIM, 6 | (tex ? 1u << 4 : 0) | (blend ? 1u << 6 : 0) | (1u << 8));
    for (int i = 0; i < n; i++)
    {
        int w = 8 + int(rnd_to(90)), h = 8 + int(rnd_to(70));
        int x = int(rnd_to(FB_W - w)), y = int(rnd_to(FB_H - h));
        uint32_t rgba = rnd() & 0xFFFFFF;
        rgba |= (blend ? 0x30u + rnd_to(0x50) : 0x80u) << 24;
        uint32_t z = zlo + rnd_to(zhi - zlo);
        int u = int(rnd_to(60)), v = int(rnd_to(60));
        sprite(g, x, y, x + w, y + h, z, rgba, tex, u, v, u + 4 + int(rnd_to(60)), v + 4 + int(rnd_to(60)));
    }
}

static void clear(Gif &g, uint32_t rgba)
{
    g.ad(TEST_1, 0);                                   /* no tests: the Z buffer takes the clear's Z too */
    g.ad(PRIM, 6 | (1u << 8));
    sprite(g, 0, 0, FB_W, FB_H, 0, rgba, false, 0, 0, 0, 0);
    g.ad(TEST_1, (1ull << 16) | (2ull << 17));
}

/* ---- the display and the pictures */

static void display()
{
    mh2gs_priv_write(0x12000000, 0xFF25);                                /* PMODE: circuit 1, alpha from ALP */
    mh2gs_priv_write(0x12000010, (32ull << 3) | (2ull << 13));           /* SMODE1: NTSC, analogue */
    mh2gs_priv_write(0x12000020, 0);                                     /* SMODE2: progressive */
    mh2gs_priv_write(0x12000070, 0ull | (10ull << 9) | (0ull << 15));    /* DISPFB1: base 0, 640 wide, CT32 */
    mh2gs_priv_write(0x12000080, 636ull | (50ull << 12) | (3ull << 23) | (0ull << 27) | (2559ull << 32) | (447ull << 44));
    mh2gs_priv_write(0x120000E0, 0);
}

struct Picture
{
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
};

static bool picture(Picture &p)
{
    uint64_t id = mh2gs_vsync(0, 1);
    mh2gs_frame f;
    memset(&f, 0, sizeof(f));
    if (!id || !mh2gs_get_frame(id, &f))
        return false;
    p.w = f.width;
    p.h = f.height;
    p.px.resize(size_t(f.stride) * f.height);
    memcpy(p.px.data(), f.pixels, p.px.size());
    return true;
}

static bool same(const char *what, const Picture &a, const Picture &b)
{
    if (a.w != b.w || a.h != b.h || a.px.size() != b.px.size())
    {
        printf("FAIL %s: sizes %ux%u and %ux%u\n", what, a.w, a.h, b.w, b.h);
        failures++;
        return false;
    }
    size_t diff = 0, first = 0;
    for (size_t i = 0; i < a.px.size(); i += 4)
        if (memcmp(&a.px[i], &b.px[i], 4))
        {
            if (!diff)
                first = i / 4;
            diff++;
        }
    if (diff)
    {
        printf("FAIL %s: %zu of %zu pixels differ, the first at (%zu,%zu)\n", what, diff, a.px.size() / 4, first % a.w,
               first / a.w);
        failures++;
        return false;
    }
    return true;
}

static uint64_t hash_picture(const Picture &p)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : p.px)
        h = (h ^ b) * 1099511628211ull;
    return h;
}

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

/* ---- the scenes */

/* The frames the game's state depends on (before the save). */
static void frame_1(Gif &g)
{
    setup(g);
    clear(g, 0xFF402010);
    upload_texture(g, TEX_A, 11);
    upload_clut(g, CLUT_A, 21);
    g.ad(TEX0_1, tex0(TEX_A, 2, 0x13, 7, 7, CLUT_A, 1));
    sprites(g, 101, 120, true, false, 0x1000, 0x80000);
}

static void frame_2(Gif &g)
{
    upload_texture(g, TEX_B, 12);
    upload_clut(g, CLUT_B, 22);
    g.ad(TEX0_1, tex0(TEX_B, 2, 0x13, 7, 7, CLUT_B, 1));    /* the CLUT now loaded is B's */
    sprites(g, 102, 90, true, true, 0x1000, 0x90000);
    sprites(g, 103, 40, false, true, 0x1000, 0x90000);
    upload_clut(g, CLUT_B, 23);                             /* B's memory is overwritten, its cached copy is not */
}

/* After the save: draws with the CLUT the cache holds (CLD 0), on the frame buffer and Z buffer as they are. */
static void frame_3(Gif &g)
{
    g.ad(TEX0_1, tex0(TEX_A, 2, 0x13, 7, 7, CLUT_B, 0));    /* a texture of A's memory, B's palette, not loaded */
    sprites(g, 104, 100, true, true, 0x1000, 0x98000);
    sprites(g, 105, 30, false, true, 0x8000, 0x98000);
    g.ad(TEX0_1, tex0(TEX_A, 2, 0x13, 7, 7, CLUT_A, 1));    /* and a load of A's, then drawn opaque */
    sprites(g, 109, 100, true, false, 0x1000, 0x98000);
}

static void frame_4(Gif &g)
{
    sprites(g, 106, 60, true, true, 0x1000, 0xA0000);
    upload_texture(g, TEX_B, 13);
    g.ad(TEX0_1, tex0(TEX_B, 2, 0x13, 7, 7, CLUT_A, 0));
    sprites(g, 107, 60, true, false, 0x1000, 0xA0000);
}

/* Something else: other registers, other memory, another palette loaded, a different frame buffer address. */
static void junk(Gif &g)
{
    g.ad(FRAME_1, frame_reg(0, 10, 0) | (0xFFull << 32));   /* a mask on the colour */
    g.ad(ZBUF_1, ZBP | (1ull << 32));                       /* Z writes off */
    g.ad(XYOFFSET_1, uint64_t(1500 << 4) | (uint64_t(1500 << 4) << 32));
    g.ad(SCISSOR_1, 40ull | (uint64_t(300) << 16) | (60ull << 32) | (uint64_t(200) << 48));
    g.ad(TEST_1, 1ull | (6ull << 1) | (0x40ull << 4) | (1ull << 16) | (3ull << 17));
    g.ad(ALPHA_1, 2 | (2u << 2) | (2u << 4) | (2u << 6) | (0x40ull << 32));
    g.ad(TEXA, 0x10 | (0x20ull << 32));
    upload_texture(g, TEX_A, 31);
    upload_texture(g, TEX_B, 32);
    upload_clut(g, CLUT_A, 33);
    upload_clut(g, CLUT_B, 34);
    std::vector<uint32_t> noise(64 * 64);
    rng_state = 35;
    for (auto &v : noise)
        v = rnd() * 2654435761u;
    upload(g, 5000, 1, 0, 64, 64, noise.data(), noise.size() * 4);   /* over the frame buffer */
    upload(g, 6000, 1, 0, 64, 64, noise.data(), noise.size() * 4);   /* over the Z buffer */
    g.ad(TEX0_1, tex0(TEX_B, 2, 0x13, 7, 7, CLUT_A, 1));
    sprites(g, 108, 80, true, true, 0, 0x10000);
    g.ad(FRAME_1, frame_reg(0, 10, 0));
    g.ad(ZBUF_1, ZBP);
}

static bool display_ok(const Picture &p) { return p.w >= 320 && p.h >= 224; }

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mh2gs_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = MH2GS_ABI_VERSION;
    cfg.super_sampling = 1;
    int r = mh2gs_init(&cfg);
    if (r != 0)
    {
        printf("SKIP: mh2gs_init returned %d (no Vulkan GPU for paraLLEl-GS)\n", r);
        return 77;
    }
    size_t cap = mh2gs_state_size();
    printf("state size %zu bytes\n", cap);
    CHECK(cap > 4u * 1024 * 1024);
    std::vector<uint8_t> a(cap), b(cap), c(cap);

    display();
    Picture p0, p3, p4, q3, q4;
    {
        Gif g;
        frame_1(g);
        g.send();
    }
    CHECK(picture(p0));
    CHECK(display_ok(p0));
    {
        Gif g;
        frame_2(g);
        g.send();
    }
    Picture p2;
    CHECK(picture(p2));
    CHECK(p0.px != p2.px);

    /* errors that need no state */
    CHECK(mh2gs_state_get(a.data(), cap - 1) == MH2GS_STATE_TOO_SMALL);
    CHECK(mh2gs_state_set(a.data(), 100) == MH2GS_STATE_BAD);

    /* the save, and its time */
    auto t0 = Clock::now();
    int64_t n = mh2gs_state_get(a.data(), cap);
    double save_ms = ms_since(t0);
    CHECK(n == int64_t(cap));
    printf("save %.1f ms\n", save_ms);

    /* the uninterrupted timeline: frames 3 and 4 */
    {
        Gif g;
        frame_3(g);
        g.send();
    }
    CHECK(picture(p3));
    {
        Gif g;
        frame_4(g);
        g.send();
    }
    CHECK(picture(p4));
    CHECK(p3.px != p2.px);

    /* something else, then the restore */
    {
        Gif g;
        junk(g);
        g.send();
    }
    Picture pj;
    CHECK(picture(pj));
    CHECK(pj.px != p3.px);
    t0 = Clock::now();
    int sr = mh2gs_state_set(a.data(), cap);
    double load_ms = ms_since(t0);
    CHECK(sr == 0);
    printf("restore %.1f ms\n", load_ms);

    /* the block from the restored renderer is the block that went in */
    CHECK(mh2gs_state_get(b.data(), cap) == int64_t(cap));
    if (a != b)
    {
        size_t i = 0;
        while (a[i] == b[i])
            i++;
        printf("FAIL: the block saved after the restore differs from the one restored (first byte %zu%s)\n", i,
               i < 16384 ? ", in the registers and small sections" : ", in local memory");
        failures++;
    }

    /* the pictures that follow are the uninterrupted timeline's */
    {
        Gif g;
        frame_3(g);
        g.send();
    }
    CHECK(picture(q3));
    same("frame 3 after the restore", p3, q3);
    {
        Gif g;
        frame_4(g);
        g.send();
    }
    CHECK(picture(q4));
    same("frame 4 after the restore", p4, q4);
    printf("pictures %ux%u, hashes %016llx %016llx\n", p3.w, p3.h, (unsigned long long)hash_picture(p3),
           (unsigned long long)hash_picture(p4));

    /* a second restore, from a block made later in the timeline, into a renderer that has drawn since */
    CHECK(mh2gs_state_get(c.data(), cap) == int64_t(cap));
    Picture x, y;
    {
        Gif g;
        frame_3(g);
        frame_4(g);
        frame_3(g);
        g.send();
    }
    CHECK(picture(x));
    {
        Gif g;
        junk(g);
        g.send();
    }
    CHECK(picture(pj));
    CHECK(mh2gs_state_set(c.data(), cap) == 0);
    {
        Gif g;
        frame_3(g);
        frame_4(g);
        frame_3(g);
        g.send();
    }
    CHECK(picture(y));
    same("a later block", x, y);

    /* a refused block changes nothing */
    std::vector<uint8_t> bad = a;
    bad[20000] ^= 1;
    CHECK(mh2gs_state_set(bad.data(), cap) == MH2GS_STATE_BAD);
    bad = a;
    bad[0] ^= 1;
    CHECK(mh2gs_state_set(bad.data(), cap) == MH2GS_STATE_BAD);
    CHECK(mh2gs_state_set(a.data(), cap - 16) == MH2GS_STATE_BAD);

    mh2gs_shutdown();
    CHECK(mh2gs_state_size() == 0);
    printf("%s: %d failures\n", failures ? "FAILED" : "mh2gs_state_test OK", failures);
    return failures ? 1 : 0;
}
