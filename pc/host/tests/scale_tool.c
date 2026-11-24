/*
 * The scaling's tool (a development program, never shipped; built with the tests):
 *
 *   scale_tool png IN.png W H MODE [STRENGTH] OUT.png
 *       the picture scaled to W x H by a mode (nearest, bilinear, sharp) as the host draws it, for looking at;
 *   scale_tool crop IN.png X Y W H OUT.png
 *       a part of a picture;
 *   scale_tool bench [RENDERER] [WORKERS]
 *       the cost of a picture at 1080p, 1440p and 4K window sizes: the processor's filter alone, then a whole picture through
 *       an SDL renderer in a hidden window (upload, draw, present, VSync off) for each mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../scale.h"

static SDL_Surface *load(const char *path)
{
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) {
        fprintf(stderr, "%s: %s\n", path, SDL_GetError());
        exit(2);
    }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    return c;
}

/* Bilinear and nearest as a GPU draws them (centres aligned), to show what the other modes are compared with. */
static void plain(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, int nearest)
{
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = (int)SDL_floorf(fy);
        float ty = fy - (float)y0;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = (int)SDL_floorf(fx);
            float tx = fx - (float)x0;
            for (int c = 0; c < 4; c++) {
                if (nearest) {
                    int nx = (int)(((float)x + 0.5f) * (float)sw / (float)dw), ny = (int)(((float)y + 0.5f) * (float)sh / (float)dh);
                    dst[((size_t)y * (size_t)dw + (size_t)x) * 4 + (size_t)c] = src[((size_t)ny * (size_t)sw + (size_t)nx) * 4 + (size_t)c];
                    continue;
                }
#define PX(xx, yy) src[((size_t)SDL_clamp(yy, 0, sh - 1) * (size_t)sw + (size_t)SDL_clamp(xx, 0, sw - 1)) * 4 + (size_t)c]
                float v = (1 - ty) * ((1 - tx) * PX(x0, y0) + tx * PX(x0 + 1, y0)) + ty * ((1 - tx) * PX(x0, y0 + 1) + tx * PX(x0 + 1, y0 + 1));
                dst[((size_t)y * (size_t)dw + (size_t)x) * 4 + (size_t)c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

static int do_png(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: scale_tool png IN.png W H nearest|bilinear|sharp [STRENGTH] OUT.png\n");
        return 2;
    }
    SDL_Surface *s = load(argv[2]);
    int dw = atoi(argv[3]), dh = atoi(argv[4]);
    const char *mode = argv[5];
    int strength = argc > 7 ? scale_strength_of(argv[6]) : 2;
    const char *out = argv[argc - 1];
    uint8_t *buf = malloc((size_t)dw * (size_t)dh * 4);
    if (!strcmp(mode, "sharp"))
        scale_resample(s->pixels, s->w, s->h, buf, dw, dh, strength, 3);
    else
        plain(s->pixels, s->w, s->h, buf, dw, dh, !strcmp(mode, "nearest"));
    SDL_Surface *o = SDL_CreateSurfaceFrom(dw, dh, SDL_PIXELFORMAT_RGBA32, buf, dw * 4);
    if (!SDL_SavePNG(o, out)) {
        fprintf(stderr, "%s: %s\n", out, SDL_GetError());
        return 1;
    }
    return 0;
}

static double now_ms(void)
{
    return (double)SDL_GetTicksNS() / 1e6;
}

static int do_bench(int argc, char **argv)
{
    const char *rname = argc > 2 && strcmp(argv[2], "-") ? argv[2] : NULL;
    int workers = argc > 3 ? atoi(argv[3]) : 0;
    static const int sizes[3][2] = {{1440, 1080}, {1920, 1440}, {2880, 2160}};   /* the 4:3 picture in a 1080p, 1440p and 4K window */
    static const int srcs[2][2] = {{640, 448}, {1280, 896}};
    SDL_Init(SDL_INIT_VIDEO);
    uint8_t *src = malloc(1280 * 896 * 4), *dst = malloc(3840 * 2160 * 4);
    for (int i = 0; i < 1280 * 896 * 4; i++)
        src[i] = (uint8_t)((i * 2654435761u) >> 24);
    for (int si = 0; si < 2; si++)
        for (int zi = 0; zi < 3; zi++) {
            int sw = srcs[si][0], sh = srcs[si][1], dw = sizes[zi][0], dh = sizes[zi][1];
            for (int st = 1; st <= 3; st += 2) {
                double t0 = now_ms();
                int n = 20;
                for (int i = 0; i < n; i++)
                    scale_resample(src, sw, sh, dst, dw, dh, st, workers);
                printf("filter %4dx%-4d -> %4dx%-4d strength %d workers %d: %6.2f ms\n", sw, sh, dw, dh, st, workers, (now_ms() - t0) / n);
            }
        }
    SDL_Window *win = SDL_CreateWindow("bench", 3840, 2160, SDL_WINDOW_HIDDEN);
    if (!win) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer *r = SDL_CreateRenderer(win, rname);
    if (!r) {
        fprintf(stderr, "renderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(r, 0);
    printf("renderer %s\n", SDL_GetRendererName(r));
    for (int si = 0; si < 2; si++)
        for (int zi = 0; zi < 3; zi++) {
            int sw = srcs[si][0], sh = srcs[si][1], dw = sizes[zi][0], dh = sizes[zi][1];
            SDL_SetWindowSize(win, dw + 200, dh + 100);
            SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, sw, sh);
            SDL_Texture *big = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, dw, dh);
            for (int mode = 0; mode < 4; mode++) {
                SDL_SetTextureScaleMode(tex, mode == 1 ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
                if (mode == 3) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_PIXELART);
                SDL_SetTextureScaleMode(big, SDL_SCALEMODE_NEAREST);
                int n = 60;
                double t0 = 0;
                for (int i = -5; i < n; i++) {
                    if (i == 0)
                        t0 = now_ms();
                    SDL_FRect d = {10, 10, (float)dw, (float)dh};
                    SDL_RenderClear(r);
                    if (mode == 2) {   /* sharp: the filter, the upload of the window's size, the draw 1:1 */
                        scale_resample(src, sw, sh, dst, dw, dh, 2, workers);
                        SDL_UpdateTexture(big, NULL, dst, dw * 4);
                        SDL_RenderTexture(r, big, NULL, &d);
                    } else {   /* smooth (0), whole (1: nearest) and pixel art (3): the picture's own size uploaded */
                        SDL_UpdateTexture(tex, NULL, src, sw * 4);
                        SDL_RenderTexture(r, tex, NULL, &d);
                    }
                    SDL_RenderPresent(r);
                }
                static const char *const nm[] = {"smooth", "whole(nearest)", "sharp", "pixelart"};
                printf("window %4dx%-4d picture %4dx%-4d %-15s %6.2f ms/picture\n", dw, dh, sw, sh, nm[mode], (now_ms() - t0) / n);
            }
            SDL_DestroyTexture(tex);
            SDL_DestroyTexture(big);
        }
    scale_free();
    return 0;
}

static int do_crop(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: scale_tool crop IN.png X Y W H OUT.png\n");
        return 2;
    }
    SDL_Surface *s = load(argv[2]);
    SDL_Rect r = {atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6])};
    SDL_Surface *c = SDL_CreateSurface(r.w, r.h, SDL_PIXELFORMAT_RGBA32);
    SDL_BlitSurface(s, &r, c, NULL);
    return SDL_SavePNG(c, argv[7]) ? 0 : 1;
}

