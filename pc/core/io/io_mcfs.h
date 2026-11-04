#ifndef MH2_CORE_IO_MCFS_H
#define MH2_CORE_IO_MCFS_H

/*
 * The memory card file system on a PCSX2-format card image (host_mcfs.c, adapted
 * from the original's mcfs.c, whose comment has the on-card layout).
 * A host file: these functions use the host's convention (io_host.h's rules).
 * The card service (io_mc.c) calls them. Results follow the sceMc result codes.
 * Plain C types only: both the host's files and the game-facing ones include it.
 */
#include "host.h"

enum {
    MC_OK = 0,
    MC_ERR_CHANGED = -1,     /* also: a general failure */
    MC_ERR_NOFORMAT = -2,
    MC_ERR_FULL = -3,
    MC_ERR_NOENT = -4,
    MC_ERR_PERM = -5,
    MC_ERR_NOTEMPTY = -6,
    MC_ERR_MAXHANDLE = -7,
};
/* Open flags (libmc / mcserv). */
enum { MCO_RDONLY = 0x1, MCO_WRONLY = 0x2, MCO_RDWR = 0x3, MCO_MKDIR = 0x40, MCO_CREAT = 0x200 };
/* Entry mode bits. */
enum {
    MCM_READ = 0x1, MCM_WRITE = 0x2, MCM_EXEC = 0x4, MCM_DUPPROHIBIT = 0x8, MCM_FILE = 0x10, MCM_DIR = 0x20,
    MCM_CLOSED = 0x80, MCM_0400 = 0x400, MCM_PSX = 0x1000, MCM_HIDDEN = 0x2000, MCM_EXISTS = 0x8000,
};

/* sceMcTblGetDir: what GetDir gives per entry (64 bytes, as on the EE). */
typedef struct McDirInfo {
    unsigned char created[8];   /* sceMcStDateTime: resv, sec, min, hour, day, month, year (u16), JST */
    unsigned char modified[8];
    unsigned int size;          /* bytes for files, the entry count for directories */
    unsigned short mode;
    unsigned short reserve1;
    unsigned int attr;
    unsigned int pda_apl_no;
    char name[32];
} McDirInfo;

typedef struct McFs McFs;

/* The time the next changes are stamped with (JST, sceMcStDateTime's 8 bytes):
 * the card service sets it from the kernel's calendar before each request, so
 * that nothing here reads the host's clock. */
HOST void mcfs_set_clock(const unsigned char jst[8]);
/* Loads a card image; with create set, a missing file becomes a new formatted
 * 8 MB card. NULL if the file can't be used (logged). */
HOST McFs *mcfs_load(const char *path, int create);
HOST void mcfs_free(McFs *fs);
HOST const char *mcfs_path(const McFs *fs);
HOST int mcfs_formatted(const McFs *fs);
HOST int mcfs_format(McFs *fs);
HOST int mcfs_unformat(McFs *fs);
HOST unsigned int mcfs_free_clusters(McFs *fs);
/* Writes the changed pages back to the image file (the first change of a
 * session backs the file up to PATH.bak first). The pages go to PATH.journal
 * first (flushed), then to the card (flushed); a load completes a whole
 * journal and drops a partial one, so a load always finds the image after
 * some whole commit. MC_ERR_CHANGED when the write fails: the pages
 * stay marked for the next commit. */
HOST int mcfs_commit(McFs *fs);
/* For the tests: the next commit's writes (the journal, the card, a .tmp)
 * stop after BYTES bytes, as a process killed there would; -1 cancels. */
HOST void mcfs_test_stop_write(long bytes);
/* The whole image as it is now, to another file. */
HOST int mcfs_save_copy(const McFs *fs, const char *path);
HOST int mcfs_open(McFs *fs, const char *path, int flags);
HOST int mcfs_close(McFs *fs, int fd);
HOST int mcfs_read(McFs *fs, int fd, void *dst, unsigned int size);
HOST int mcfs_write(McFs *fs, int fd, const void *src, unsigned int size);
HOST int mcfs_seek(McFs *fs, int fd, int offset, int origin);   /* origin 0 set, 1 cur, 2 end */
HOST int mcfs_flush(McFs *fs, int fd);
/* Changes the current directory; cwd gets the new absolute path. */
HOST int mcfs_chdir(McFs *fs, const char *path, char *cwd, unsigned int cap);
/* Lists the entries a pattern matches (its last component may use ? and *).
 * With cont 0 a new listing starts; otherwise the last one goes on. */
HOST int mcfs_getdir(McFs *fs, const char *pattern, int cont, int maxent, McDirInfo *out);
HOST int mcfs_delete(McFs *fs, const char *path);
/* flags: 1 attr (mode bits), 2 created, 4 modified, 8 name (rename) ... */
HOST int mcfs_setinfo(McFs *fs, const char *path, const McDirInfo *info, unsigned int flags);
/* A directory's free entry slots (sceMcGetEntSpace). */
HOST int mcfs_getentspace(McFs *fs, const char *path);

#endif
