/*
 * The picture's scaling to the window: see scale.h. The sharp mode is a separable cubic filter in 14-bit fixed point, applied
 * to the picture in the window's size (the horizontal pass over the picture's rows, the vertical pass over the window's), with
 * the filter's width following the scale when the picture is made smaller. The result is the same on every machine: integer
 * arithmetic only, whatever the number of threads.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "scale.h"

#define Q 14
#define MAXTAPS 16

int scale_mode_of(const char *w)
{
    static const char *const names[] = {"smooth", "sharp", "whole"};
    for (int i = 0; i < 3; i++)
        if (w && !SDL_strcasecmp(w, names[i]))
            return i;
    return -1;
}

int scale_strength_of(const char *w)
{
    static const char *const names[] = {"low", "medium", "high"};
    for (int i = 0; i < 3; i++)
        if (w && !SDL_strcasecmp(w, names[i]))
            return i + 1;
    return -1;
}

int scale_whole_size(int ww, int wh, int sw, int aw, int ah, int *ow, int *oh)
{
    int uw = sw, uh = (int)((int64_t)sw * ah / aw);   /* the multiple of one: the picture's width, at the shape of the screen */
    int n = ww / uw < wh / uh ? ww / uw : wh / uh;
    if (n < 1)
        return 0;
    *ow = uw * n;
    *oh = uh * n;
    return n;
}

/* ------------------------------------------------------------ the filter */

/* The cubic of Keys: A = -0.5 is Catmull-Rom; a more negative A is sharper, with more overshoot at an edge. */
static double cubic(double x, double a)
{
    x = fabs(x);
    if (x < 1.0)
        return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0)
        return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    return 0.0;
}

static double strength_a(int strength)
{
    return strength <= 1 ? -0.6 : strength == 2 ? -1.0 : -1.5;
}

typedef struct {
    int n;                 /* the number of outputs */
    int ntaps;             /* the taps of each, the same for all (the missing ones are weight 0) */
    int *first;            /* n: the first source index of each */
    int16_t *w;            /* n * ntaps: weights, Q14, summing to 1 << Q */
} Taps;

static void taps_free(Taps *t)
{
    free(t->first);
    free(t->w);
    memset(t, 0, sizeof(*t));
}

/* The taps that make DST outputs from SRC inputs (centres aligned, the edge pixel repeated beyond the picture). */
static int taps_make(Taps *t, int src, int dst, int strength)
{
    double a = strength_a(strength), scale = (double)src / dst, fs = scale > 1.0 ? scale : 1.0;
    int ntaps = (int)ceil(4.0 * fs) + 1;
    if (ntaps > MAXTAPS)
        ntaps = MAXTAPS;
    t->n = dst;
    t->ntaps = ntaps;
    t->first = malloc((size_t)dst * sizeof(int));
    t->w = calloc((size_t)dst * (size_t)ntaps, sizeof(int16_t));
    if (!t->first || !t->w) {
        taps_free(t);
        return -1;
    }
    for (int i = 0; i < dst; i++) {
        double c = ((double)i + 0.5) * scale - 0.5;
        int f = (int)floor(c - 2.0 * fs) + 1;
        double wt[MAXTAPS], sum = 0.0;
        for (int k = 0; k < ntaps; k++) {
            wt[k] = cubic(((double)(f + k) - c) / fs, a);
            sum += wt[k];
        }
        int iw[MAXTAPS], tot = 0, big = 0;
        for (int k = 0; k < ntaps; k++) {
            iw[k] = (int)lrint(wt[k] / sum * (double)(1 << Q));
            tot += iw[k];
            if (abs(iw[k]) > abs(iw[big]))
                big = k;
        }
        iw[big] += (1 << Q) - tot;
        t->first[i] = f;
        for (int k = 0; k < ntaps; k++)
            t->w[(size_t)i * (size_t)ntaps + (size_t)k] = (int16_t)iw[k];
    }
    return 0;
}

