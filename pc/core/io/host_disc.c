/* The disc archive (io_host.h), adapted from the original's disc code
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
#include "io/host_sha256.h"
#include "io/io_host.h"
#include "ipc/packini.h"

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
#define PACK_KEY 96u
#define PACK_MAX 65536u

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

/* A pack (Host_DiscPackOpen): files of a folder read in place of the disc's, by name. Each one the game asks for is
 * given sectors after the disc's last, in the order it is first asked for; the answers for the names the pack lacks
 * are remembered, so a name is looked for once. */
typedef struct PackFile {
    char key[PACK_KEY];     /* normalized like the archive's names */
    char *path;             /* the file on the host; NULL: the pack has none by this name */
    unsigned int lba, size, sectors;
} PackFile;

static char *g_packDir;
static char g_packWhy[900];   /* why the last Host_DiscPackOpen failed (a sentence) */
static char g_packName[PACKINI_NAME];   /* the open pack's name (pack.ini) */
static char g_packHash[65];   /* the pack's hash (PackHash), made when it opens */
static PackFile *g_pack;
static unsigned int g_npack, g_capPack;
static unsigned int g_packBase, g_packNext;   /* the first virtual sector; the next free one */

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

static char *DupStr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

HOST void Host_DiscPackClose(void)
{
    for (unsigned int i = 0; i < g_npack; i++)
        free(g_pack[i].path);
    free(g_pack);
    free(g_packDir);
    g_pack = NULL;
    g_packDir = NULL;
    g_packName[0] = 0;
    Host_TextClose();
    g_npack = g_capPack = 0;
    g_packBase = g_packNext = 0;
}

HOST int Host_DiscPackIsOpen(void)
{
    return g_packDir != NULL;
}

/* The pack's file for a normalized name ("/DATA.BIN/0/00050"), its sectors given on the first ask; NULL when the pack
 * has none (or the name is not one a pack can hold). */
static PackFile *PackLookup(const char *norm)
{
    const char *key = norm[0] == '/' ? norm + 1 : norm;
    if (!g_packDir || !*key || strlen(key) >= PACK_KEY || strstr(key, ".."))
        return NULL;
    for (unsigned int i = 0; i < g_npack; i++)
        if (strcmp(g_pack[i].key, key) == 0)
            return g_pack[i].path ? &g_pack[i] : NULL;
    if (g_npack >= PACK_MAX)
        return NULL;
    if (g_npack == g_capPack) {
        unsigned int cap = g_capPack ? g_capPack * 2 : 64;
        PackFile *grown = realloc(g_pack, (size_t)cap * sizeof(PackFile));
        if (!grown)
            return NULL;
        g_pack = grown;
        g_capPack = cap;
    }
    PackFile *p = &g_pack[g_npack++];
    memset(p, 0, sizeof(*p));
    snprintf(p->key, sizeof(p->key), "%s", key);   /* the miss is remembered: path stays NULL */
    char path[4096 + PACK_KEY + 16];
    snprintf(path, sizeof(path), "%s/files/%s", g_packDir, key);
    FILE *f = HostFile_Open(path, "rb");
    if (!f)
        return NULL;
    int first = fgetc(f);   /* a folder opens on some systems and cannot be read: it is no file */
    if (first == EOF && ferror(f)) {
        fclose(f);
        return NULL;
    }
    long long n = SEEKEND64(f) == 0 ? (long long)TELL64(f) : -1;
    fclose(f);
    if (n <= 0 || n > 0x7FFFFFFFLL) {
        LogLine(path, n == 0 ? ": an empty file in the pack is ignored" : ": cannot be sized");
        return NULL;
    }
    p->path = DupStr(path);
    if (!p->path)
        return NULL;
    p->size = (unsigned int)n;
    p->sectors = (unsigned int)(((unsigned long long)n + SECTOR - 1) / SECTOR);
    p->lba = g_packNext;
    g_packNext += p->sectors;
    char line[600];
    snprintf(line, sizeof(line), "disc: pack file %s (lbn %u, %u bytes)", key, p->lba, p->size);
    Host_Log(line);
    return p;
}

static PackFile *PackAt(unsigned int lba)
{
    if (!g_packDir || lba < g_packBase)
        return NULL;
    for (unsigned int i = 0; i < g_npack; i++) {
        PackFile *p = &g_pack[i];
        if (p->path && lba >= p->lba && lba - p->lba < p->sectors)
            return p;
    }
    return NULL;
}

/* The pack's hash: the SHA-256 of one line for each file of the folder, at any depth, in the byte order of the relative
 * names: NAME, a tab, the file's size in decimal, a tab, the file's SHA-256 (64 lower-case hex digits), a line feed. NAME
 * is the path below the folder with '/' between its parts. */
typedef struct PackList {
    char **names;
    unsigned int n, cap;
    int bad;
} PackList;

static void PackWalk(const char *base, const char *rel, PackList *l);

