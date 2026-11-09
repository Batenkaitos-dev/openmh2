/*
 * Building the game's program from the launcher (the release's "Build the game" step). The release folder carries a private
 * build kit (kit/: a Python, the compiler, the build's scripts and source); this module runs its script, tools/pc/kitbuild.py,
 * as a child process on the player's disc, reads the progress lines it prints and reports them to the launcher's window.
 * The finished program is kept in the user data folder (core/), with a note of which kit made it.
 */
#ifndef MH2_LAUNCHER_BUILD_H
#define MH2_LAUNCHER_BUILD_H

#include <stddef.h>

enum { BD_IDLE, BD_RUNNING, BD_DONE, BD_FAILED, BD_CANCELLED };

typedef struct BuildStatus {
    int state;                  /* BD_* */
    int step, nsteps;           /* the step running (1 based) */
    char step_text[120];        /* its words, from the script */
    int done, total;            /* the compiler's progress inside step 4 (0: not known) */
    double secs;                /* since the start */
    char code[16];              /* BD_FAILED: disc, space, tool, build */
    char message[700];          /* BD_FAILED: what is wrong, for the player */
} BuildStatus;

/* The kit beside mh2 (kit/kit.txt): 1 and its identity in ID, or 0. */
int build_kit_present(char *id, size_t cap);
/* The user data folder holds a program made by this kit. */
int build_core_current(void);
/* Where the built program is (empty when there is no data folder). */
const char *build_core_path(void);
/* Starts the build from DISC (a disc image or a folder). 0 started; -1 not started, with the reason in ERR. */
int build_start(const char *disc, char *err, size_t cap);
/* Takes in what the script has said, and tells when it has ended; fills ST. */
void build_poll(BuildStatus *st);
/* Asks the script to stop (it ends everything it started, within a second or two). */
void build_cancel(void);
/* Forgets a finished build. */
void build_reset(void);
/* The build's log file. */
const char *build_log_path(void);

#endif
