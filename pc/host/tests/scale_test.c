/*
 * scale.c's checks: the words of the settings, the whole-numbers size, and the sharp filter (a flat picture stays flat, the same
 * size changes nothing, a step keeps its edge in place, and the threads change nothing).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../scale.h"

static int fails;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #c);                \
            fails++;                                                   \
        }                                                              \
    } while (0)

static uint8_t *make(int w, int h, int kind)
{
    uint8_t *p = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *q = p + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (kind == 0) {   /* flat */
                q[0] = 200; q[1] = 17; q[2] = 99; q[3] = 255;
            } else if (kind == 1) {   /* a vertical step in the middle */
                uint8_t v = x < w / 2 ? 20 : 230;
                q[0] = v; q[1] = v; q[2] = v; q[3] = 255;
            } else {   /* noise, fixed */
                unsigned u = (unsigned)(x * 73856093u) ^ (unsigned)(y * 19349663u);
                u = u * 2654435761u;
                q[0] = (uint8_t)(u >> 24); q[1] = (uint8_t)(u >> 16); q[2] = (uint8_t)(u >> 8); q[3] = 255;
            }
        }
    return p;
}

int main(void)
{
    CHECK(scale_mode_of("smooth") == SCALE_SMOOTH && scale_mode_of("Sharp") == SCALE_SHARP && scale_mode_of("whole") == SCALE_WHOLE);
    CHECK(scale_mode_of("x") == -1 && scale_mode_of(NULL) == -1 && scale_mode_of("") == -1);
    CHECK(scale_strength_of("low") == 1 && scale_strength_of("medium") == 2 && scale_strength_of("high") == 3 && scale_strength_of("off") == -1);

    int ow = 0, oh = 0;
    CHECK(scale_whole_size(2560, 1440, 640, 4, 3, &ow, &oh) == 3 && ow == 1920 && oh == 1440);
    CHECK(scale_whole_size(2560, 1440, 640, 16, 9, &ow, &oh) == 4 && ow == 2560 && oh == 1440);
    CHECK(scale_whole_size(3840, 2160, 640, 4, 3, &ow, &oh) == 4 && ow == 2560 && oh == 1920);
    CHECK(scale_whole_size(1920, 1080, 640, 4, 3, &ow, &oh) == 2 && ow == 1280 && oh == 960);
    CHECK(scale_whole_size(2560, 1440, 1280, 4, 3, &ow, &oh) == 1 && ow == 1280 && oh == 960);
    CHECK(scale_whole_size(3840, 2160, 1280, 4, 3, &ow, &oh) == 2 && ow == 2560 && oh == 1920);
    CHECK(scale_whole_size(600, 800, 640, 4, 3, &ow, &oh) == 0);

    for (int st = 1; st <= 3; st++) {
        /* a flat picture stays flat, at every size (up, down, odd) */
        static const int sz[][2] = {{1920, 1440}, {641, 449}, {320, 224}, {1000, 333}};
        uint8_t *f = make(640, 448, 0);
        for (int i = 0; i < 4; i++) {
            uint8_t *d = malloc((size_t)sz[i][0] * (size_t)sz[i][1] * 4);
            CHECK(scale_resample(f, 640, 448, d, sz[i][0], sz[i][1], st, 0) == 0);
            int bad = 0;
            for (size_t k = 0; k < (size_t)sz[i][0] * (size_t)sz[i][1]; k++)
                bad += d[k * 4] != 200 || d[k * 4 + 1] != 17 || d[k * 4 + 2] != 99 || d[k * 4 + 3] != 255;
            CHECK(bad == 0);
            free(d);
        }
        /* the same size: no change */
        uint8_t *n = make(640, 448, 2), *d = malloc(640 * 448 * 4);
        CHECK(scale_resample(n, 640, 448, d, 640, 448, st, 0) == 0);
        CHECK(!memcmp(n, d, 640 * 448 * 4));
        free(d);
        /* the threads change nothing */
        uint8_t *a = malloc(1920 * 1440 * 4), *b = malloc(1920 * 1440 * 4);
        CHECK(scale_resample(n, 640, 448, a, 1920, 1440, st, 0) == 0);
        CHECK(scale_resample(n, 640, 448, b, 1920, 1440, st, 3) == 0);
        CHECK(!memcmp(a, b, 1920 * 1440 * 4));
        free(a); free(b); free(n); free(f);
        /* a step stays where it was: the middle of the output is the middle of the step, and its sides keep their levels */
        uint8_t *s = make(640, 448, 1), *o = malloc(1920 * 1440 * 4);
        CHECK(scale_resample(s, 640, 448, o, 1920, 1440, st, 0) == 0);
        const uint8_t *row = o + 700 * 1920 * 4;
        CHECK(row[100 * 4] == 20 && row[1800 * 4] == 230);
        CHECK(row[(960 - 2) * 4] < 128 && row[(960 + 1) * 4] > 128);
        free(s); free(o);
    }
    scale_free();
    if (fails) {
        printf("scale_test: %d FAIL\n", fails);
        return 1;
    }
    printf("scale_test: OK\n");
    return 0;
}
