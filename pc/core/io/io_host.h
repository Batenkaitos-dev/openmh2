#ifndef MH2_CORE_IO_HOST_H
#define MH2_CORE_IO_HOST_H

/*
 * The io host seam (pc/core/host.h's rules): the functions here use the
 * host's calling convention and are written in host_*.c files, which include
 * the system headers. The game-facing files of io (the services and
 * pc/core/api's library files) reach the host's files only through these.
 */
#include "host.h"

/* ---- the disc archive (host_disc.c; tools/port/make_archive.py writes it) ----
 * The archive holds the files the game uses, each at its first sector (LBA) on
 * the disc. Read-only. */

/* Opens the archive at path: 0, -1 when there is no such file, -2 when it is
 * not an archive of this version or is incomplete (a line is logged). Any
 * archive opened before is closed. */
HOST int Host_DiscOpen(const char *path);
/* Closes the archive (nothing when none is open). */
HOST void Host_DiscClose(void);
/* 1 when an archive is open. */
HOST int Host_DiscIsOpen(void);
/* A file by its disc path, as the game names it: any device prefix ("cdrom0:")
 * and version suffix (";1") dropped, '\\' as '/', upper case. 0 and its first
 * sector and size, or -1. */
HOST int Host_DiscFind(const char *path, unsigned int *lba, unsigned int *size);
/* count sectors of 2048 bytes from lba into dst: 0, or -1 when a sector is in no
 * file of the archive (logged) or the archive can't be read. A read may run
 * from one file into the next. */
HOST int Host_DiscRead(unsigned int lba, unsigned int count, void *dst);
/* Where sector lba is on the host: the archive's path (absolute when the
 * system gives one; valid while it is open) and the byte offset of the
 * sector's bytes in it. 0, or -1 when the sector is in no file. */
HOST int Host_DiscLocation(unsigned int lba, const char **path, unsigned long long *offset);
/* The file name in the archive's directory, read whole (Io_DiscSideFile):
 * Host_Alloc's memory with a 0 byte after it, 0; or -1. */
HOST int Host_DiscSideFile(const char *name, void **data, unsigned int *size);
/* The disc's size in sectors as far as the archive knows it: one past the last
 * sector of its last file. */
HOST unsigned int Host_DiscSectors(void);
/* The archive's number of files, and the i-th one's path, sector and size (for
 * the tests and the logs). 0, or -1 past the end. */
HOST unsigned int Host_DiscFileCount(void);
HOST int Host_DiscFileAt(unsigned int i, const char **path, unsigned int *lba, unsigned int *size);

/* ---- memory (host_io.c) ----
 * Frees what host.h's Host_Alloc gave (NULL: nothing). */
HOST void Host_IoFree(void *p);

#endif