typedef struct PackWalkArg {
    const char *base, *rel;
    PackList *l;
} PackWalkArg;

static void PackWalkEntry(const char *name, int is_folder, void *arg)
{
    PackWalkArg *a = (PackWalkArg *)arg;
    char rel[PACK_KEY * 4];
    snprintf(rel, sizeof(rel), "%s%s%s", a->rel, a->rel[0] ? "/" : "", name);
    if (is_folder) {
        PackWalk(a->base, rel, a->l);
        return;
    }
    if (a->l->n == a->l->cap) {
        unsigned int cap = a->l->cap ? a->l->cap * 2 : 64;
        char **grown = realloc(a->l->names, (size_t)cap * sizeof(char *));
        if (!grown) {
            a->l->bad = 1;
            return;
        }
        a->l->names = grown;
        a->l->cap = cap;
    }
    a->l->names[a->l->n] = DupStr(rel);
    if (!a->l->names[a->l->n])
        a->l->bad = 1;
    else
        a->l->n++;
}

static void PackWalk(const char *base, const char *rel, PackList *l)
{
    char dir[4096 + PACK_KEY * 4];
    PackWalkArg a = {base, rel, l};
    snprintf(dir, sizeof(dir), "%s%s%s", base, rel[0] ? "/" : "", rel);
    if (HostFile_List(dir, PackWalkEntry, &a) != 0) {
        l->bad = 1;
        snprintf(g_packWhy, sizeof(g_packWhy), "the folder %s can't be read", dir);
    }
}

static int PackNameCmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int PackHash(const char *dir, char hex[65])
{
    PackList l = {NULL, 0, 0, 0};
    HostSha256 all;
    int ok = 1;
    PackWalk(dir, "", &l);
    if (l.bad)
        ok = 0;
    qsort(l.names, l.n, sizeof(char *), PackNameCmp);
    HostSha256_Init(&all);
    for (unsigned int i = 0; ok && i < l.n; i++) {
        char path[4096 + PACK_KEY * 4], line[PACK_KEY * 4 + 100], fh[65];
        unsigned char buf[65536];
        HostSha256 one;
        unsigned long long size = 0;
        size_t got;
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", dir, l.names[i]);
        f = HostFile_Open(path, "rb");
        if (!f) {
            snprintf(g_packWhy, sizeof(g_packWhy), "the file %s can't be read", l.names[i]);
            ok = 0;
            break;
        }
        HostSha256_Init(&one);
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            HostSha256_Update(&one, buf, got);
            size += got;
        }
        fclose(f);
        HostSha256_Final(&one, fh);
        snprintf(line, sizeof(line), "%s\t%llu\t%s\n", l.names[i], size, fh);
        HostSha256_Update(&all, line, strlen(line));
    }
    HostSha256_Final(&all, hex);
    for (unsigned int i = 0; i < l.n; i++)
        free(l.names[i]);
    free(l.names);
    return ok ? 0 : -1;
}

HOST int Host_DiscPackHash(char hex[65])
{
    if (!g_packDir)
        return -1;
    memcpy(hex, g_packHash, 65);
    return 0;
}

/* A reason for Host_DiscPackWhy, with the folder named. */
static int PackFail(const char *dir, const char *what)
{
    snprintf(g_packWhy, sizeof(g_packWhy), "%s", what);
    LogLine(dir, g_packWhy[0] ? ": cannot be used as a translation pack" : "");
    return -1;
}

static void NoteFolder(const char *name, int is_folder, void *arg)
{
    (void)name;
    (void)is_folder;
    (*(int *)arg)++;
}

HOST const char *Host_DiscPackWhy(void)
{
    return g_packWhy;
}

HOST const char *Host_DiscPackName(void)
{
    return g_packName;
}

HOST void Host_DiscPackRefuse(void)
{
    char msg[1500];
    snprintf(msg, sizeof(msg), "The translation pack was not loaded: %s. Choose another pack, or set text_pack to off.", g_packWhy);
    Host_Refuse(msg);
}