static inline uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ------------------------------------------------------------ the passes, by bands of rows */

typedef struct {
    const uint8_t *src;
    uint8_t *mid, *dst;
    int sw, sh, dw, dh;
    const Taps *tx, *ty;
} Job;

/* The horizontal pass over the picture's rows Y0 to Y1: sw x sh to dw x sh. */
static void pass_h(const Job *j, int y0, int y1)
{
    const Taps *t = j->tx;
    for (int y = y0; y < y1; y++) {
        const uint8_t *row = j->src + (size_t)y * (size_t)j->sw * 4;
        uint8_t *out = j->mid + (size_t)y * (size_t)j->dw * 4;
        for (int i = 0; i < j->dw; i++) {
            const int16_t *w = t->w + (size_t)i * (size_t)t->ntaps;
            int f = t->first[i], r = 0, g = 0, b = 0, a = 0;
            if (f >= 0 && f + t->ntaps <= j->sw) {
                const uint8_t *p = row + (size_t)f * 4;
                for (int k = 0; k < t->ntaps; k++, p += 4) {
                    r += w[k] * p[0];
                    g += w[k] * p[1];
                    b += w[k] * p[2];
                    a += w[k] * p[3];
                }
            } else {
                for (int k = 0; k < t->ntaps; k++) {
                    const uint8_t *p = row + (size_t)clampi(f + k, 0, j->sw - 1) * 4;
                    r += w[k] * p[0];
                    g += w[k] * p[1];
                    b += w[k] * p[2];
                    a += w[k] * p[3];
                }
            }
            out[4 * i + 0] = clamp8((r + (1 << (Q - 1))) >> Q);
            out[4 * i + 1] = clamp8((g + (1 << (Q - 1))) >> Q);
            out[4 * i + 2] = clamp8((b + (1 << (Q - 1))) >> Q);
            out[4 * i + 3] = clamp8((a + (1 << (Q - 1))) >> Q);
        }
    }
}

/* The vertical pass over the window's rows Y0 to Y1: dw x sh to dw x dh. */
static void pass_v(const Job *j, int y0, int y1)
{
    const Taps *t = j->ty;
    size_t rowbytes = (size_t)j->dw * 4;
    for (int y = y0; y < y1; y++) {
        const int16_t *w = t->w + (size_t)y * (size_t)t->ntaps;
        const uint8_t *p[MAXTAPS];
        int nt = t->ntaps;
        for (int k = 0; k < nt; k++)
            p[k] = j->mid + (size_t)clampi(t->first[y] + k, 0, j->sh - 1) * rowbytes;
        uint8_t *out = j->dst + (size_t)y * rowbytes;
        if (nt == 5) {   /* the case of every upscale (5 taps), unrolled so that the compiler vectorises it */
            const int w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3], w4 = w[4];
            const uint8_t *p0 = p[0], *p1 = p[1], *p2 = p[2], *p3 = p[3], *p4 = p[4];
            for (size_t x = 0; x < rowbytes; x++) {
                int v = w0 * p0[x] + w1 * p1[x] + w2 * p2[x] + w3 * p3[x] + w4 * p4[x];
                out[x] = clamp8((v + (1 << (Q - 1))) >> Q);
            }
        } else {
            for (size_t x = 0; x < rowbytes; x++) {
                int v = 0;
                for (int k = 0; k < nt; k++)
                    v += w[k] * p[k][x];
                out[x] = clamp8((v + (1 << (Q - 1))) >> Q);
            }
        }
    }
}

/* ------------------------------------------------------------ the workers */

#define MAXWORKERS 7

static struct {
    SDL_Thread *thr[MAXWORKERS];
    SDL_Semaphore *go[MAXWORKERS], *done[MAXWORKERS];
    int n;
    volatile int quit;
    struct { const Job *job; int stage, y0, y1; } task[MAXWORKERS];
    uint8_t *mid;
    size_t midcap;
    Taps tx, ty;
    int kx_src, kx_dst, ky_src, ky_dst, k_strength;
} S;

