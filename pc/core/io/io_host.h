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
/* A pack: a folder (it holds a pack.ini) whose "files" folder has files that the game reads in place of the disc's, by
 * name: files/DATA.BIN/0/00050 for entry 50 of file 0 of DATA.BIN, files/SLPM_662.80 for a disc file, in the archive's
 * upper case. The archive must be open (the pack's sectors follow its last). Each file the game asks for gets sectors
 * of its own after the disc's, and Host_DiscFind, Host_DiscRead and Host_DiscLocation answer for them; a name the pack
 * lacks is the disc's. 0, or -1 (no archive, no pack.ini). The pack closes with the archive. */
HOST int Host_DiscPackOpen(const char *dir);
HOST void Host_DiscPackClose(void);
HOST int Host_DiscPackIsOpen(void);
/* Why the last Host_DiscPackOpen failed (a sentence that names the folder or file), and the open pack's name from its
 * pack.ini (empty when none is open). */
HOST const char *Host_DiscPackWhy(void);
HOST const char *Host_DiscPackName(void);
/* Ends the run with a message the host shows (Host_Refuse): the pack that was asked for can't be opened (Host_DiscPackWhy
 * says why). */
HOST void Host_DiscPackRefuse(void) __attribute__((noreturn));
/* The pack's text table (host_text.c): Host_TextLoad reads TEXT.TSV of the pack's folder with its character map (NULL: none); 0
 * when loaded or when the pack has no table, -1 when it fails a check (Host_TextWhy says why, with the line). Host_TextLookup is
 * the text layer's question (PC_TEXT in include/pal.h): the pack's replacement for a string, or the string. */
HOST int Host_TextLoad(const char *dir, const char *charmap);
HOST void Host_TextClose(void);
HOST const char *Host_TextWhy(void);
HOST unsigned int Host_TextCount(void);
HOST const char *Host_TextLookup(const char *s);
/* The open pack's hash (64 lower-case hex digits and a NUL): 0, or -1 when no pack is open. */
HOST int Host_DiscPackHash(char hex[65]);
/* The pack's file for a name (as Host_DiscFind takes it, or a key like "DATA.BIN/0/00050"): 0 with its first sector and
 * size, or -1 when the pack has none. */
HOST int Host_DiscPackFind(const char *key, unsigned int *lba, unsigned int *size);
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
