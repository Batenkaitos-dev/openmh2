/* mh2interp_test: the stream parser, the matcher and the substitution of mh2interp.cpp, on streams built here
 * (no GPU). Part of the port (CC0 1.0). Exit 0: all passed. */
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "mh2interp.hpp"

using namespace Mh2Interp;

static int failures = 0;
#define CHECK(c)                                                    \
    do                                                              \
    {                                                               \
        if (!(c))                                                   \
        {                                                           \
            printf("FAILED line %d: %s\n", __LINE__, #c);           \
            failures++;                                             \
        }                                                           \
    } while (0)

struct Stream
{
    std::vector<uint8_t> b;
    void q(uint64_t lo, uint64_t hi)
    {
        uint8_t t[16];
        memcpy(t, &lo, 8);
        memcpy(t + 8, &hi, 8);
        b.insert(b.end(), t, t + 16);
    }
    void q4(uint32_t a, uint32_t c, uint32_t d, uint32_t e)
    {
        uint32_t w[4] = {a, c, d, e};
        uint8_t t[16];
        memcpy(t, w, 16);
        b.insert(b.end(), t, t + 16);
    }
};

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* An A+D write. */
static void ad(Stream &s, uint32_t reg, uint64_t v) { s.q(v, reg); }

/* A path 1 packet: a PRE tag with PRIM (a triangle strip, textured, CTXT 0), then n vertices as STQ, RGBAQ, XYZF2 (NREG 3). */
static void strip(Stream &s, int n, int x0, int y0, float q, int shift)
{
    uint64_t prim = 4 | 0x10;   /* triangle strip, TME */
    uint64_t tag = uint64_t(n) | (1ull << 15) | (1ull << 46) | (prim << 47) | (0ull << 58) | (3ull << 60);
    s.q(tag, 0x412ull);   /* regs: ST(2), RGBAQ(1), XYZF2(4) */
    for (int i = 0; i < n; i++)
    {
        s.q4(f2u(0.25f * i * q), f2u(0.5f * q), f2u(q), 0);
        s.q4(10 + i, 20, 30, 128);
        s.q4(uint32_t(x0 + 16 * i + shift), uint32_t(y0 + 16 * (i & 1) + shift), (1000u + i) << 4, 0);
    }
}

static void feed(Capture &c, Stream &s, Step &out, uint32_t path = 1)
{
    c.gif(path, s.b.data(), s.b.size());
    c.finish(out);
}

int main()
{
    Capture cap;
    Stream env;
    env.q(3ull | (1ull << 15) | (1ull << 60), 0xE);   /* an A+D tag: three writes */
    ad(env, 0x18, uint64_t(0x8000) << 0 | (uint64_t(0x8000) << 32));   /* XYOFFSET_1 */
    ad(env, 0x06, 0x1234);                                             /* TEX0_1 */
    ad(env, 0x47, 0x5);                                                /* TEST_1 */
    Stream s1, s2, s3;
    strip(s1, 6, 0x8100, 0x8100, 2.0f, 0);
    strip(s2, 6, 0x8100, 0x8100, 4.0f, 32);   /* every vertex 2 px right and down; Q doubled */
    strip(s3, 6, 0x9000, 0x9000, 4.0f, 0);    /* far away */
    Step a, b, c;
    cap.gif(3, env.b.data(), env.b.size());
    feed(cap, s1, a);
    feed(cap, s2, b);
    CHECK(a.p1_kicks == 1 && b.p1_kicks == 1);
    CHECK(a.kicks.size() == 1 && a.kicks[0].count == 6);
    CHECK(a.kicks[0].key == b.kicks[0].key);
    CHECK(a.verts[0].fmt == 4 && a.verts[0].adc == 0);
    CHECK(a.verts[1].x == 0x8110 && b.verts[1].x == 0x8130);
    CHECK(a.kicks[0].ofx == 0x8000);
    Index ia;
    ia.build(a);
    Params p;
    /* the live in-between of a and b */
    Result r = interpolate(b, a, ia, b, ia, p);
    CHECK(!r.cut && r.interpolated == 1 && r.v_interpolated == 6);
    Capture cap2;
    Step chk;
    uint8_t *d = b.data.data();
    /* the data now holds the mean: x 0x8110 and 0x8130 -> 0x8120 */
    uint16_t x1;
    memcpy(&x1, d + b.verts[1].xyz_off, 2);
    CHECK(x1 == 0x8120);
    uint16_t y0;
    memcpy(&y0, d + b.verts[0].xyz_off + 4, 2);
    CHECK(y0 == 0x8110);   /* 0x8100 and 0x8120 */
    float sv[3];
    memcpy(sv, d + b.verts[2].st_off, 12);
    /* vertex 2: a has s = 0.5*2/... check S/Q is averaged: a: s=0.25*2*2=1.0, q=2 -> 0.5; b: s=0.25*2*4=2.0, q=4 -> 0.5; mean 0.5 * q 3 */
    CHECK(fabsf(sv[2] - 3.0f) < 1e-6f && fabsf(sv[0] - 1.5f) < 1e-5f);
    CHECK(d[b.verts[0].rgb_off] == 10);
    /* parsing the changed data gives the substituted vertices */
    Capture cap3;
    cap3.gif(3, env.b.data(), env.b.size());
    cap3.gif(1, b.data.data(), b.data.size());
    cap3.finish(chk);
    CHECK(chk.verts.size() == 6 && chk.verts[1].x == 0x8120);

    /* any alpha (1/256): the weighted mean, from one plan; 128 is the mean above, 0 is a, 256 is b */
    {
        Plan plan;
        CHECK(plan_interp(b, a, ia, b, ia, p, plan) && plan.res.interpolated == 1);
        const uint32_t al[5] = {0, 64, 128, 192, 256};
        const uint16_t want_x1[5] = {0x8110, 0x8118, 0x8120, 0x8128, 0x8130};
        for (int i = 0; i < 5; i++)
        {
            std::vector<uint8_t> dd = b.data;
            Result ra = apply_interp(b, dd.data(), a, b, plan, p, al[i]);
            uint16_t x;
            memcpy(&x, dd.data() + b.verts[1].xyz_off, 2);
            CHECK(!ra.cut && x == want_x1[i]);
        }
        std::vector<uint8_t> d128 = b.data, dm = b.data;
        apply_interp(b, d128.data(), a, b, plan, p, 128);
        Step bm = b;
        interpolate(bm, a, ia, bm, ia, p);
        CHECK(d128 == bm.data);   /* the weighted mean at 128 is the plain mean, bit for bit */
        (void)dm;
    }

    /* a far kick is gated, and a step of gated kicks is a cut */
    Capture cap4;
    cap4.gif(3, env.b.data(), env.b.size());
    Step a4, c4;
    feed(cap4, s1, a4);
    feed(cap4, s3, c4);
    Index i4;
    i4.build(a4);
    Result r4 = interpolate(c4, a4, i4, c4, i4, p);
    CHECK(r4.gated == 1 && r4.cut);

    /* an unpaired kick (another vertex count) is unmatched */
    Stream s5;
    strip(s5, 4, 0x8100, 0x8100, 2.0f, 0);
    Step c5;
    feed(cap4, s5, c5);
    Result r5 = interpolate(c5, a4, i4, c5, i4, p);
    CHECK(r5.unmatched == 1 && r5.cut);

    /* tags split across calls: the vertices come out the same */
    Capture cap6;
    cap6.gif(3, env.b.data(), env.b.size());
    for (size_t i = 0; i < s1.b.size(); i += 48)
        cap6.gif(1, s1.b.data() + i, std::min<size_t>(48, s1.b.size() - i));
    Step a6;
    cap6.finish(a6);
    CHECK(a6.verts.size() == 6 && a6.verts[5].x == a.verts[5].x && a6.kicks[0].key == a.kicks[0].key);

    /* an image transfer (path 3 IMAGE) is skipped, not parsed as tags */
    Capture cap7;
    Stream im;
    im.q(8ull | (2ull << 58), 0);
    for (int i = 0; i < 8; i++)
        im.q(0x0000000100000001ull, 0x1);   /* looks like a tag */
    Step a7;
    feed(cap7, im, a7, 3);
    CHECK(a7.kicks.empty());

    /* a privileged DISPFB write is a flip */
    Capture cap8;
    cap8.priv(0x12000090, 0x1234);
    Step a8;
    cap8.finish(a8);
    CHECK(a8.flip && a8.events.size() == 1);

    /* a vertex that two kicks share stays one vertex: kick A (30 vertices) is interpolated, kick B (4) is gated (it is far away in the step
     * before); B's first vertex is where A's last is, in the step. B is as the step has it, so A's last vertex keeps its place; with no
     * common position (abl 64) the two are apart, A's at the mean. A's others go to the mean. */
    {
        Capture cap9;
        cap9.gif(3, env.b.data(), env.b.size());
        Stream sa1, sa2, sb1, sb2;
        strip(sa1, 30, 0x8100, 0x8100, 2.0f, 0);
        strip(sb1, 4, 0x9000, 0x9000, 2.0f, 0);
        strip(sa2, 30, 0x8100, 0x8100, 4.0f, 32);
        strip(sb2, 4, 0x8100 + 16 * 29 + 32, 0x8100 + 16 + 32, 4.0f, 0);   /* its vertex 0 is A's vertex 29 */
        Step a9, b9;
        cap9.gif(1, sa1.b.data(), sa1.b.size());
        cap9.gif(1, sb1.b.data(), sb1.b.size());
        cap9.finish(a9);
        cap9.gif(1, sa2.b.data(), sa2.b.size());
        cap9.gif(1, sb2.b.data(), sb2.b.size());
        cap9.finish(b9);
        CHECK(a9.kicks.size() == 2 && b9.kicks.size() == 2 && b9.kicks[1].count == 4);
        Index i9;
        i9.build(a9);
        Step b9n = b9;
        const Step b9p = b9;   /* as captured */
        Params pn;
        pn.abl = 64;
        interpolate(b9n, a9, i9, b9n, i9, pn);
        Result r9 = interpolate(b9, a9, i9, b9, i9, p);
        CHECK(!r9.cut && r9.interpolated == 1 && r9.gated == 1);
        auto xy = [](const Step &st, uint32_t k, uint32_t j) {
            uint16_t y, x;
            memcpy(&y, st.data.data() + st.verts[st.kicks[k].first + j].xyz_off + 4, 2);
            memcpy(&x, st.data.data() + st.verts[st.kicks[k].first + j].xyz_off, 2);
            return uint32_t(y) | (uint32_t(x) << 16);
        };
        const uint32_t mean_y = 0x8100 + 16 + 16, step_y = 0x8100 + 16 + 32, x29 = 0x8100 + 16 * 29 + 16;
        CHECK(xy(b9, 0, 29) == xy(b9, 1, 0) && (xy(b9, 0, 29) & 0xFFFF) == step_y);   /* kept where the step has it */
        CHECK(xy(b9n, 0, 29) != xy(b9n, 1, 0) && xy(b9n, 0, 29) == (mean_y | (x29 << 16)));   /* no common position: apart */
        CHECK(xy(b9, 1, 1) == xy(b9n, 1, 1));   /* B's others are as the step has them */
        CHECK((xy(b9, 0, 2) & 0xFFFF) == 0x8100 + 16);   /* a vertex of A that is not shared: the mean (y 0x8100 and 0x8120 for an even vertex) */

        /* the same at other alphas: below 1/2 the gated kick B is as step a has it (xy, z) and A's vertex that B shares goes with it; from 1/2 on both are
         * as the step has them (alpha 128 is the interpolate() above) */
        Plan pl9;
        CHECK(plan_interp(b9p, a9, i9, b9p, i9, p, pl9));
        auto at = [&](uint32_t al) {
            std::vector<uint8_t> d = b9p.data;
            apply_interp(b9p, d.data(), a9, b9p, pl9, p, al);
            return d;
        };
        auto xyd = [&](const std::vector<uint8_t> &d, const Step &st, uint32_t k, uint32_t j) {
            uint16_t y, x;
            memcpy(&y, d.data() + st.verts[st.kicks[k].first + j].xyz_off + 4, 2);
            memcpy(&x, d.data() + st.verts[st.kicks[k].first + j].xyz_off, 2);
            return uint32_t(y) | (uint32_t(x) << 16);
        };
        std::vector<uint8_t> d64 = at(64), d127 = at(127), d128 = at(128), d200 = at(200);
        CHECK(d128 == b9.data);
        CHECK(xyd(d64, b9p, 1, 0) == xyd(a9.data, a9, 1, 0) && xyd(d127, b9p, 1, 1) == xyd(a9.data, a9, 1, 1));   /* B as step a has it */
        /* (B's place in step a is far from A's there: A's vertex keeps step n's place, or A would be stretched to it) */
        CHECK(xyd(d64, b9p, 0, 29) == xyd(b9p.data, b9p, 0, 29));
        CHECK(xyd(d64, b9p, 0, 2) != xyd(a9.data, a9, 0, 2) && xyd(d64, b9p, 0, 2) != xyd(b9p.data, b9p, 0, 2));   /* an unshared vertex of A: between */
        CHECK(xyd(d200, b9p, 1, 0) == xyd(b9p.data, b9p, 1, 0) && xyd(d200, b9p, 0, 29) == xyd(b9p.data, b9p, 0, 29));   /* from 1/2: step n */
        CHECK(at(0) != b9p.data);
    }

    /* the same where B was near A's vertex in step a (300 units, under the gate): A's vertex goes with B's below 1/2 */
    {
        Capture capc;
        capc.gif(3, env.b.data(), env.b.size());
        Stream sa1, sa2, sb1, sb2;
        strip(sa1, 30, 0x8100, 0x8100, 2.0f, 0);
        strip(sb1, 4, 0x8100 + 16 * 29 - 300, 0x8100 + 16 - 300, 2.0f, 0);
        strip(sa2, 30, 0x8100, 0x8100, 4.0f, 600);
        strip(sb2, 4, 0x8100 + 16 * 29 + 600, 0x8100 + 16 + 600, 4.0f, 0);
        Step ac, bc;
        capc.gif(1, sa1.b.data(), sa1.b.size());
        capc.gif(1, sb1.b.data(), sb1.b.size());
        capc.finish(ac);
        capc.gif(1, sa2.b.data(), sa2.b.size());
        capc.gif(1, sb2.b.data(), sb2.b.size());
        capc.finish(bc);
        Index ic;
        ic.build(ac);
        Plan plc;
        CHECK(plan_interp(bc, ac, ic, bc, ic, p, plc) && plc.res.interpolated == 1 && plc.res.gated == 1);
        auto atc = [&](uint32_t al, uint32_t k, uint32_t j) {
            std::vector<uint8_t> d = bc.data;
            apply_interp(bc, d.data(), ac, bc, plc, p, al);
            uint16_t y, x;
            memcpy(&y, d.data() + bc.verts[bc.kicks[k].first + j].xyz_off + 4, 2);
            memcpy(&x, d.data() + bc.verts[bc.kicks[k].first + j].xyz_off, 2);
            return uint32_t(y) | (uint32_t(x) << 16);
        };
        const uint32_t bax = 0x8100 + 16 * 29 - 300, bay = 0x8100 + 16 - 300;
        CHECK(atc(64, 1, 0) == (bay | (bax << 16)));   /* B's vertex: where step a had it */
        CHECK(atc(64, 0, 29) == atc(64, 1, 0));        /* A's shared vertex goes with it */
        CHECK(atc(200, 0, 29) == atc(200, 1, 0));      /* from 1/2 on, both at step n's */
        CHECK((atc(200, 1, 0) & 0xFFFF) == 0x8100 + 16 + 600);
    }

    printf(failures ? "mh2interp_test: %d FAILED\n" : "mh2interp_test: all passed\n", failures);
    return failures ? 1 : 0;
}
