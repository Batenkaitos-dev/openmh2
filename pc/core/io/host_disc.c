/* The disc archive (io_host.h), adapted from the original's disc.c
 * (its archive mode only: the core reads nothing else). The format is
 * tools/port/make_archive.py's docstring. The archive is over 2 GB, so its
 * offsets are 64-bit on i686 too. */
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700   /* realpath, fseeko */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_file.h"
#include "io/io_host.h"

#if defined(_WIN32)
#define SEEK64(f, o) _fseeki64((f), (long long)(o), SEEK_SET)
#define SEEKEND64(f) _fseeki64((f), 0, SEEK_END)
#define TELL64(f) ((unsigned long long)_ftelli64(f))
#else
#define SEEK64(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#define SEEKEND64(f) fseeko((f), 0, SEEK_END)
#define TELL64(f) ((unsigned long long)ftello(f))
#endif

#define SECTOR 2048u
#define ARC_HEADER 32u
#define ARC_ENTRY 128u
#define ARC_MAX 4096u

typedef struct ArcFile {
    char path[81];          /* normalized: "/IOP/MODULES/X.IRX" */
    unsigned int lba, size, sectors;
    unsigned long long off; /* the file's bytes in the archive */
    int used;               /* looked up or read: logged the first time */
} ArcFile;

static FILE *g_arc;
static ArcFile *g_files;
static unsigned int g_nfiles;
static unsigned int g_sectors;
static char g_arcPath[4096];   /* the archive's path, absolute when the system can say */

static void LogLine(const char *path, const char *what)
{
    char line[600];
    snprintf(line, sizeof(line), "disc: %s%s", path, what);
    Host_Log(line);
}

static unsigned int Rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

static unsigned long long Rd64(const unsigned char *p)
{
    return (unsigned long long)Rd32(p) | (unsigned long long)Rd32(p + 4) << 32;
}

/* The game's name of a file as the archive's: the device ("cdrom0:") and the
 * version (";1") dropped, '\\' as '/', upper case, one leading '/', no "//". */
static void Normalize(const char *in, char *out, size_t cap)
{
    const char *p = in;
    const char *colon = strchr(p, ':');
    size_t n = 0;
    if (colon)
        p = colon + 1;
    if (*p != '\\' && *p != '/' && n + 1 < cap)
        out[n++] = '/';
    for (; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c == ';')
            break;
        if (c == '\\')
            c = '/';
        else if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/' && n && out[n - 1] == '/')
            continue;
        out[n++] = c;
    }
    out[n] = 0;
}

static void NoteUse(ArcFile *f, const char *how)
{
    if (!f->used) {
        f->used = 1;
        char line[200];
        snprintf(line, sizeof(line), "disc: first %s of %s (lbn %u, %u bytes)", how, f->path, f->lba, f->size);
        Host_Log(line);
    }
}

HOST void Host_DiscClose(void)
{
    if (g_arc)
        fclose(g_arc);
    free(g_files);
    g_arc = NULL;
    g_files = NULL;
    g_nfiles = 0;
    g_sectors = 0;
}

HOST int Host_DiscIsOpen(void)
{
    return g_arc != NULL;
}

HOST int Host_DiscOpen(const char *path)
{
    Host_DiscClose();
    FILE *f = HostFile_Open(path, "rb");
    if (!f)
        return -1;
    unsigned char h[ARC_HEADER];
    unsigned int n = 0;
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || memcmp(h, "MH2DARC", 8) != 0 || Rd32(h + 8) != 1 ||
        Rd32(h + 16) != ARC_ENTRY || (n = Rd32(h + 12)) == 0 || n > ARC_MAX) {
        LogLine(path, " is not a disc archive of this version (tools/port/make_archive.py writes it)");
        fclose(f);
        return -2;
    }
    unsigned long long total = Rd64(h + 24);
    if (SEEKEND64(f) != 0 || TELL64(f) != total) {
        LogLine(path, " is incomplete (its size is not the header's)");
        fclose(f);
        return -2;
    }
    unsigned char *t = malloc((size_t)n * ARC_ENTRY);
    ArcFile *files = calloc(n, sizeof(ArcFile));
    if (!t || !files || SEEK64(f, ARC_HEADER) != 0 || fread(t, ARC_ENTRY, n, f) != n) {
        free(t);
        free(files);
        fclose(f);
        return -2;
    }
    unsigned int sectors = 0;
    for (unsigned int i = 0; i < n; i++) {
        const unsigned char *e = t + (size_t)i * ARC_ENTRY;
        char name[81];
        memcpy(name, e, 80);
        name[80] = 0;
        ArcFile *a = &files[i];
        Normalize(name, a->path, sizeof(a->path));
        a->lba = Rd32(e + 80);
        a->size = Rd32(e + 84);
        a->off = Rd64(e + 88);
        a->sectors = (unsigned int)(((unsigned long long)a->size + SECTOR - 1) / SECTOR);
        if (a->off % SECTOR || a->off + (unsigned long long)a->sectors * SECTOR > total) {
            LogLine(path, ": an entry is out of the archive");
            free(t);
            free(files);
            fclose(f);
            return -2;
        }
        if (a->lba + a->sectors > sectors)
            sectors = a->lba + a->sectors;
    }
    free(t);
    g_arc = f;
    g_files = files;
    g_nfiles = n;
    g_sectors = sectors;
