/* The console (the SDK's mw/console, under the C library's write glue): the
 * platform layer's. ps2crt's stdout (crt/crt_stdout.c)
 * hands every line it flushes to crt_k_write(1, buf, n) (crt_kernel.h), the
 * C library's write(fd, buf, n) (0x1140E8).
 *
 * Observed on the boot route (the original's --lib-tree 1140E8): the first
 * write opens the console's file, sceOpen("tty00:", 2) through the fileio
 * client (the client's lock, its set-up check, a slot, the open RPC and the
 * waits on its answer); the descriptor it gives is kept, failure or not, and
 * the file is never opened again. Every write, the first too, is then a
 * sceWrite(descriptor, buf, n). Offline the original's fileio answers ENOENT
 * for "tty00:" (-2), so each sceWrite finds no open slot (the table's
 * semaphore, the lock taken and released) and fails with no RPC.
 *
 * The write's result is the count all the same (session 8, the original's
 * boot with --lib-tree 109130 and --peek 197A60): after the boot's three
 * printfs, each one sceWrite of its whole line, stdout's flags are 0x0889
 * (line buffered, writing, its buffer from malloc), without the error flag,
 * so the C library's flush saw no failed write, and each printf returns its
 * count (0x18, 0x1A and 0x1B at VBlank 2). A short count would
 * have made the flush write again: there is one sceWrite per line. Only
 * failed sceWrites are seen offline, so the result of one that succeeds is
 * not observed: the count, here, whatever sceWrite gives. */
#include "common.h"
#include "crt/crt_kernel.h"

s32 sceOpen(void *name, s32 flags, s32 mode);
s32 sceWrite(s32 fd, void *buf, s32 size);

static struct {
    int opened;   /* the open was made (whatever it gave) */
    s32 fd;
} g_con;

int crt_k_write(int fd, const void *buf, crt_u32 n)
{
    (void)fd;   /* only stdout (1) is written on the routes */
    if (!g_con.opened) {
        static char name[] = "tty00:";
        g_con.fd = sceOpen(name, 2, 0);
        g_con.opened = 1;
    }
    sceWrite(g_con.fd, (void *)buf, (s32)n);
    return (int)n;
}
