#ifndef MH2_CORE_IO_H
#define MH2_CORE_IO_H

/*
 * The io platform layer: the
 * disc archive and the file services, cards, pads, the keyboard, settings and
 * logs. Its library functions are in pc/core/api (fileio.c, libcdvd.c, ...).
 * Compiled with the game's flags: no system headers here (host.h's rules).
 */
#include "common.h"

/* ---- the start ----
 * Io_Option: an io option of the command line at argv[i] (the kernel's main
 * calls it for each option): --disc FILE (the
 * disc archive), --memcard1 FILE, --memcard2 FILE (the cards in the two ports;
 * the core takes its cards only from these: without them the slot is empty).
 * Returns how many arguments it took, 0 if it isn't one.
 * Io_Start (from the kernel's start-up hook, Kern_InitIo, io_kernel.c): the
 * cards loaded, the archive opened (--disc, else $MH2_DISC, else mh2-disc.arc
 * in the working directory, else build/port/mh2-disc.arc), the IOP's services
 * registered with the kernel's registry. 0, or -1 when the archive can't be
 * opened (logged).
 * Io_Init: Io_Option over the whole command line, then Io_Start. */
s32 Io_Option(s32 argc, char **argv, s32 i);
int Io_Start(void);
int Io_Init(int argc, char **argv);
/* The IOP's services of io, registered with the kernel's registry (at
 * Io_Init, and again after an IOP reboot if the kernel asks). */
void Io_RegisterServices(void);

/* ---- logs ---- */
/* "mh2-core: io: TEXT", TEXT fmt with up to two values: %d %u %x %X %s (a
 * string's address as a u32: pointers are 32-bit here), with an optional 0 and
 * width (%08X). */
void Io_Log(const char *fmt, u32 a, u32 b);
/* fmt with a and b into out (cap bytes, always terminated); the length. */
u32 Io_Format(char *out, u32 cap, const char *fmt, u32 a, u32 b);

/* ---- small helpers (no C library here) ---- */
static inline u32 Io_StrLen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}
static inline int Io_StrNCmp(const char *a, const char *b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (u8)a[i] - (u8)b[i];
        if (!a[i])
            return 0;
    }
    return 0;
}

/* ---- the disc (io_disc.c, over the archive of io_host.h) ---- */
#define IO_SECTOR 2048u
/* A file by the game's name ("cdrom0:\\DATA\\X.BIN;1"): 0 with its sector and
 * size, or -1. */
int Io_DiscFind(const char *path, u32 *lba, u32 *size);
/* count sectors from lba into dst: 0 or -1 (fileio's loads, the
 * ELF's). */
int Io_DiscRead(u32 lba, u32 count, void *dst);
/* The same, for the reads the original game does itself: the CD service's
 * (sceCdRead) and the movie player's, which
 * look at AFS02.AFS's first two sectors
 * (movie.c: the host's). */
int Io_DiscReadSectors(u32 lba, u32 count, void *dst);

/* The pack of io_host.h (Host_DiscPackOpen): 0, or -1. */
int Io_DiscPackOpen(const char *dir);
int Io_DiscPackFind(const char *key, u32 *lba, u32 *size);
/* The open pack's hash (64 hex digits and a NUL): 0, or -1 when no pack is open. */
int Io_DiscPackHash(char hex[65]);
/* The open pack's name from its pack.ini ("" when no pack is open). */
const char *Io_DiscPackName(void);

/* The development log of the loads (io_loadlog.c). */
void IoLoadLog_Read(u32 lbn, u32 sectors);
void IoLoadLog_VBlank(void);
/* Where sector lba is on the host, for the host's movie player (the
 * original's disc_host_location): the archive's path (absolute when the
 * system gives one; valid while the run lasts) and the byte offset of the
 * sector in it; a file's sectors are contiguous there, the file's size is
 * Io_DiscFind's. 0, or -1 when the sector is in no file of the archive. */
int Io_DiscLocation(u32 lba, const char **path, u64 *offset);
/* A set-up file beside the disc archive (the movie table, mh2-movies.txt: the
 * lead's ruling on the host's third session), read whole into memory from
 * Host_Alloc with a 0 byte after its size bytes (Host_Free frees it): 0, or
 * -1 when no archive is open or the file can't be read. name is a plain file
 * name, joined to the archive's directory. */
int Io_DiscSideFile(const char *name, void **data, u32 *size);
/* size bytes of a file at byte offset pos (any alignment) into dst: 0 or -1. */
int Io_DiscReadBytes(u32 lba, u32 pos, u32 size, void *dst);

/* ---- the services (IOP side) ---- */
void IoFio_Register(void);       /* fileio 0x80000001: io_fio.c */
void IoFio_Reset(void);          /* every file closed (tests; an IOP reboot) */
void IoCdvd_Register(void);
void IoKb_Register(void);         /* the USB keyboard 0x80000211: io_usbkb.c */
/* The USB keyboard (io_usbkb.c; the original's model). Not connected (the default), the
 * service answers that there is none and the game never sees a key. Connected when --usb-keyboard says so, or the
 * host does (Io_KbHostWants, set by the glue: MH2IPC_HOST_USB_KEYBOARD).
 * The keyboard is the pad's software keyboard otherwise. */
extern s32 (*Io_KbHostWants)(void);
s32 Io_KbOption(s32 argc, char **argv, s32 i);   /* --usb-keyboard: 1 when it is that */
s32 Io_KbConnected(void);
/* The glue, at each VBlank's start with the pads: the host's key events (peek gives the next, take takes it; NULL
 * for neither) make this VBlank's report, which goes to Io_KbSetReport. Does nothing, but empty the ring,
 * without a keyboard. */
void Io_KbVBlank(s32 (*peek)(u32 *event), void (*take)(void));
/* A boot report (modifier bits, 0, six usage IDs) for this VBlank: queued for the service if it differs
 * from the last. */
void Io_KbSetReport(const u8 *report, u32 bytes);
void IoMc_Register(void);         /* mcserv 0x80000400: io_mc.c */
/* The card in port (0 or 1): the image at path (a missing file becomes a new
 * formatted card), or none (NULL). 0, or -1 (logged; the slot stays empty). */
int IoMc_Load(int port, const char *path);
/* The card in port as it is now, to another file. */
int IoMc_SaveCopy(int port, const char *path);      /* the CD/DVD services 0x80000592-0x8000059C: io_cdvd.c */

#endif
