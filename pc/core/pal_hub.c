/*
 * The offline hub's setting (include/pal.h: Pal_OfflineHub). With it on, the village's gate leads to the online town
 * (the areas 0x84 to 0x88 of the lobby overlay) with no network: the game's own online town and its two quest counters,
 * played by one hunter. An enhancement: off by default, and with it off the game is Dos as it was. With it on the
 * game's state differs from a run without it, so a save state made with it loads only with it (and the other way
 * round), as with a text pack.
 *
 * The core's --offline-hub is how the host asks (mh2.ini's offline_hub = on, mh2's --offline-hub). Pal_OfflineHubStart
 * runs once, after the options are read and before the game starts. A release build records nothing: the setting is
 * only the option.
 */
#include "pal_core.h"

#include "pal_hub.h"
#include "host.h"

static s32 sOption;   /* --offline-hub */
static s32 sOn;       /* in force */

s32 Pal_OfflineHubOption(s32 argc, char **argv, s32 i)
{
    const char *a = argv[i], *b = "--offline-hub";
    (void)argc;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    if (*a || *b) {
        return 0;
    }
    sOption = 1;
    return 1;
}

void Pal_OfflineHubStart(void)
{
    sOn = sOption != 0;
    if (sOn) {
        Host_Log("offline hub: on (the village's gate leads to the online town, played offline; an enhancement: it changes the game's state)");
    }
}

s32 Pal_OfflineHub(void)
{
    return sOn;
}
