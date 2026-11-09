/* The timing of the frame rates above 60 (hfr.c): the alphas of a game frame, the clock, the divider and the step down. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../hfr.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("hfr_test: line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
    uint16_t a[HFR_MAX];
    double tau[HFR_MAX];

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

    /* 60: the one picture at half a step, at b + a VBlank */
    n = hfr_plan_half(1e9, a, tau);
    CHECK(n == 1 && a[0] == 128 && fabs(tau[0] - (1e9 + HFR_VBLANK_NS)) < 1.0);
    CHECK(hfr_step_down(144, 144.0) == 60 && hfr_step_down(144, 180.0) == 120);
    CHECK(hfr_step_down(120, 144.0) == 60 && hfr_step_down(60, 60.0) == 30);

    printf(failures ? "hfr_test: %d FAILED\n" : "hfr_test: all passed\n", failures);
    return failures ? 1 : 0;
}
