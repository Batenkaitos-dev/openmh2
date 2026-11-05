/*
 * The launcher (the launcher design, 1 and 4): a window mh2 shows before anything else starts, where the player looks at
 * the settings, changes them, and presses Play. It is a module of mh2 (product code), not a program of its own.
 */
#ifndef MH2_LAUNCHER_H
#define MH2_LAUNCHER_H

enum { LAUNCH_PLAY, LAUNCH_QUIT, LAUNCH_NONE };

/* Should the launcher open? Never in an automated run (g_cfg.automated, --no-window, --quit-after, a test's options, the
 * core's own arguments) or with --no-launcher; with --launcher, with Shift held at the start, or while mh2.ini does not
 * say launcher = off; and always when the archive is not found (the first run's steps, or the banner of a missing archive,
 * whatever mh2.ini says). The development test's script (--launcher-script) also opens it in a run that names a picture. */
int launcher_wanted(void);
/* Runs it. LAUNCH_PLAY: go on (the changes are in HostConfig, and saved in mh2.ini); LAUNCH_QUIT: the player quit;
 * LAUNCH_NONE: no window could be opened (said in the log): go on as if it had not been asked for. */
int launcher_run(void);

#endif