static void run_band(const Job *j, int stage, int y0, int y1)
{
    if (stage == 0)
        pass_h(j, y0, y1);
    else
        pass_v(j, y0, y1);
}

static int worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (;;) {
        SDL_WaitSemaphore(S.go[id]);
        if (S.quit)
            return 0;
        run_band(S.task[id].job, S.task[id].stage, S.task[id].y0, S.task[id].y1);
        SDL_SignalSemaphore(S.done[id]);
    }
}

static void workers_start(int want)
{
    if (want > MAXWORKERS)
        want = MAXWORKERS;
    while (S.n < want) {
        int id = S.n;
        S.go[id] = SDL_CreateSemaphore(0);
        S.done[id] = SDL_CreateSemaphore(0);
        if (!S.go[id] || !S.done[id])
            return;
        S.thr[id] = SDL_CreateThread(worker, "scale", (void *)(intptr_t)id);
        if (!S.thr[id])
            return;
        S.n++;
    }
}

/* One pass over ROWS rows, split in bands among this thread and the workers. */
static void run_pass(const Job *j, int stage, int rows, int workers)
{
    int nw = workers < S.n ? workers : S.n;
    int parts = nw + 1;
    int per = (rows + parts - 1) / parts;
    for (int i = 0; i < nw; i++) {
        int y0 = per * (i + 1), y1 = y0 + per;
        if (y0 > rows)
            y0 = rows;
        if (y1 > rows)
            y1 = rows;
        S.task[i].job = j;
        S.task[i].stage = stage;
        S.task[i].y0 = y0;
        S.task[i].y1 = y1;
        SDL_SignalSemaphore(S.go[i]);
    }
    run_band(j, stage, 0, per < rows ? per : rows);
    for (int i = 0; i < nw; i++)
        SDL_WaitSemaphore(S.done[i]);
}

int scale_resample(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, int strength, int workers)
{
    if (sw < 1 || sh < 1 || dw < 1 || dh < 1)
        return -1;
    if (S.kx_src != sw || S.kx_dst != dw || S.k_strength != strength || !S.tx.w) {
        taps_free(&S.tx);
        S.kx_src = S.kx_dst = 0;
        if (taps_make(&S.tx, sw, dw, strength))
            return -1;
        S.kx_src = sw;
        S.kx_dst = dw;
    }
    if (S.ky_src != sh || S.ky_dst != dh || S.k_strength != strength || !S.ty.w) {
        taps_free(&S.ty);
        S.ky_src = S.ky_dst = 0;
        if (taps_make(&S.ty, sh, dh, strength))
            return -1;
        S.ky_src = sh;
        S.ky_dst = dh;
    }
    S.k_strength = strength;
    size_t need = (size_t)dw * (size_t)sh * 4;
    if (need > S.midcap) {
        free(S.mid);
        S.mid = malloc(need);
        S.midcap = S.mid ? need : 0;
        if (!S.mid)
            return -1;
    }
    if (workers > 0)
        workers_start(workers);
    Job j = {src, S.mid, dst, sw, sh, dw, dh, &S.tx, &S.ty};
    run_pass(&j, 0, sh, workers);
    run_pass(&j, 1, dh, workers);
    return 0;
}

void scale_free(void)
{
    S.quit = 1;
    for (int i = 0; i < S.n; i++) {
        SDL_SignalSemaphore(S.go[i]);
        SDL_WaitThread(S.thr[i], NULL);
        SDL_DestroySemaphore(S.go[i]);
        SDL_DestroySemaphore(S.done[i]);
    }
    taps_free(&S.tx);
    taps_free(&S.ty);
    free(S.mid);
    memset(&S, 0, sizeof(S));
}
