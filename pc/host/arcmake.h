#ifndef MH2_ARCMAKE_H
#define MH2_ARCMAKE_H

/*
 * The disc archive's maker (the launcher design, 8.1; the archive is a copy, for good). From
 * the player's own disc, as a disc image or a folder with the disc's files, it writes mh2-disc.arc: the files the port opens,
 * each checked against its size and SHA-1, in the order of their sectors on the disc. It writes the file that
 * tools/port/make_archive.py writes from the same input, byte for byte (that tool stays the build's, and the oracle); the
 * format is the one in that tool's header.
 *
 * Inputs: the table of the files (their disc paths, sectors, sizes and SHA-1: config/disc_layout.txt and config/disc.sha1,
 * built into mh2 by pc/host/cmake/arc_tables.cmake) and the source. A disc image is read as ISO 9660 (ECMA-119; arcmake.c reads
 * only the volume descriptor and the directories on the way to the files); a file that does not match its table stops the
 * job, names the file, and leaves nothing behind (the half-written OUT.tmp is removed).
 *
 * No SDL thread, no window: a job runs in whatever thread calls it and reports through a callback, which can also stop it.
 * Offsets and sizes are 64-bit on every system (the archive is 2.7 GB).
 */
#include <stddef.h>
#include <stdint.h>

#define ARC_MAXFILES 16

typedef struct ArcFile {
    char path[80];          /* on the disc: "NTGUIDVD/BIN/BG.RGB", no leading slash */
    uint32_t lba, size;     /* the disc's first sector of it, and its size in bytes */
    uint8_t sha1[20];
} ArcFile;

typedef struct ArcTable {
    int n;
    ArcFile f[ARC_MAXFILES];
} ArcTable;

/* The built-in table (the files of the NTSC-J disc, version 1.04). */
void arc_builtin(ArcTable *t);

enum {
    AM_OK = 0,
    AM_CANCELLED,
    AM_E_TABLE,       /* the table itself is unusable (a test table) */
    AM_E_NOSOURCE,    /* the source is neither a file nor a folder */
    AM_E_NODISC,      /* a folder with no disc image and no disc files */
    AM_E_NOTISO,      /* the image is not ISO 9660 (with 2048-byte sectors) */
    AM_E_NOTONDISC,   /* the image's directory has no FILE */
    AM_E_LAYOUT,      /* the image has FILE elsewhere or of another size than the table says */
    AM_E_MISSING,     /* the folder has no FILE */
    AM_E_SIZE,        /* the folder's FILE has another size */
    AM_E_SHORT,       /* FILE is cut off */
    AM_E_BADSUM,      /* FILE's SHA-1 differs */
    AM_E_TAIL,        /* the rest of FILE's last sector on the image is not zero */
    AM_E_SPACE,       /* not enough free space */
    AM_E_READ,        /* a read error */
    AM_E_WRITE,       /* a write error (a full disk), or no folder for the archive */
    AM_E_OPEN         /* the source could not be opened */
};

typedef struct ArcResult {
    int code;               /* ARC_* */
    char file[96];          /* the disc file concerned (FILE above) */
    char where[1100];       /* a path: the source, or the folder that holds or should hold the file */
    char detail[200];       /* the system's words for a read or write error */
    uint64_t need, have;    /* AM_E_SPACE: the archive's size and the free bytes */
    uint64_t total;         /* the archive's size */
    int up_to_date;         /* AM_OK: OUT already was this archive; nothing was written */
} ArcResult;

/* FILE is 0 while the job prepares (the source is looked at, the room is checked), then 1 to NFILES as each file is copied.
 * DONE and TOTAL are the bytes of the files copied (all of them, over the whole job). Returns nonzero to stop the job. */
typedef int (*ArcProgress)(void *ud, int file, int nfiles, uint64_t done, uint64_t total, const char *name);

typedef struct ArcJob {
    const ArcTable *table;
    const char *source;     /* a disc image (a file), or a folder (with SYSTEM.CNF, or holding an image) */
    const char *out;        /* the archive */
    int force;              /* write it even if OUT already is this archive */
    uint64_t free_bytes;    /* (the tests, --archive-free) pretend this many bytes are free; 0: ask the system */
    ArcProgress progress;   /* or NULL */
    void *ud;
    int slow_file, slow_ms; /* (the tests, --archive-slow) wait SLOW_MS at the start of file number SLOW_FILE, in slices that notice a stop */
} ArcJob;

/* Writes the archive. The result is in R (always filled). */
int arc_make(const ArcJob *job, ArcResult *r);

/* The archive's size for TABLE, or 0 if the table is unusable. */
uint64_t arc_size(const ArcTable *t);

/* The bytes of an archive's header and entry table, as written (for a test and for the movie table's reader), by
 * sector order: HEAD gets at most CAP bytes; returns their count. */
size_t arc_head(const ArcTable *t, uint8_t *head, size_t cap);

/* Free bytes on the volume of PATH's folder (the nearest one that exists); 0 when it cannot be told. */
uint64_t arc_free_space(const char *path);

/* Where AFS02.AFS (or any file) is in an archive: its LBA, size and byte offset. 0, or -1 (not an archive of this version,
 * or no such file). NAME is the entry's path with its leading slash. */
int arc_find(const char *archive, const char *name, uint32_t *lba, uint32_t *size, uint64_t *offset);

#endif
