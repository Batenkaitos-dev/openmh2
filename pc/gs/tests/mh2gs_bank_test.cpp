/* The renderer's graphics memory above 4 MB, without the core and without the game: block pointers of 16384 or more
 * (the bank in TEX1 and BITBLTBUF's 16-bit pointers; patch 0006 of paraLLEl-GS), drawn by the renderer at 8 MB from
 * GIF streams made here. Needs a Vulkan GPU (it exits 77, "skipped", where mh2gs finds none). Part of the port (CC0 1.0).
 *
 *   mh2gs_bank_test [control | noreload]
 *
 * Five scenes, each a block pointer that differs from another only above bit 13:
 *   tex      two pictures uploaded to blocks 0x2400 and 0x6400, drawn one after the other (and the other way round): two
 *            pictures. A texture cache keyed without the bank draws one of them twice.
 *   keep     a TEX1 write that changes the sampling only (bit 56 set) between binding a block at 0x6400 and drawing: the bank
 *            stays. The control writes it without the bit, as a renderer that did not know it would see the game's stream.
 *   clut     two palettes at 0x2800 and 0x6800, one picture drawn with each, loaded by CLD 2 and compared by CLD 4: the
 *            compare must see two pointers. A cached pointer of 14 bits keeps the first palette.
 *   mip      a picture at block 0x3FE0 whose second level is at 0x4000, past the bank boundary: the level pointer is read
 *            as a distance from the base (a mip level has no bank of its own).
 *   download the data of two uploads to 0x3000 and 0x7000 read back from each.
 *   order    a palette's TEX0 written before the TEX1 that names its bank (a writer that sets the sampling last): the
 *            palette is loaded again before the draw, from the block the two registers name together; the usual order
 *            beside it. With `noreload` (MH2GS_BANK_RELOAD=0: the palette stays the one TEX0 loaded) this scene must
 *            come out wrong and the five others right.
 * Without an argument every scene must come out right. With `control` the stream is made as a renderer that ignores the
 * bank would see it (TEX1's bank bits zero, the downloads' pointers 14 bits): the scenes tex, keep, clut and download must then
 * come out wrong, which shows that the test can fail. The mip scene has no such stream (the level pointer's 14 bits are the
 * same in both readings): it is skipped in the control, and checks that the colour it expects is not the one that block 0
 * (the frame buffer, where a reading without the carry would look) holds. Exit status 0 when every scene came out as it must. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../mh2gs.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- GIF packets (as mh2gs_state_test.cpp) */

enum { PRIM = 0x00, RGBAQ = 0x01, UV = 0x03, XYZ2 = 0x05, TEX0_1 = 0x06, TEX1_1 = 0x14, XYOFFSET_1 = 0x18,
       PRMODECONT = 0x1A, TEXA = 0x3B, SCISSOR_1 = 0x40, ALPHA_1 = 0x42, COLCLAMP = 0x46, TEST_1 = 0x47,
       FRAME_1 = 0x4C, ZBUF_1 = 0x4E, BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52, TRXDIR = 0x53,
       TEXFLUSH = 0x3F, MIPTBP1_1 = 0x34 };

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

static bool control = false;
static bool noreload = false;   /* `noreload`: the renderer with MH2GS_BANK_RELOAD=0, the control of the scene order */
static bool dump_ppm = false;   /* `dump`: each scene's first picture as NAME.ppm in the working folder */

static uint64_t frame_reg(uint32_t fbp, uint32_t fbw, uint32_t psm) { return fbp | (uint64_t(fbw) << 16) | (uint64_t(psm) << 24); }

/* TEX0 with the low 14 bits of the pointers: what the game writes. */
static uint64_t tex0(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cbp, uint32_t cld)
{
    return (tbp & 0x3FFF) | (uint64_t(tbw) << 14) | (uint64_t(psm) << 20) | (uint64_t(tw) << 26) | (uint64_t(th) << 30) |
           (1ull << 34) | (uint64_t(cbp & 0x3FFF) << 37) | (uint64_t(cld) << 61);
}

/* TEX1: nearest filtering, and the high bits of the two pointers in bits 48-51 and 52-55. A renderer that ignored the bank
 * would see them zero (the control). */
static uint64_t tex1(uint32_t tbp, uint32_t cbp, uint64_t lod_bits = 0)
{
    uint32_t tb = control ? 0 : (tbp >> 14), cb = control ? 0 : (cbp >> 14);
    return lod_bits | (uint64_t(tb) << 48) | (uint64_t(cb) << 52);
}

