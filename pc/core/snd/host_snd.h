#ifndef MH2_SND_HOST_SND_H
#define MH2_SND_HOST_SND_H

/*
 * The sound module's own host seam (pc/game_flags.txt): the
 * functions the module's platform files need from the host's C library and
 * operating system, in the host's convention (HOST, cdecl), implemented in
 * host_snd.c, which includes the system headers. (The IOP's threads run on
 * the kernel's raw fibers: snd_kernel.h.) None of them is observable by the game.
 */

#include "host.h"

/* ---- logs: "snd SYS: MSG" lines on stderr ----
 * Levels as the original's: 0 errors, 1 warnings, 2 info, 3 debug, 4 trace.
 * The level shown is MH2_SND_LOG (default 1). Variadic, so cdecl anyway. */
enum { SND_LOG_ERR, SND_LOG_WARN, SND_LOG_INFO, SND_LOG_DEBUG, SND_LOG_TRACE };
HOST int SndHost_LogLevel(void);
void SndHost_Log(int level, const char *sys, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
HOST void SndHost_Fatal(const char *sys, const char *msg) __attribute__((noreturn));

#endif
