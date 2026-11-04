/* ps2crt's diagnostics: the inputs the game was never seen to pass
 * (ps2crt.h, CRT_UNSEEN). They are counted, and the first call of each
 * function is kept (its name and its format) for the platform layer's debug
 * output: crt_unseen_get. Nothing here reaches a host API. */
#include "ps2crt.h"

#define CRT_UNSEEN_MAX 32

static struct {
    const char *func;
    const char *fmt;
    u32 count;
} g_seen[CRT_UNSEEN_MAX];
static u32 g_nseen, g_total;

void crt_unseen(const char *func, const char *fmt, ...)
{
    g_total++;
    for (u32 i = 0; i < g_nseen; i++) {
        if (g_seen[i].func == func && g_seen[i].fmt == fmt) {
            g_seen[i].count++;
            return;
        }
    }
    if (g_nseen < CRT_UNSEEN_MAX) {
        g_seen[g_nseen].func = func;
        g_seen[g_nseen].fmt = fmt;
        g_seen[g_nseen].count = 1;
        g_nseen++;
    }
}

u32 crt_unseen_count(void)
{
    return g_total;
}

/* The i-th distinct (function, format) pair seen and how often; 0 past the end. */
int crt_unseen_get(u32 i, const char **func, const char **fmt, u32 *count)
{
    if (i >= g_nseen)
        return 0;
    *func = g_seen[i].func;
    *fmt = g_seen[i].fmt;
    *count = g_seen[i].count;
    return 1;
}
