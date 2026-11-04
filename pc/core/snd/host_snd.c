/* The sound module's host seam (host_snd.h): the logs and the fatal stop. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "snd/host_snd.h"

static int g_level = -1;

HOST int SndHost_LogLevel(void)
{
    if (g_level < 0) {
        const char *e = getenv("MH2_SND_LOG");
        g_level = e && *e ? atoi(e) : SND_LOG_WARN;
    }
    return g_level;
}

void SndHost_Log(int level, const char *sys, const char *fmt, ...)
{
    if (level > SndHost_LogLevel())
        return;
    static const char *const names[] = {"error", "warning", "info", "debug", "trace"};
    char msg[4224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "snd %s %s: %s\n", sys, names[level < 0 ? 0 : level > 4 ? 4 : level], msg);
}

HOST void SndHost_Fatal(const char *sys, const char *msg)
{
    fflush(stdout);
    fprintf(stderr, "mh2-core: snd %s: %s\n", sys, msg);
    fflush(stderr);
    abort();
}