static const uint32_t FB_W = 640, FB_H = 448, OFS = 1728, OFSY = 1824;   /* the offsets that put (0,0) at the corner */
static const uint32_t ZBP = 140;

static void upload(Gif &g, uint32_t dbp, uint32_t dbw, uint32_t psm, uint32_t w, uint32_t h, const void *data, size_t bytes)
{
    /* DBP is 16 bits in BITBLTBUF: a block at 16384 or more is in the stream whole. The control cannot strip it: a renderer
     * that read 14 bits would see the same bits as this one writes (the game's uploads need no bank). */
    g.ad(BITBLTBUF, (uint64_t(dbp & 0xFFFF) << 32) | (uint64_t(dbw) << 48) | (uint64_t(psm) << 56));
    g.ad(TRXPOS, 0);
    g.ad(TRXREG, w | (uint64_t(h) << 32));
    g.ad(TRXDIR, 0);
    g.image(data, bytes);
}

static void upload_solid32(Gif &g, uint32_t dbp, uint32_t w, uint32_t h, uint32_t abgr)
{
    std::vector<uint32_t> t(size_t(w) * h, abgr);
    upload(g, dbp, 1, 0, w, h, t.data(), t.size() * 4);
}

static void setup(Gif &g)
{
    g.ad(FRAME_1, frame_reg(0, 10, 0));
    g.ad(ZBUF_1, ZBP);
    g.ad(XYOFFSET_1, uint64_t(OFS << 4) | (uint64_t(OFSY << 4) << 32));
    g.ad(SCISSOR_1, 0ull | (uint64_t(FB_W - 1) << 16) | (0ull << 32) | (uint64_t(FB_H - 1) << 48));
    g.ad(TEST_1, (1ull << 16) | (2ull << 17));        /* Z test on, GEQUAL */
    g.ad(COLCLAMP, 1);
    g.ad(PRMODECONT, 1);
    g.ad(TEXA, 0x80 | (0x80ull << 32));
}

static void sprite(Gif &g, int x0, int y0, int x1, int y1, uint32_t z, uint32_t rgba, int u0, int v0, int u1, int v1)
{
    g.ad(RGBAQ, rgba | (0x3F800000ull << 32));
    g.ad(UV, uint64_t(u0 << 4) | (uint64_t(v0 << 4) << 16));
    g.ad(XYZ2, uint64_t((x0 + OFS) << 4) | (uint64_t((y0 + OFSY) << 4) << 16) | (uint64_t(z) << 32));
    g.ad(UV, uint64_t(u1 << 4) | (uint64_t(v1 << 4) << 16));
    g.ad(XYZ2, uint64_t((x1 + OFS) << 4) | (uint64_t((y1 + OFSY) << 4) << 16) | (uint64_t(z) << 32));
}

/* A 32 x 32 textured sprite at (x, y) with the texture the registers name; the colour does not change the texel. */
static void draw32(Gif &g, int x, int y)
{
    g.ad(PRIM, 6 | (1u << 4) | (1u << 8));
    sprite(g, x, y, x + 32, y + 32, 0x1000, 0x80808080, 0, 0, 32, 32);
}

static void clear(Gif &g, uint32_t rgba)
{
    g.ad(TEST_1, 0);
    g.ad(PRIM, 6 | (1u << 8));
    sprite(g, 0, 0, FB_W, FB_H, 0, rgba, 0, 0, 0, 0);
    g.ad(TEST_1, (1ull << 16) | (2ull << 17));
}

static void display()
{
    mh2gs_priv_write(0x12000000, 0xFF25);
    mh2gs_priv_write(0x12000010, (32ull << 3) | (2ull << 13));
    mh2gs_priv_write(0x12000020, 0);
    mh2gs_priv_write(0x12000070, 0ull | (10ull << 9) | (0ull << 15));
    mh2gs_priv_write(0x12000080, 636ull | (50ull << 12) | (3ull << 23) | (0ull << 27) | (2559ull << 32) | (447ull << 44));
    mh2gs_priv_write(0x120000E0, 0);
}

