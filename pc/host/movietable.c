/*
 * The movies' timing table (movietable.h). The measurement and the file are tools/pc/movie_table.py's.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcmake.h"
#include "hostfs.h"
#include "movielib.h"
#include "movietable.h"

#define RATE 48000

static const char *const k_comment = "# The movies' timing (tools/pc/movie_table.py): derived from the disc, build/ only.";

long long movietable_shown(double pts)
{
    if (pts <= 0.0)
        return 0;
    long long c = (long long)(pts * RATE);
    while (c > 0 && (double)(c - 1) / (double)RATE >= pts)
        c--;
    while ((double)c / (double)RATE < pts)
        c++;
    return c;
}

void movietable_repr(double x, char *out, size_t cap)
{
    if (x == 0.0) {
        snprintf(out, cap, "%s", signbit(x) ? "-0.0" : "0.0");
        return;
    }
    char buf[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof(buf), "%.*e", p - 1, x);
        if (strtod(buf, NULL) == x)
            break;
    }
    /* buf: [-]d[.ddd]e[+-]XX */
    char digits[24];
    int nd = 0, neg = buf[0] == '-';
    const char *s = buf + neg;
    for (; *s && *s != 'e'; s++)
        if (*s != '.' && nd < 20)
            digits[nd++] = *s;
    digits[nd] = 0;
    int e10 = atoi(s + 1);
    while (nd > 1 && digits[nd - 1] == '0')
        digits[--nd] = 0;
    int decpt = e10 + 1;
    char r[80];
    size_t k = 0;
    if (neg)
        r[k++] = '-';
    if (decpt > 16 || decpt <= -4) {   /* the exponent form */
        r[k++] = digits[0];
        if (nd > 1) {
            r[k++] = '.';
            memcpy(r + k, digits + 1, (size_t)nd - 1);
            k += (size_t)nd - 1;
        }
        k += (size_t)snprintf(r + k, sizeof(r) - k, "e%c%02d", e10 < 0 ? '-' : '+', abs(e10));
    } else if (decpt <= 0) {
        r[k++] = '0';
        r[k++] = '.';
        for (int i = 0; i < -decpt; i++)
            r[k++] = '0';
        memcpy(r + k, digits, (size_t)nd);
        k += (size_t)nd;
    } else if (decpt >= nd) {
        memcpy(r + k, digits, (size_t)nd);
        k += (size_t)nd;
        for (int i = nd; i < decpt; i++)
            r[k++] = '0';
        r[k++] = '.';
        r[k++] = '0';
    } else {
        memcpy(r + k, digits, (size_t)decpt);
        k += (size_t)decpt;
        r[k++] = '.';
        memcpy(r + k, digits + decpt, (size_t)(nd - decpt));
        k += (size_t)(nd - decpt);
    }
    r[k] = 0;
    snprintf(out, cap, "%s", r);
}

typedef struct Measure {
    long long frames, shown, sound;
    double top;
    char err[300];
} Measure;

