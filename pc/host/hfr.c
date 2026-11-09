/* The timing of the frame rates above 60: see hfr.h. */
#include <math.h>

#include "hfr.h"

double hfr_clock(HfrClock *c, double arrival_ns, uint64_t vblank)
{
    if (c->have && vblank > c->vblank && vblank - c->vblank <= 8) {
        double pred = c->est + (double)(vblank - c->vblank) * HFR_VBLANK_NS;
        double err = arrival_ns - pred;
        if (fabs(err) <= 8e6) {
            c->est = pred + err / 8.0;
            c->vblank = vblank;
            c->steady++;
            return c->est;
        }
    }
    c->est = arrival_ns;
    c->vblank = vblank;
    c->steady = 0;
    c->have = 1;
    return c->est;
}

int hfr_divider(double refresh_hz, int rate)
{
    if (refresh_hz <= 0 || rate <= 0)
        return 1;
    int d = (int)floor(refresh_hz / (double)rate + 0.5);
    return d < 1 ? 1 : d;
}

int hfr_plan(double b_ns, double ref_ns, double refresh_ns, int divider, uint16_t *alpha, double *tau, int max)
{
    const double R = refresh_ns * (divider < 1 ? 1 : divider);
    const double P = 2.0 * HFR_VBLANK_NS;
    if (R <= 0)
        return 0;
    int n = 0;
    double k = ceil((b_ns - R / 2.0 - ref_ns) / R);
    for (;; k += 1.0) {
        double t = ref_ns + k * R;
        if (t >= b_ns + P - R / 2.0 || n >= max)
            break;
        double a = (t - b_ns) / P;
        if (a < 1.0 / 32.0)
            continue;   /* the real picture */
        int q = (int)floor(a * 256.0 + 0.5);
        alpha[n] = (uint16_t)(q > 255 ? 255 : q < 1 ? 1 : q);
        tau[n] = t;
        n++;
    }
    return n;
}

int hfr_plan_half(double b_ns, uint16_t *alpha, double *tau)
{
    alpha[0] = 128;
    tau[0] = b_ns + HFR_VBLANK_NS;
    return 1;
}

int hfr_step_down(int rate, double refresh_hz)
{
    if (rate >= 144) {
        if (hfr_divider(refresh_hz, 120) == hfr_divider(refresh_hz, 144))
            return 60;
        return 120;
    }
    return rate > 60 ? 60 : 30;
}