HOST int Host_DiscPackOpen(const char *dir)
{
    Host_DiscPackClose();
    g_packWhy[0] = 0;
    if (!g_arc || !dir || !*dir)
        return PackFail(dir ? dir : "", g_arc ? "no folder was named" : "there is no disc archive to put the pack's files after");
    char marker[4096 + 16];
    int entries = 0;
    if (HostFile_List(dir, NoteFolder, &entries) != 0) {
        char what[900];
        snprintf(what, sizeof(what), "the folder %s doesn't exist or can't be read", dir);
        return PackFail(dir, what);
    }
    snprintf(marker, sizeof(marker), "%s/pack.ini", dir);
    FILE *f = HostFile_Open(marker, "rb");
    if (!f) {
        char what[900];
        snprintf(what, sizeof(what), "the folder %s has no pack.ini that can be read (a translation pack holds one)", dir);
        return PackFail(dir, what);
    }
    {
        static char ini[16384];
        PackIni pi;
        size_t got = fread(ini, 1, sizeof(ini), f);
        int tooBig = fgetc(f) != EOF;
        int bad = ferror(f);
        char why[300];
        fclose(f);
        if (bad || tooBig) {
            char what[900];
            snprintf(what, sizeof(what), "the pack.ini of %s %s", dir, bad ? "can't be read" : "is over 16 KB");
            return PackFail(dir, what);
        }
        if (PackIni_Parse(ini, (unsigned long)got, &pi, why, sizeof(why)) != 0) {
            char what[900];
            snprintf(what, sizeof(what), "%s (in %s)", why, dir);
            return PackFail(dir, what);
        }
        if (pi.hasCharmap) {
            char cm[4096 + 80];
            FILE *c;
            snprintf(cm, sizeof(cm), "%s/%s", dir, pi.charmap);
            c = HostFile_Open(cm, "rb");
            if (!c) {
                char what[900];
                snprintf(what, sizeof(what), "the character map %s that pack.ini names is not in %s", pi.charmap, dir);
                return PackFail(dir, what);
            }
            fclose(c);
        }
        snprintf(g_packName, sizeof(g_packName), "%s", pi.name);
        {   /* the character map: the file pack.ini names, else charmap.txt when the pack has one */
            char cm[4096 + 80];
            FILE *c;
            snprintf(cm, sizeof(cm), "%s/%s", dir, pi.charmap);
            c = HostFile_Open(cm, "rb");
            if (c)
                fclose(c);
            if (!c && !pi.hasCharmap)
                pi.charmap[0] = 0;
        }
        if (Host_TextLoad(dir, pi.charmap[0] ? pi.charmap : NULL) != 0) {
            char what[900];
            snprintf(what, sizeof(what), "%s (in %s)", Host_TextWhy(), dir);
            g_packName[0] = 0;
            return PackFail(dir, what);
        }
    }
    if (PackHash(dir, g_packHash) != 0) {
        char what[900];
        snprintf(what, sizeof(what), "%s", g_packWhy[0] ? g_packWhy : "a file of the pack can't be read");
        g_packName[0] = 0;
        return PackFail(dir, what);
    }
    g_packDir = DupStr(dir);
    if (!g_packDir) {
        g_packName[0] = 0;
        return PackFail(dir, "out of memory");
    }
    g_packBase = (g_sectors + 0x3FFu) & ~0x3FFu;
    g_packNext = g_packBase;
    {
        char line[200];
        snprintf(line, sizeof(line), "disc: pack hash %s", g_packHash);
        Host_Log(line);
    }
    {
        char line[200];
        snprintf(line, sizeof(line), "disc: %u text table entries", Host_TextCount());
        Host_Log(line);
    }
    LogLine(dir, ": a pack");
    return 0;
}

HOST int Host_DiscPackFind(const char *key, unsigned int *lba, unsigned int *size)
{
    char norm[PACK_KEY + 8];
    Normalize(key, norm, sizeof(norm));
    PackFile *p = PackLookup(norm);
    if (!p)
        return -1;
    *lba = p->lba;
    *size = p->size;
    return 0;
}

/* n sectors of the pack file p from sector cur of the disc's numbering; the bytes past the file's end are 0. */
static int PackRead(const PackFile *p, unsigned int cur, unsigned int n, unsigned char *out)
{
    unsigned long long off = (unsigned long long)(cur - p->lba) * SECTOR;
    unsigned long long want = (unsigned long long)n * SECTOR;
    unsigned long long have = p->size > off ? p->size - off : 0;
    FILE *f = HostFile_Open(p->path, "rb");
    int ok;
    if (!f)
        return -1;
    memset(out, 0, (size_t)want);
    if (have > want)
        have = want;
    ok = SEEK64(f, off) == 0 && fread(out, 1, (size_t)have, f) == (size_t)have;
    fclose(f);
    return ok ? 0 : -1;
}

HOST void Host_DiscClose(void)
{
    Host_DiscPackClose();
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
        LogLine(path, " is not a game archive for this version: make it again in the launcher (the Game archive page)");
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
    PackFile *pk = PackLookup(norm);
    if (pk) {   /* the pack's file in place of the disc's */
        *lba = pk->lba;
        *size = pk->size;
        return 0;
    }
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
    const PackFile *pk = PackAt(lba);
    if (pk) {
        *path = pk->path;
        *offset = (unsigned long long)(lba - pk->lba) * SECTOR;
        return 0;
    }
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
        const PackFile *pk = PackAt(cur);
        if (pk) {
            unsigned int pend = pk->lba + pk->sectors;
            unsigned int pn = (lba + count < pend ? lba + count : pend) - cur;
            if (PackRead(pk, cur, pn, out + (size_t)(cur - lba) * SECTOR) != 0) {
                char line[300];
                snprintf(line, sizeof(line), "disc: cannot read %s from the pack", pk->key);
                Host_Log(line);
                return -1;
            }
            cur += pn;
            continue;
        }
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
