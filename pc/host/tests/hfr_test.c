/* The timing of the frame rates above 60 (hfr.c): the alphas of a game frame, the clock, the divider and the step down. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../hfr.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("hfr_test: line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(int argc, char **argv)
{
    uint16_t a[HFR_MAX];
    double tau[HFR_MAX];

    /* the rate a setting gives, and the in-between pictures' ring: made at 60 and above, never at 30. "control": the same checks with the
     * ring asked of 30, which must fail (the test is registered WILL_FAIL) */
    if (argc > 1 && argv[1][0] == 'c') {
        CHECK(!hfr_pass_ring(hfr_rate_rule(60, 24)));
        CHECK(hfr_pass_ring(hfr_rate_rule(30, 24)));
        printf(failures ? "hfr_test control: %d FAILED\n" : "hfr_test control: all passed\n", failures);
        return failures ? 1 : 0;
    }
    CHECK(hfr_rate_rule(0, 24) == 30 && hfr_rate_rule(30, 24) == 30 && hfr_rate_rule(30, 2) == 30);
    CHECK(hfr_rate_rule(60, 24) == 60 && hfr_rate_rule(60, 2) == 60);
    CHECK(hfr_rate_rule(120, 8) == 120 && hfr_rate_rule(144, 8) == 144 && hfr_rate_rule(144, 24) == 144);
    CHECK(hfr_rate_rule(120, 7) == 60 && hfr_rate_rule(144, 7) == 60 && hfr_rate_rule(144, 4) == 60);
    CHECK(!hfr_pass_ring(hfr_rate_rule(0, 24)) && !hfr_pass_ring(hfr_rate_rule(30, 24)));
    CHECK(hfr_pass_ring(hfr_rate_rule(60, 24)) && hfr_pass_ring(hfr_rate_rule(120, 24)) && hfr_pass_ring(hfr_rate_rule(144, 24)));
    CHECK(hfr_pass_ring(hfr_rate_rule(144, 4)));

    /* 120 Hz with the grid on the real picture: alphas 1/4, 2/4, 3/4 */
    int n = hfr_plan(1e9, 1e9, 1e9 / 120.0, 1, a, tau, HFR_MAX);
    CHECK(n == 3 && a[0] == 64 && a[1] == 128 && a[2] == 192);
    CHECK(fabs(tau[1] - (1e9 + 2e9 / 120.0)) < 1.0);

    /* 144 Hz at every phase: four or five refreshes a game frame (one of them the real picture when its alpha is small), steps of R / P */
    int minn = 99, maxn = 0;
    for (int ph = 0; ph < 100; ph++) {
        double ref = 5e8 + ph * (1e9 / 144.0) / 100.0;
        n = hfr_plan(1e9, ref, 1e9 / 144.0, 1, a, tau, HFR_MAX);
        if (n < minn) minn = n;
        if (n > maxn) maxn = n;
        for (int i = 1; i < n; i++)
            CHECK(abs((int)a[i] - (int)a[i - 1] - 53) <= 1);   /* 0.2083 * 256 = 53.3 */
        for (int i = 0; i < n; i++)
            CHECK(a[i] >= 8 && a[i] <= 255 && tau[i] >= 1e9);
    }
    CHECK(minn == 4 && maxn == 5);

    /* a 60 Hz display with 144 chosen: the grid is the display's (divider 1): two pictures a game frame, 0.5 of a frame apart */
    n = hfr_plan(1e9, 1e9 + 3e6, HFR_VBLANK_NS, 1, a, tau, HFR_MAX);
    CHECK(n == 2 && a[0] == 23 && a[1] == 151);

    /* a 240 Hz display with 144 chosen: every second refresh: 120 pictures a second */
    CHECK(hfr_divider(240, 144) == 2 && hfr_divider(240, 120) == 2);
    n = hfr_plan(1e9, 1e9, 1e9 / 240.0, 2, a, tau, HFR_MAX);
    CHECK(n == 3);
    CHECK(hfr_divider(144, 120) == 1 && hfr_divider(60, 144) == 1 && hfr_divider(165, 144) == 1 && hfr_divider(360, 144) == 3);

    /* the clock: a steady arrival with jitter, a late record, a jump */
    HfrClock c = {0};
    double t = 1e9, e = 0;
    for (int i = 1; i <= 200; i++) {
        t += HFR_VBLANK_NS;
        double jitter = (i % 7 == 0 ? 3e6 : (i % 3) * 2e5);
        e = hfr_clock(&c, t + jitter, (uint64_t)i);
    }
    CHECK(fabs(e - t) < 1.5e6);
    double e2 = hfr_clock(&c, t + HFR_VBLANK_NS + 5e5, 201);
    CHECK(fabs(e2 - (t + HFR_VBLANK_NS)) < 1e6);
    double e3 = hfr_clock(&c, t + 5e8, 202);   /* half a second later: a pause */
    CHECK(e3 == t + 5e8);

    /* the core's timeline (the stamps of the second pass): a game frame that ends late and its VBlank right after it (the pairs of a heavy step: 15 ms and 1 ms after
     * their VBlanks) is not a jump on the timeline, and is one by the rule with no timeline; a break of the timeline is a jump at once */
    {
        HfrClock a1 = {0}, a0 = {0};
        int resets1 = 0, resets0 = 0, r;
        double ta = 5e9, ea = 0;
        for (int i = 1; i <= 400; i++) {
            ta += HFR_VBLANK_NS;
            double late = (i & 1) ? 15e6 : 1e6;   /* the frame's own VBlank ends 15 ms after its due time, the next one 1 ms after */
            ea = hfr_clock_timeline(&a1, ta + late, (uint64_t)i, 1, &r);
            resets1 += r && i > 1;
            hfr_clock_timeline(&a0, ta + late, (uint64_t)i, 0, &r);
            resets0 += r && i > 1;
        }
        CHECK(resets1 == 0);                     /* on the timeline: no jump */
        CHECK(resets0 > 100);                    /* the control: the same arrivals with no timeline reset again and again */
        CHECK(fabs(ea - ta - 8e6) < 2.5e6);      /* and the estimate sits among the arrivals (their mean is 8 ms late) */
        double eb = hfr_clock_timeline(&a1, ta + HFR_VBLANK_NS + 5e5, 401, 2, &r);   /* the timeline broke: the arrival */
        CHECK(r == 1 && eb == ta + HFR_VBLANK_NS + 5e5);
        double ec = hfr_clock_timeline(&a1, ta + 4 * HFR_VBLANK_NS, 404, 1, &r);   /* a gap of a few VBlanks on the timeline is no jump */
        CHECK(r == 0 && fabs(ec - (ta + 4 * HFR_VBLANK_NS)) < 2e6);
        double ed = hfr_clock_timeline(&a1, ta + 20 * HFR_VBLANK_NS, 424, 1, &r);   /* a gap of over 8 VBlanks is */
        CHECK(r == 1 && ed == ta + 20 * HFR_VBLANK_NS);
    }

    /* 60: the one picture at half a step, at b + a VBlank */
    n = hfr_plan_half(1e9, a, tau);
    CHECK(n == 1 && a[0] == 128 && fabs(tau[0] - (1e9 + HFR_VBLANK_NS)) < 1.0);
    CHECK(hfr_step_down(144, 144.0) == 60 && hfr_step_down(144, 180.0) == 120);
    CHECK(hfr_step_down(120, 144.0) == 60 && hfr_step_down(60, 60.0) == 30);


    /* the live alphas: the plan predicted at an anchor against the plan of the next one (hfr_predict, hfr_match) */
    {
        const double R = 1e9 / 144.0, B0 = 2e9, REF = 1.3e9;
        HfrPlan pr, ac;
        HfrMatch mt;
        /* a steady clock: the next anchor's b is b + P exactly: every refresh takes the predicted moment of its own time */
        int np = hfr_predict(B0, REF, R, 1, 0, &pr);
        CHECK(np >= 4 && np <= 5 && fabs(pr.b - (B0 + 2 * HFR_VBLANK_NS)) < 1.0);
        ac.b = B0 + 2 * HFR_VBLANK_NS;
        ac.n = hfr_plan(ac.b, REF, R, 1, ac.alpha, ac.tau, HFR_MAX);
        hfr_match(&pr, &ac, R, HFR_LIVE_STEP_NS, &mt);
        CHECK(!mt.stale && mt.matched == ac.n && mt.bs == 0);
        for (int i = 0; i < ac.n; i++)
            CHECK(mt.use[i] == i);
        /* every phase of the display against the step: all matched when the clock is steady */
        for (int ph = 0; ph < 50; ph++) {
            double ref = 1.3e9 + ph * R / 50.0;
            hfr_predict(B0, ref, R, 1, 0, &pr);
            ac.b = B0 + 2 * HFR_VBLANK_NS;
            ac.n = hfr_plan(ac.b, ref, R, 1, ac.alpha, ac.tau, HFR_MAX);
            hfr_match(&pr, &ac, R, HFR_LIVE_STEP_NS, &mt);
            CHECK(!mt.stale && mt.matched == ac.n);
        }
        /* a clock 0.5 ms off: the step keeps its moments, each refresh its own (the grid is the display's, so the times agree), the alpha it gets is the predicted one */
        ac.b = B0 + 2 * HFR_VBLANK_NS + 5e5;
        ac.n = hfr_plan(ac.b, REF, R, 1, ac.alpha, ac.tau, HFR_MAX);
        hfr_match(&pr, &ac, R, HFR_LIVE_STEP_NS, &mt);
        CHECK(!mt.stale && mt.matched >= ac.n - 1);
        /* 1.5 ms off: the whole step is B's; its control (a tolerance of a second) keeps the moments, so the test sees the rule at work */
        ac.b = B0 + 2 * HFR_VBLANK_NS + 1.5e6;
        ac.n = hfr_plan(ac.b, REF, R, 1, ac.alpha, ac.tau, HFR_MAX);
        hfr_match(&pr, &ac, R, HFR_LIVE_STEP_NS, &mt);
        CHECK(mt.stale && mt.matched == 0 && mt.bs == ac.n);
        for (int i = 0; i < ac.n; i++)
            CHECK(mt.use[i] == -1);
        hfr_match(&pr, &ac, R, 1e9, &mt);
        CHECK(!mt.stale && mt.matched >= ac.n - 1);   /* the control: with no tolerance rule the step is not B's */
        /* the edge refresh: b moved 0.9 ms later, so the refresh at the end of the window leaves the actual plan or the one at its start comes in: it has no moment and is B's, the others match */
        int edge_seen = 0;
        for (int ph = 0; ph < 200 && !edge_seen; ph++) {
            double ref = 1.3e9 + ph * R / 200.0;
            hfr_predict(B0, ref, R, 1, 0, &pr);
            ac.b = B0 + 2 * HFR_VBLANK_NS + 9e5;
            ac.n = hfr_plan(ac.b, ref, R, 1, ac.alpha, ac.tau, HFR_MAX);
            hfr_match(&pr, &ac, R, HFR_LIVE_STEP_NS, &mt);
            if (mt.bs == 1 && !mt.stale) {
                edge_seen = 1;
                CHECK(mt.matched == ac.n - 1);
                int k = 0;
                for (int i = 0; i < ac.n; i++)
                    k += mt.use[i] < 0;
                CHECK(k == 1);
            }
        }
        CHECK(edge_seen);
        /* 60: the one moment at 128; a steady clock matches it, a late b is B's */
        hfr_predict(B0, 0, 0, 1, 1, &pr);
        ac.b = B0 + 2 * HFR_VBLANK_NS;
        ac.n = hfr_plan_half(ac.b, ac.alpha, ac.tau);
        hfr_match(&pr, &ac, HFR_VBLANK_NS * 2, HFR_LIVE_STEP_NS, &mt);
        CHECK(pr.n == 1 && pr.alpha[0] == 128 && !mt.stale && mt.matched == 1 && mt.use[0] == 0);
        ac.b += 2e6;
        ac.n = hfr_plan_half(ac.b, ac.alpha, ac.tau);
        hfr_match(&pr, &ac, HFR_VBLANK_NS * 2, HFR_LIVE_STEP_NS, &mt);
        CHECK(mt.stale && mt.bs == 1);
    }

    printf(failures ? "hfr_test: %d FAILED\n" : "hfr_test: all passed\n", failures);
    return failures ? 1 : 0;
}