#if defined(_WIN32)
    if (!_fullpath(g_arcPath, path, sizeof(g_arcPath)))
#else
    if (!realpath(path, g_arcPath))   /* g_arcPath holds PATH_MAX (4096) bytes */
#endif
        snprintf(g_arcPath, sizeof(g_arcPath), "%s", path);
    LogLine(path, ": the disc archive");
    return 0;
}

HOST int Host_DiscSideFile(const char *name, void **data, unsigned int *size)
{
    char path[4096 + 256];
    const char *slash, *bs;
    size_t dir;
    FILE *f;
    long long n;
    char *p;

    if (!g_arc || !name || !*name)
        return -1;
    slash = strrchr(g_arcPath, '/');
    bs = strrchr(g_arcPath, '\\');
    if (bs && (!slash || bs > slash))
        slash = bs;
    dir = slash ? (size_t)(slash - g_arcPath + 1) : 0;
    if (dir + strlen(name) >= sizeof(path))
        return -1;
    snprintf(path, sizeof(path), "%.*s%s", (int)dir, g_arcPath, name);
    f = HostFile_Open(path, "rb");
    if (!f)
        return -1;
    if (SEEKEND64(f) != 0 || (n = (long long)TELL64(f)) < 0 || n > 0x10000000 || SEEK64(f, 0) != 0) {
        fclose(f);
        return -1;
    }
    p = Host_Alloc((unsigned int)n + 1);
    if (!p || (n && fread(p, 1, (size_t)n, f) != (size_t)n)) {
        if (p)
            Host_Free(p);
        fclose(f);
        return -1;
    }
    fclose(f);
    p[n] = 0;
    *data = p;
    *size = (unsigned int)n;
    LogLine(path, ": a set-up file beside the archive");
    return 0;
}

HOST unsigned int Host_DiscSectors(void)
{
    return g_sectors;
}

HOST unsigned int Host_DiscFileCount(void)
{
    return g_nfiles;
}

HOST int Host_DiscFileAt(unsigned int i, const char **path, unsigned int *lba, unsigned int *size)
{
    if (i >= g_nfiles)
        return -1;
    *path = g_files[i].path;
    *lba = g_files[i].lba;
    *size = g_files[i].size;
    return 0;
}

HOST int Host_DiscFind(const char *path, unsigned int *lba, unsigned int *size)
{
    char norm[256];
    Normalize(path, norm, sizeof(norm));
    for (unsigned int i = 0; i < g_nfiles; i++) {
        if (strcmp(g_files[i].path, norm) == 0) {
            NoteUse(&g_files[i], "lookup");
            *lba = g_files[i].lba;
            *size = g_files[i].size;
            return 0;
        }
    }
    if (g_arc) {   /* the disc may hold it: the game would see a file the archive lacks */
        char line[300];
        snprintf(line, sizeof(line), "disc: %s is not in the archive", norm);
        Host_Log(line);
    }
    return -1;
}

static ArcFile *FileAt(unsigned int lba)
{
    for (unsigned int i = 0; i < g_nfiles; i++) {
        ArcFile *f = &g_files[i];
        if (lba >= f->lba && lba - f->lba < f->sectors)
            return f;
    }
    return NULL;
}

HOST int Host_DiscLocation(unsigned int lba, const char **path, unsigned long long *offset)
{
    ArcFile *f = g_arc ? FileAt(lba) : NULL;
    if (!f)
        return -1;
    *path = g_arcPath;
    *offset = f->off + (unsigned long long)(lba - f->lba) * SECTOR;
    return 0;
}

HOST int Host_DiscRead(unsigned int lba, unsigned int count, void *dst)
{
    unsigned char *out = dst;
    if (!g_arc)
        return -1;
    for (unsigned int cur = lba; cur - lba < count;) {
        ArcFile *f = FileAt(cur);
        if (!f) {
            char line[200];
            snprintf(line, sizeof(line), "disc: sector %u is not in the archive (a read of %u sectors at %u)", cur, count, lba);
            Host_Log(line);
            return -1;
        }
        NoteUse(f, "read");
        unsigned int end = f->lba + f->sectors;
        unsigned int n = (lba + count < end ? lba + count : end) - cur;
        if (SEEK64(g_arc, f->off + (unsigned long long)(cur - f->lba) * SECTOR) != 0 ||
            fread(out + (size_t)(cur - lba) * SECTOR, SECTOR, n, g_arc) != n) {
            char line[200];
            snprintf(line, sizeof(line), "disc: cannot read %s from the archive", f->path);
            Host_Log(line);
            return -1;
        }
        cur += n;
    }
    return 0;
}