/* scale_tool cmp WIN.png SRC.png smooth|sharp|whole [STRENGTH]: the window the host drew against the picture it was made from, scaled here:
 * the largest differences, and the pixels outside the picture's rectangle (which must be black). */
static int do_cmp(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: scale_tool cmp WIN.png SRC.png smooth|sharp|whole [low|medium|high] [AW AH]\n");
        return 2;
    }
    SDL_Surface *win = load(argv[2]), *src = load(argv[3]);
    const char *mode = argv[4];
    int strength = argc > 5 ? scale_strength_of(argv[5]) : 2;
    int aw = argc > 7 ? atoi(argv[6]) : 4, ah = argc > 7 ? atoi(argv[7]) : 3;
    int ww = win->w, wh = win->h, rw, rh;
    if (ww * ah > wh * aw) {
        rh = wh;
        rw = (int)((float)wh * (float)aw / (float)ah + 0.5f);
    } else {
        rw = ww;
        rh = (int)((float)ww * (float)ah / (float)aw + 0.5f);
    }
    if (!strcmp(mode, "whole") && !scale_whole_size(ww, wh, src->w, aw, ah, &rw, &rh))
        printf("whole: no whole multiple fits: the fit, filtered\n");
    int rx = (ww - rw) / 2, ry = (wh - rh) / 2;
    if (!strcmp(mode, "sharp")) {   /* (the host rounds the fitted rectangle's position the same way) */
        float fx = ((float)ww - (float)rw) / 2.0f, fy = ((float)wh - (float)rh) / 2.0f;
        rx = (int)(fx + 0.5f);
        ry = (int)(fy + 0.5f);
    }
    uint8_t *buf = malloc((size_t)rw * (size_t)rh * 4);
    if (!strcmp(mode, "sharp"))
        scale_resample(src->pixels, src->w, src->h, buf, rw, rh, strength, 3);
    else
        plain(src->pixels, src->w, src->h, buf, rw, rh, !strcmp(mode, "whole"));
    long diff = 0, big = 0, outside = 0;
    int maxd = 0;
    const uint8_t *wp = win->pixels;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const uint8_t *q = wp + ((size_t)y * (size_t)win->pitch + (size_t)x * 4);
            if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
                const uint8_t *r = buf + ((size_t)(y - ry) * (size_t)rw + (size_t)(x - rx)) * 4;
                int d = 0;
                for (int c = 0; c < 3; c++)
                    d = SDL_max(d, abs(q[c] - r[c]));
                diff += d != 0;
                big += d > 8;
                maxd = SDL_max(maxd, d);
            } else {
                outside += q[0] || q[1] || q[2];
            }
        }
    printf("cmp %s: window %dx%d, picture %dx%d at %d,%d: %ld of %ld pixels differ (max %d, over 8: %ld); outside not black: %ld\n", mode, ww, wh, rw, rh,
           rx, ry, diff, (long)rw * rh, maxd, big, outside);
    return 0;
}

