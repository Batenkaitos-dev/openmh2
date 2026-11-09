/* The system configuration library (libscf): sceScfGetLanguage, the one call
 * Capcom's code makes (src/sub_main/net_sockets.c).
 *
 * Written from the library's behaviour at its interface, as the original shows it:
 * GetOsdConfigParam, the ROM's
 * version read once through the fileio client (its 14 bytes, the first time
 * only), GetOsdConfigParam again; the language from the configuration word. */
#include "common.h"
#include "libscf.h"
#include "io/io_kernel.h"

static char g_romver[0xE];

/* The ROM's version, read once (nothing when its first byte is set). */
static void ReadRomVer(void)
{
    if (g_romver[0])
        return;
    s32 fd = sceOpen("rom0:ROMVER", 1, 0);
    if (fd >= 0) {
        sceRead(fd, g_romver, sizeof(g_romver));
        sceClose(fd);
    }
}

u8 sceScfGetLanguage(void);
static u8 sceScfGetLanguageWork(void)
{
    u32 p = 0;
    GetOsdConfigParam(&p);
    ReadRomVer();
    GetOsdConfigParam(&p);
    if (p & 0xE000u)
        return (u8)(p >> 16 & 0x1F);
    return (u8)(p >> 4 & 1);
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls. */

u8 sceScfGetLanguage(void)
{
    return (u8)(sceScfGetLanguageWork());
}
