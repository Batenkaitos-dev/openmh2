#ifndef PAL_HUB_H
#define PAL_HUB_H
/* The offline hub's setting (pal_hub.c; include/pal.h has Pal_OfflineHub). */
#include "pal_core.h"

/* main's option loop: 1 (one argument taken) when argv[i] is --offline-hub. */
s32 Pal_OfflineHubOption(s32 argc, char **argv, s32 i);

/* After the options are read, before the game starts: the setting in force. A saved session made with the other setting
 * is refused. */
void Pal_OfflineHubStart(void);

#endif