/* scale_tool diff A.png B.png [OUT.png]: how two pictures of one size differ (a picture of the differences, times 4, to OUT). */
static int do_diff(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: scale_tool diff A.png B.png [OUT.png]\n");
        return 2;
    }
    SDL_Surface *a = load(argv[2]), *b = load(argv[3]);
    if (a->w != b->w || a->h != b->h) {
        printf("diff: sizes differ (%dx%d, %dx%d)\n", a->w, a->h, b->w, b->h);
        return 1;
    }
    SDL_Surface *o = SDL_CreateSurface(a->w, a->h, SDL_PIXELFORMAT_RGBA32);
    long nd = 0, big = 0;
    double sum = 0;
    int mx = 0, x0 = a->w, y0 = a->h, x1 = -1, y1 = -1;
    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            const uint8_t *p = (const uint8_t *)a->pixels + (size_t)y * (size_t)a->pitch + (size_t)x * 4;
            const uint8_t *q = (const uint8_t *)b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * 4;
            uint8_t *r = (uint8_t *)o->pixels + (size_t)y * (size_t)o->pitch + (size_t)x * 4;
            int d = 0;
            for (int c = 0; c < 3; c++) {
                int e = abs(p[c] - q[c]);
                d = SDL_max(d, e);
                sum += e;
                r[c] = (uint8_t)SDL_min(255, e * 4);
            }
            r[3] = 255;
            if (d) {
                nd++;
                big += d > 8;
                mx = SDL_max(mx, d);
                x0 = SDL_min(x0, x); x1 = SDL_max(x1, x); y0 = SDL_min(y0, y); y1 = SDL_max(y1, y);
            }
        }
    printf("diff: %ld of %ld pixels differ (%.2f%%), max %d, over 8: %ld, mean %.3f, box x %d-%d y %d-%d\n", nd, (long)a->w * a->h,
           100.0 * (double)nd / ((double)a->w * a->h), mx, big, sum / ((double)a->w * a->h * 3), x0, x1, y0, y1);
    if (argc > 4)
        SDL_SavePNG(o, argv[4]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "diff"))
        return do_diff(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "cmp"))
        return do_cmp(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "crop"))
        return do_crop(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "png"))
        return do_png(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "bench"))
        return do_bench(argc, argv);
    fprintf(stderr, "usage: scale_tool png ... | bench [RENDERER|-] [WORKERS]\n");
    return 2;
}