/* One movie: SIZE bytes at OFFSET of the archive. 0 done, 1 stopped. */
static int measure(const MovieLib *lib, const char *archive, uint64_t offset, uint64_t size, MovieProgress progress, void *ud, int i,
                   int n, Measure *m)
{
    mh2movie_info info;
    memset(&info, 0, sizeof(info));
    memset(m, 0, sizeof(*m));
    mh2movie *mv = lib->open(archive, offset, size, &info, m->err, (int)sizeof(m->err));
    if (!mv) {
        m->frames = -1;
        m->sound = -1;
        if (!m->err[0])
            snprintf(m->err, sizeof(m->err), "cannot open it");
        return 0;
    }
    uint8_t *rgba = malloc((size_t)(info.width > 0 ? info.width : 1) * (size_t)(info.height > 0 ? info.height : 1) * 4);
    int16_t *audio = malloc(sizeof(int16_t) * 2 * 4096);
    if (!rgba || !audio) {
        lib->close(mv);
        free(rgba);
        free(audio);
        m->frames = -1;
        m->sound = -1;
        snprintf(m->err, sizeof(m->err), "out of memory");
        return 0;
    }
    long long frames = 0, samples = 0;
    double top = 0.0;
    int have_top = 0, rc = 1, stopped = 0;
    for (;;) {   /* as movie.c's worker: a frame, then the sound decoded meanwhile */
        double pts = 0.0;
        rc = lib->next_frame(mv, rgba, &pts);
        for (int got; (got = lib->audio(mv, audio, 4096)) > 0;)
            samples += got;
        if (rc <= 0)
            break;
        if (frames > 0 && (!have_top || pts > top)) {
            top = pts;
            have_top = 1;
        }
        frames++;
        if (progress && progress(ud, i, n, 1)) {
            stopped = 1;
            break;
        }
    }
    lib->close(mv);
    free(rgba);
    free(audio);
    if (stopped)
        return 1;
    if (rc < 0) {
        m->frames = -1;
        m->sound = -1;
        snprintf(m->err, sizeof(m->err), "decoding error after %lld frames", frames);
        return 0;
    }
    m->frames = frames;
    m->shown = movietable_shown(top);
    m->sound = info.has_audio ? samples : -1;
    m->top = top;
    return 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int movietable_make(const char *archive, const char *libdir, MovieProgress progress, MovieNote note, void *ud, char *text, size_t cap)
{
    uint32_t lba, fsize;
    uint64_t foff;
    if (arc_find(archive, "/AFS02.AFS", &lba, &fsize, &foff) != 0) {
        snprintf(text, cap, "%s is not a disc archive of this version, or has no AFS02.AFS", archive);
        return MT_E_ARCHIVE;
    }
    FILE *f = hfs_fopen(archive, "rb");
    uint8_t hdr[8];
    uint8_t *toc = NULL;
    uint32_t count = 0;
    int ok = 0;
    if (f) {
#ifdef _WIN32
        int sk = _fseeki64(f, (__int64)foff, SEEK_SET);
#else
        int sk = fseeko(f, (off_t)foff, SEEK_SET);
#endif
        if (sk == 0 && fread(hdr, 1, 8, f) == 8 && memcmp(hdr, "AFS\0", 4) == 0 && le32(hdr + 4) <= 1000) {
            count = le32(hdr + 4);
            toc = malloc(8 * (size_t)count + 8);
            ok = toc && fread(toc, 1, 8 * (size_t)count, f) == 8 * (size_t)count;
        }
        fclose(f);
    }
    if (!ok) {
        free(toc);
        snprintf(text, cap, "AFS02.AFS: not an AFS file");
        return MT_E_AFS;
    }
    MovieLib lib;
    char lpath[1200], why[300];
    memset(&lib, 0, sizeof(lib));
    int lrc = count ? movielib_load(&lib, libdir, lpath, sizeof(lpath), why, sizeof(why)) : MOVIELIB_OK;   /* (no movies: no library) */
    if (lrc != MOVIELIB_OK) {
        if (lrc == MOVIELIB_NOLOAD)   /* (no path, no file name: the words read the same on every system) */
            snprintf(text, cap, "the movie library could not be loaded: it is not beside mh2, or the FFmpeg files it needs are not");
        else
            snprintf(text, cap, "the movie library beside mh2 is of another version");
        free(toc);
        return MT_E_NOLIB;
    }
    size_t cap_lines = 256 + (size_t)count * 160;
    char *buf = malloc(cap_lines);
    if (!buf) {
        free(toc);
        snprintf(text, cap, "out of memory");
        return MT_E_WRITE;
    }
    size_t k = 0;
    k += (size_t)snprintf(buf + k, cap_lines - k, "%s\nmh2-movies 1 afs02 %u %u %u\n", k_comment, (unsigned)lba, (unsigned)fsize,
                          (unsigned)count);
    int rc = MT_OK;
    for (uint32_t i = 0; i < count && rc == MT_OK; i++) {
        if (progress && progress(ud, (int)i + 1, (int)count, 0)) {
            rc = MT_CANCELLED;
            break;
        }
        uint32_t off = le32(toc + 8 * i), size = le32(toc + 8 * i + 4);
        Measure m;
        if (measure(&lib, archive, foff + off, size, progress, ud, (int)i + 1, (int)count, &m)) {
            rc = MT_CANCELLED;
            break;
        }
        if (m.err[0] && note)
            note(ud, (int)i, m.err);
        char pts[40];
        movietable_repr(m.top, pts, sizeof(pts));
        k += (size_t)snprintf(buf + k, cap_lines - k, "%u %u %u %lld %lld %lld %s\n", (unsigned)i, (unsigned)off, (unsigned)size,
                              m.frames, m.shown, m.sound, pts);
    }
    free(toc);   /* (the library stays loaded: the game loads it next, and gets this one) */
    if (rc == MT_OK) {   /* the table beside the archive: written whole, then renamed over the old one */
        char out[1400], tmp[1400];
        snprintf(out, sizeof(out), "%s", archive);
        char *cut = NULL;
        for (char *p = out; *p; p++)
            if (*p == '/' || *p == '\\')
                cut = p;
        int dirlen = cut ? (int)(cut + 1 - out) : 0;
        snprintf(tmp, sizeof(tmp), "%.*smh2-movies.tmp", dirlen, out);
        snprintf(out + dirlen, sizeof(out) - (size_t)dirlen, "mh2-movies.txt");
        FILE *w = hfs_fopen(tmp, "wb");
        int bad = !w || fwrite(buf, 1, k, w) != k;
        if (w)
            bad |= fclose(w) != 0;
        if (bad || !SDL_RenamePath(tmp, out)) {
            snprintf(text, cap, "cannot write %s (%s)", out, bad ? strerror(errno) : SDL_GetError());
            hfs_remove(tmp);
            rc = MT_E_WRITE;
        }
    }
    free(buf);
    return rc;
}
