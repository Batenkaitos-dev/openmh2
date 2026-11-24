/* The timing of the frame rates above 60: see hfr.h. */
#include <math.h>

#include "hfr.h"

double hfr_clock_timeline(HfrClock *c, double arrival_ns, uint64_t vblank, unsigned state, int *reset)
{
    if (c->have && vblank > c->vblank && vblank - c->vblank <= 8 && state != 2) {
        double pred = c->est + (double)(vblank - c->vblank) * HFR_VBLANK_NS;
        double err = arrival_ns - pred;
        if (state == 1 || fabs(err) <= 8e6) {
            if (err > 8e6)   /* on the core's timeline a late step is not a jump (the game's frame takes what it takes): the estimate moves by at most what a jump of 8 ms would give it */
                err = 8e6;
            else if (err < -8e6)
                err = -8e6;
            c->est = pred + err / 8.0;
            c->vblank = vblank;
            c->steady++;
            if (reset)
                *reset = 0;
            return c->est;
        }
    }
    if (reset)
        *reset = 1;
    c->est = arrival_ns;
    c->vblank = vblank;
    c->steady = 0;
    c->have = 1;
    return c->est;
}

double hfr_clock(HfrClock *c, double arrival_ns, uint64_t vblank)
{
    return hfr_clock_timeline(c, arrival_ns, vblank, 0, 0);
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

int hfr_rate_rule(int fps, int cpus)
{
    if (fps < 60)
        return 30;
    return fps > 60 && cpus < HFR_MIN_CPUS ? 60 : fps;
}

int hfr_pass_ring(int rate)
{
    return rate >= 60;
}

int hfr_predict(double b_ns, double ref_ns, double refresh_ns, int divider, int half, HfrPlan *out)
{
    const double P = 2.0 * HFR_VBLANK_NS;
    out->b = b_ns + P;
    out->n = half ? hfr_plan_half(out->b, out->alpha, out->tau) : hfr_plan(out->b, ref_ns, refresh_ns, divider, out->alpha, out->tau, HFR_MAX);
    return out->n;
}

void hfr_match(const HfrPlan *pred, const HfrPlan *act, double period_ns, double step_tol_ns, HfrMatch *m)
{
    int taken[HFR_MAX] = {0};
    m->stale = fabs(act->b - pred->b) > step_tol_ns;
    m->matched = m->bs = 0;
    for (int i = 0; i < HFR_MAX; i++)
        m->use[i] = -1;
    for (int i = 0; i < act->n && i < HFR_MAX; i++) {
        int best = -1;
        double bd = period_ns / 4.0;
        if (!m->stale)
            for (int j = 0; j < pred->n && j < HFR_MAX; j++) {
                double d = fabs(pred->tau[j] - act->tau[i]);
                if (!taken[j] && d < bd) {
                    bd = d;
                    best = j;
                }
            }
        if (best >= 0) {
            taken[best] = 1;
            m->use[i] = best;
            m->matched++;
        } else
            m->bs++;
    }
}