struct Picture
{
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0, stride = 0;
    /* the colour (R, G, B) at a point of the game's 640 x 448 screen, as 0xRRGGBB */
    size_t not_clear(uint32_t *x0 = nullptr, uint32_t *y0 = nullptr, uint32_t *x1 = nullptr, uint32_t *y1 = nullptr) const
    {
        size_t n = 0;
        uint32_t ax = w, ay = h, bx = 0, by = 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const uint8_t *p = &px[size_t(y) * stride + size_t(x) * 4];
                if (((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]) != 0x102030)
                {
                    n++;
                    ax = x < ax ? x : ax;
                    ay = y < ay ? y : ay;
                    bx = x > bx ? x : bx;
                    by = y > by ? y : by;
                }
            }
        if (x0) { *x0 = ax; *y0 = ay; *x1 = bx; *y1 = by; }
        return n;
    }
    /* the colour (R, G, B) at a point of the game's screen as 0xRRGGBB: the picture is a field of the display, whose top is
     * TOP rows below the picture's (DISPLAY's DY of 50, halved) */
    uint32_t at(int x, int y) const
    {
        static const int TOP = 25;
        const uint8_t *p = &px[size_t(y + TOP) * stride + size_t(x) * 4];
        return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }
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
    p.stride = f.stride;
    p.px.resize(size_t(f.stride) * f.height);
    memcpy(p.px.data(), f.pixels, p.px.size());
    return true;
}

static void dump(const Picture &p, const char *name)
{
    if (!dump_ppm)
        return;
    char fn[64];
    snprintf(fn, sizeof(fn), "%s.ppm", name);
    FILE *f = fopen(fn, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", p.w, p.h);
    for (uint32_t y = 0; y < p.h; y++)
        for (uint32_t x = 0; x < p.w; x++)
            fwrite(&p.px[size_t(y) * p.stride + size_t(x) * 4], 1, 3, f);
    fclose(f);
}

static const uint32_t RED = 0xFF0000, GREEN = 0x00FF00, BLUE = 0x0000FF, YELLOW = 0xFFFF00, CYAN = 0x00FFFF, MAGENTA = 0xFF00FF;
static const uint32_t CLEAR = 0x102030;   /* what the frame buffer holds (and block 0 with it) */

/* the 8-bit-per-channel colours as texels: A 0x80, then B, G, R */
static uint32_t texel(uint32_t rgb) { return 0x80000000u | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF); }

/* Every check of a scene is one line: what was expected, what was drawn. */
struct Scene
{
    const char *name;
    int bad = 0, total = 0;
    explicit Scene(const char *n) : name(n) {}
    void expect(const char *what, uint32_t want, uint32_t got)
    {
        total++;
        if (want != got)
        {
            bad++;
            printf("  %s: %s: expected %06X, drawn %06X\n", name, what, want, got);
        }
    }
};

/* The scene's verdict: in the normal run every check must hold, in the control at least one must fail. */
static void verdict(const Scene &s)
{
    bool ok = control ? s.bad > 0 : s.bad == 0;
    printf("%-9s %s (%d of %d checks %s)\n", s.name, ok ? "PASS" : "FAIL", control ? s.bad : s.total - s.bad, s.total,
           control ? "came out wrong, as the control must" : "right");
    if (!ok)
        failures++;
}

