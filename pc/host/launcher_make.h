/*
 * Making the disc archive from the launcher (the launcher design, 4.2, 6.1 and 8.1). The maker is mh2's own: arcmake.c writes the
 * archive from the player's disc image or folder, movietable.c the movies' timing table beside it; this module runs them on a
 * thread and reports what they find. The job has three stages: getting ready (the source against the known disc, the free space),
 * the archive's files, the movies' timing table.
 */
#ifndef MH2_LAUNCHER_MAKE_H
#define MH2_LAUNCHER_MAKE_H

#include <stddef.h>
#include <stdint.h>

#include "arcmake.h"

enum { MK_IDLE, MK_RUNNING, MK_DONE, MK_FAILED, MK_CANCELLED };
enum { MKS_SIZE = 1, MKS_FILES, MKS_MOVIES };

typedef struct MakeStatus {
    int state;                  /* MK_* */
    int stage;                  /* MKS_*: the stage running (or the one that failed) */
    int file, nfiles;           /* the archive's file being copied */
    char name[120];             /* its name on the disc (AFS01.AFS) */
    uint64_t done, total;       /* bytes of the archive's files */
    int movie, nmovies;         /* the movie being timed */
    double secs;                /* since the start */
    int archive_made;           /* MK_FAILED in the last stage: the archive itself is whole */
    char message[700];          /* MK_FAILED: what is wrong, in the launcher's words; MK_DONE: the last line */
} MakeStatus;

/* Starts making OUT from SOURCE (a disc image, or a folder with the disc's files). FORCE: even if OUT is up to date. 0
 * started; -1 not started, with the reason in ERR (a test table that cannot be read; every other trouble is the job's). */
int make_start(const char *source, const char *out, int force, char *err, size_t cap);
/* Takes in what the job has found, and tells when it has ended; fills ST. */
void make_poll(MakeStatus *st);
/* Stops the job (it notices within a chunk of 8 MB, or a frame of a movie) and removes the half-written file (OUT.tmp). */
void make_cancel(void);
/* Forgets a finished job (the next make_start begins afresh). */
void make_reset(void);

/* Free bytes on the volume of PATH's folder (the nearest one that exists); 0 when it cannot be told. */
uint64_t make_free_space(const char *path);
/* "2.72 GB" */
void make_size_text(uint64_t bytes, char *out, size_t cap);

/* Where the player's copy may be: beside mh2, in the folder above it, in MH2_ORIG_DIR (a disc image, or a folder with the
 * disc's files). Returns how many were put in OUT (at most MAX). */
typedef struct MakeSource {
    char path[1200];
    int iso;   /* a disc image; otherwise a folder */
} MakeSource;
int make_sources(MakeSource *out, int max, int env_only);   /* ENV_ONLY: MH2_ORIG_DIR alone (the test) */

/* The launcher's words for a failed job. */
void make_explain(const ArcResult *r, char *out, size_t cap);


#endif