static void scene_tex()
{
    Scene s("tex");
    Gif g;
    setup(g);
    clear(g, texel(CLEAR));
    upload_solid32(g, 0x2400, 32, 32, texel(RED));
    upload_solid32(g, 0x6400, 32, 32, texel(BLUE));
    g.ad(TEXFLUSH, 0);
    const uint32_t lo = 0x2400, hi = 0x6400;
    g.ad(TEX1_1, tex1(lo, 0));
    g.ad(TEX0_1, tex0(lo, 1, 0, 5, 5, 0, 0));
    draw32(g, 50, 20);
    g.ad(TEX1_1, tex1(hi, 0));
    g.ad(TEX0_1, tex0(hi, 1, 0, 5, 5, 0, 0));   /* the same TEX0 as the first draw's: only the bank differs */
    draw32(g, 150, 20);
    g.ad(TEX1_1, tex1(lo, 0));
    g.ad(TEX0_1, tex0(lo, 1, 0, 5, 5, 0, 0));
    draw32(g, 250, 20);
    g.send();
    Picture p;
    CHECK(picture(p));
    dump(p, "tex");
    uint32_t bx0, by0, bx1, by1;
    size_t nc = p.not_clear(&bx0, &by0, &bx1, &by1);
    printf("  picture %ux%u, %zu pixels are not the clear colour, within (%u,%u)-(%u,%u); at the first sprite %06X\n", p.w, p.h,
           nc, bx0, by0, bx1, by1, p.at(66, 36));
    s.expect("low block, first", RED, p.at(66, 36));
    s.expect("high block", BLUE, p.at(166, 36));
    s.expect("low block again", RED, p.at(266, 36));
    /* the other way round, in another picture: the cached images are in place now */
    Gif h;
    h.ad(TEX1_1, tex1(hi, 0));
    h.ad(TEX0_1, tex0(hi, 1, 0, 5, 5, 0, 0));
    draw32(h, 50, 20);
    h.ad(TEX1_1, tex1(lo, 0));
    h.ad(TEX0_1, tex0(lo, 1, 0, 5, 5, 0, 0));
    draw32(h, 150, 20);
    h.ad(TEX1_1, tex1(hi, 0));
    h.ad(TEX0_1, tex0(hi, 1, 0, 5, 5, 0, 0));
    draw32(h, 250, 20);
    h.send();
    CHECK(picture(p));
    s.expect("high block, second picture", BLUE, p.at(66, 36));
    s.expect("low block, second picture", RED, p.at(166, 36));
    s.expect("high block again", BLUE, p.at(266, 36));
    verdict(s);
}

/* A TEX1 write that changes the sampling only (the game's filter change, between binding a texture and drawing it) carries bit 56
 * and keeps the bank of the TEX1 it replaces. The control writes it without the bit, as a stream would for a renderer that knew
 * nothing of it: the write clears the bank and the draw reads the low block. */
static void scene_keep()
{
    Scene s("keep");
    Gif g;
    setup(g);
    clear(g, texel(CLEAR));
    upload_solid32(g, 0x2400, 32, 32, texel(RED));
    upload_solid32(g, 0x6400, 32, 32, texel(BLUE));
    g.ad(TEXFLUSH, 0);
    const uint32_t lo = 0x2400, hi = 0x6400;
    const uint64_t linear = 0x20 | (1u << 6), keep = control ? 0 : (1ull << 56);
    auto bind = [&](uint32_t tbp) {
        g.ad(TEX1_1, (uint64_t(tbp >> 14) << 48) | linear);
        g.ad(TEX0_1, tex0(tbp, 1, 0, 5, 5, 0, 0));
    };
    bind(hi);
    g.ad(TEX1_1, linear | keep);
    draw32(g, 50, 20);
    bind(lo);
    g.ad(TEX1_1, linear | keep);
    draw32(g, 150, 20);
    bind(hi);
    g.ad(TEX1_1, 0 | keep);
    draw32(g, 250, 20);
    g.send();
    Picture p;
    CHECK(picture(p));
    dump(p, "keep");
    s.expect("high block after a filter write", BLUE, p.at(66, 36));
    s.expect("low block after a filter write", RED, p.at(166, 36));
    s.expect("high block, nearest filter write", BLUE, p.at(266, 36));
    verdict(s);
}

static void scene_clut()
{
    Scene s("clut");
    Gif g;
    setup(g);
    clear(g, texel(CLEAR));
    std::vector<uint8_t> idx(32 * 32);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            idx[size_t(y) * 32 + x] = x < 16 ? 1 : 2;
    upload(g, 0x2C00, 1, 0x13, 32, 32, idx.data(), idx.size());
    std::vector<uint32_t> p0(256, texel(0)), p1(256, texel(0));
    p0[1] = texel(GREEN);
    p0[2] = texel(YELLOW);
    p1[1] = texel(CYAN);
    p1[2] = texel(MAGENTA);
    upload(g, 0x2800, 1, 0, 16, 16, p0.data(), p0.size() * 4);
    upload(g, 0x6800, 1, 0, 16, 16, p1.data(), p1.size() * 4);
    g.ad(TEXFLUSH, 0);
    /* a load that writes the cached pointer CBP0 (CLD 2) of the first palette; a compare (CLD 4) with the second's pointer,
     * which has the same low 14 bits and so must load; a compare with the first's again, which must load again */
    g.ad(TEX1_1, tex1(0x2C00, 0x2800));
    g.ad(TEX0_1, tex0(0x2C00, 1, 0x13, 5, 5, 0x2800, 2));
    draw32(g, 50, 50);
    g.ad(TEX1_1, tex1(0x2C00, 0x6800));
    g.ad(TEX0_1, tex0(0x2C00, 1, 0x13, 5, 5, 0x6800, 4));
    draw32(g, 150, 50);
    g.ad(TEX1_1, tex1(0x2C00, 0x2800));
    g.ad(TEX0_1, tex0(0x2C00, 1, 0x13, 5, 5, 0x2800, 4));
    draw32(g, 250, 50);
    g.send();
    Picture p;
    CHECK(picture(p));
    s.expect("first palette, index 1", GREEN, p.at(58, 66));
    s.expect("first palette, index 2", YELLOW, p.at(74, 66));
    s.expect("second palette, index 1", CYAN, p.at(158, 66));
    s.expect("second palette, index 2", MAGENTA, p.at(174, 66));
    s.expect("first palette again, index 1", GREEN, p.at(258, 66));
    s.expect("first palette again, index 2", YELLOW, p.at(274, 66));
    verdict(s);
}

static void scene_mip()
{
    Scene s("mip");
    if (control)
    {
        printf("%-9s skipped in the control (see the top of the file)\n", s.name);
        return;
    }
    Gif g;
    setup(g);
    clear(g, texel(CLEAR));
    upload_solid32(g, 0x3FE0, 32, 32, texel(RED));     /* level 0: the last page below the boundary */
    upload_solid32(g, 0x4000, 16, 16, texel(BLUE));    /* level 1: the first page above it */
    g.ad(TEXFLUSH, 0);
    /* LCM 1 (the LOD is K), MXL 1, MMIN nearest mipmap nearest, K 1.0 (16 in 1/16): level 1 */
    const uint64_t lod1 = 1ull | (1ull << 2) | (2ull << 6) | (16ull << 32);
    g.ad(TEX1_1, tex1(0x3FE0, 0, lod1));
    g.ad(TEX0_1, tex0(0x3FE0, 1, 0, 5, 5, 0, 0));
    g.ad(MIPTBP1_1, (0x4000 & 0x3FFF) | (1ull << 14));   /* TBP1's 14 bits: 0, below the base's: the carry */
    draw32(g, 50, 20);
    /* K 0: level 0 of the same picture */
    g.ad(TEX1_1, tex1(0x3FE0, 0, 1ull | (1ull << 2) | (2ull << 6)));
    draw32(g, 150, 20);
    g.send();
    Picture p;
    CHECK(picture(p));
    s.expect("level 1, past the boundary", BLUE, p.at(66, 36));
    s.expect("level 0", RED, p.at(166, 36));
    s.expect("the colour a reading without the carry would give is not blue", 1, CLEAR != BLUE);
    verdict(s);
}

static void scene_download()
{
    Scene s("download");
    Gif g;
    setup(g);
    std::vector<uint32_t> a(32 * 32), b(32 * 32);
    for (size_t i = 0; i < a.size(); i++)
    {
        a[i] = 0x01000193u * uint32_t(i + 1) ^ 0xA5A5A5A5u;
        b[i] = 0x9E3779B1u * uint32_t(i + 7) ^ 0x5A5A5A5Au;
    }
    upload(g, 0x3000, 1, 0, 32, 32, a.data(), a.size() * 4);
    upload(g, 0x7000, 1, 0, 32, 32, b.data(), b.size() * 4);
    g.send();
    for (int which = 0; which < 2; which++)
    {
        uint32_t sbp = which ? 0x7000 : 0x3000;
        if (control)
            sbp &= 0x3FFF;   /* a renderer that read 14 bits */
        Gif d;
        d.ad(BITBLTBUF, uint64_t(sbp) | (1ull << 16) | (0ull << 24));
        d.ad(TRXPOS, 0);
        d.ad(TRXREG, 32 | (32ull << 32));
        d.ad(TRXDIR, 1);
        d.send();
        std::vector<uint32_t> got(32 * 32, 0);
        mh2gs_read_fifo(got.data(), 32 * 32 * 4 / 16);
        const std::vector<uint32_t> &want = which ? b : a;
        s.expect(which ? "block 0x7000" : "block 0x3000", 1, got == want);
    }
    verdict(s);
}

/* TEX0 before the TEX1 that names the palette's bank: the palette is loaded again before the draw. */
static void scene_order()
{
    Scene s("order");
    Gif g;
    setup(g);
    clear(g, texel(CLEAR));
    std::vector<uint8_t> idx(32 * 32);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            idx[size_t(y) * 32 + x] = x < 16 ? 1 : 2;
    upload(g, 0x2C00, 1, 0x13, 32, 32, idx.data(), idx.size());
    std::vector<uint32_t> p0(256, texel(0)), p1(256, texel(0));
    p0[1] = texel(GREEN);
    p0[2] = texel(YELLOW);
    p1[1] = texel(CYAN);
    p1[2] = texel(MAGENTA);
    upload(g, 0x2800, 1, 0, 16, 16, p0.data(), p0.size() * 4);
    upload(g, 0x6800, 1, 0, 16, 16, p1.data(), p1.size() * 4);
    g.ad(TEXFLUSH, 0);
    /* the usual order, the low palette */
    g.ad(TEX1_1, tex1(0x2C00, 0x2800));
    g.ad(TEX0_1, tex0(0x2C00, 1, 0x13, 5, 5, 0x2800, 1));
    draw32(g, 50, 50);
    /* TEX0 first (it loads from the bank TEX1 still names, the low one), then the TEX1 that names the high palette's bank */
    g.ad(TEX0_1, tex0(0x2C00, 1, 0x13, 5, 5, 0x6800, 1));
    g.ad(TEX1_1, tex1(0x2C00, 0x6800));
    draw32(g, 150, 50);
    /* and back the same way: TEX0 first (it loads the high palette), then the TEX1 that names the low bank */
    g.ad(TEX0_1, tex0(0x2C00, 1, 0x13, 5, 5, 0x2800, 1));
    g.ad(TEX1_1, tex1(0x2C00, 0x2800));
    draw32(g, 250, 50);
    /* the usual order after it, the high palette */
    g.ad(TEX1_1, tex1(0x2C00, 0x6800));
    g.ad(TEX0_1, tex0(0x2C00, 1, 0x13, 5, 5, 0x6800, 1));
    draw32(g, 350, 50);
    g.send();
    Picture p;
    CHECK(picture(p));
    if (dump_ppm)
        dump(p, "order");
    s.expect("low palette, the usual order, index 1", GREEN, p.at(58, 66));
    s.expect("low palette, the usual order, index 2", YELLOW, p.at(74, 66));
    s.expect("high palette, TEX0 before TEX1, index 1", CYAN, p.at(158, 66));
    s.expect("high palette, TEX0 before TEX1, index 2", MAGENTA, p.at(174, 66));
    s.expect("low palette, TEX0 before TEX1, index 1", GREEN, p.at(258, 66));
    s.expect("low palette, TEX0 before TEX1, index 2", YELLOW, p.at(274, 66));
    s.expect("high palette, the usual order, index 1", CYAN, p.at(358, 66));
    s.expect("high palette, the usual order, index 2", MAGENTA, p.at(374, 66));
    if (noreload)   /* the two draws with TEX0 first must come out wrong, the two in the usual order right */
    {
        const bool ok = s.bad == 4 && p.at(58, 66) == GREEN && p.at(358, 66) == CYAN;
        printf("%-9s %s (%d of %d checks came out wrong; the control must have 4)\n", s.name, ok ? "PASS" : "FAIL", s.bad, s.total);
        if (!ok)
            failures++;
    }
    else
        verdict(s);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        control |= !strcmp(argv[i], "control");
        noreload |= !strcmp(argv[i], "noreload");
        dump_ppm |= !strcmp(argv[i], "dump");
    }
    if (noreload)
    {
        static char env[] = "MH2GS_BANK_RELOAD=0";
        putenv(env);
    }
    mh2gs_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = MH2GS_ABI_VERSION;
    cfg.super_sampling = 1;
    cfg.vram_mb = 8;
    int r = mh2gs_init(&cfg);
    if (r != 0)
    {
        printf("SKIP: mh2gs_init returned %d (no Vulkan GPU for paraLLEl-GS)\n", r);
        return 77;
    }
    printf("graphics memory 8 MB%s\n", control ? ", the control (the bank ignored)" : noreload ? ", the control of the scene order (MH2GS_BANK_RELOAD=0)" : "");
    display();
    scene_tex();
    scene_keep();
    scene_clut();
    scene_mip();
    scene_download();
    scene_order();
    mh2gs_shutdown();
    printf(failures ? "mh2gs_bank_test: FAILED (%d)\n" : "mh2gs_bank_test: OK\n", failures);
    return failures ? 1 : 0;
}
