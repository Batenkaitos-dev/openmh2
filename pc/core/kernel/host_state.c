/* The save states' host functions (khost_state.h): system headers, the host's calling convention. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE     /* dl_iterate_phdr */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <link.h>
#endif

#include "host_file.h"
#include "kernel/khost_state.h"
#include "zlib.h"

HOST int HostBuf_Append(HostBuf *b, const void *data, unsigned int n)
{
    if (b->n + n > b->cap) {
        unsigned int cap = b->cap ? b->cap : 65536u;
        unsigned char *np;
        while (cap < b->n + n) {
            cap = cap > 0x40000000u ? 0xFFFFFFFFu : cap * 2u;
        }
        np = (unsigned char *)realloc(b->p, cap);
        if (!np) {
            return -1;
        }
        b->p = np;
        b->cap = cap;
    }
    if (n) {
        memcpy(b->p + b->n, data, n);
    }
    b->n += n;
    return 0;
}

HOST void HostBuf_Free(HostBuf *b)
{
    free(b->p);
    b->p = 0;
    b->n = b->cap = 0;
}

/* ---------------------------------------------------------------- layout */

#ifdef _WIN32
extern IMAGE_DOS_HEADER __ImageBase;

HOST void HostState_Layout(HostStateLayout *l)
{
    const unsigned char *base = (const unsigned char *)&__ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + __ImageBase.e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    unsigned int i;
    memset(l, 0, sizeof(*l));
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        char name[9];
        unsigned int lo = (unsigned int)(size_t)base + s->VirtualAddress;
        unsigned int size = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        memcpy(name, s->Name, 8);
        name[8] = 0;
        if (!strcmp(name, "hostbss") || !strcmp(name, "hostdat") || !strcmp(name, "hostnb")) {
            if (l->nHost < HOSTSTATE_RANGES) {
                l->hostLo[l->nHost] = lo;
                l->hostHi[l->nHost++] = lo + size;
            }
        } else if ((s->Characteristics & IMAGE_SCN_MEM_WRITE) && !(s->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) &&
                   strcmp(name, ".idata") && strcmp(name, ".tls") && strcmp(name, ".CRT") && strcmp(name, ".bss$")) {
            /* the game's data and bss blocks, .data and .bss; the import table, the thread-local data and the
             * start-up's tables are the loader's */
            if (l->nData < HOSTSTATE_RANGES) {
                l->dataLo[l->nData] = lo;
                l->dataHi[l->nData++] = lo + size;
            }
        } else if (!strcmp(name, ".text")) {
            l->codeLo = lo;
            l->codeHi = lo + size;
        }
    }
}
#else
extern char __data_start[], __executable_start[], etext[];
extern char __start_hostbss[] __attribute__((weak)), __stop_hostbss[] __attribute__((weak));
extern char __start_hostdat[] __attribute__((weak)), __stop_hostdat[] __attribute__((weak));
extern char __start_hostnb[] __attribute__((weak)), __stop_hostnb[] __attribute__((weak));

/* The executable's writable segments from __data_start on: .data and .bss, which are separate segments with a
 * gap of unmapped pages between them. (Before __data_start: the relocation tables and the dynamic section.) */
static int OnSegments(struct dl_phdr_info *info, size_t size, void *arg)
{
    HostStateLayout *l = (HostStateLayout *)arg;
    int i;
    (void)size;
    if (info->dlpi_name && info->dlpi_name[0]) {
        return 0;     /* the main program is the first object, with no name */
    }
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        unsigned int lo, hi;
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) {
            continue;
        }
        lo = (unsigned int)(info->dlpi_addr + ph->p_vaddr);
        hi = lo + (unsigned int)ph->p_memsz;
        if (lo < (unsigned int)(size_t)__data_start) {
            lo = (unsigned int)(size_t)__data_start;
        }
        if (hi > lo && l->nData < HOSTSTATE_RANGES) {
            l->dataLo[l->nData] = lo;
            l->dataHi[l->nData++] = hi;
        }
    }
    return 1;
}

HOST void HostState_Layout(HostStateLayout *l)
{
    memset(l, 0, sizeof(*l));
    dl_iterate_phdr(OnSegments, l);
    if (__start_hostbss) {
        l->hostLo[l->nHost] = (unsigned int)(size_t)__start_hostbss;
        l->hostHi[l->nHost++] = (unsigned int)(size_t)__stop_hostbss;
    }
    if (__start_hostdat) {
        l->hostLo[l->nHost] = (unsigned int)(size_t)__start_hostdat;
        l->hostHi[l->nHost++] = (unsigned int)(size_t)__stop_hostdat;
    }
    if (__start_hostnb) {
        l->hostLo[l->nHost] = (unsigned int)(size_t)__start_hostnb;
        l->hostHi[l->nHost++] = (unsigned int)(size_t)__stop_hostnb;
    }
    /* the C library's stream pointers, copy-relocated into the executable's .bss: libc's, at a random base */
    l->hostLo[l->nHost] = (unsigned int)(size_t)&stdout;
    l->hostHi[l->nHost++] = (unsigned int)(size_t)&stdout + sizeof(stdout);
    l->hostLo[l->nHost] = (unsigned int)(size_t)&stderr;
    l->hostHi[l->nHost++] = (unsigned int)(size_t)&stderr + sizeof(stderr);
    l->codeLo = (unsigned int)(size_t)__executable_start;
    l->codeHi = (unsigned int)(size_t)etext;
}
#endif

/* ------------------------------------------------------------ compressor */

HOST int HostState_Deflate(const void *in, unsigned int n, HostBuf *out, int level)
{
    z_stream z;
    unsigned char chunk[65536];
    int rc;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return -1;
    }
    z.next_in = (Bytef *)in;
    z.avail_in = n;
    do {
        z.next_out = chunk;
        z.avail_out = sizeof(chunk);
        rc = deflate(&z, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            deflateEnd(&z);
            return -1;
        }
        if (HostBuf_Append(out, chunk, (unsigned int)(sizeof(chunk) - z.avail_out)) != 0) {
            deflateEnd(&z);
            return -1;
        }
    } while (rc != Z_STREAM_END);
    n = (unsigned int)z.total_out;
    deflateEnd(&z);
    return (int)n;
}

HOST int HostState_Inflate(const void *in, unsigned int n, void *out, unsigned int outN)
{
    z_stream z;
    int rc;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) {
        return -1;
    }
    z.next_in = (Bytef *)in;
    z.avail_in = n;
    z.next_out = (Bytef *)out;
    z.avail_out = outN;
    rc = inflate(&z, Z_FINISH);
    n = (unsigned int)z.total_out;
    inflateEnd(&z);
    return rc == Z_STREAM_END && n == outN ? 0 : -1;
}

HOST unsigned int HostState_Adler(const void *data, unsigned int n)
{
    return (unsigned int)adler32(1, (const Bytef *)data, n);
}

/* ------------------------------------------------------------------ files */

HOST int HostState_ReadFile(const char *path, HostBuf *out)
{
    unsigned char chunk[65536];
    size_t got;
    FILE *f = HostFile_Open(path, "rb");
    if (!f) {
        return -1;
    }
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (HostBuf_Append(out, chunk, (unsigned int)got) != 0) {
            fclose(f);
            return -1;
        }
    }
    got = (size_t)ferror(f);
    fclose(f);
    return got ? -1 : 0;
}

HOST int HostState_WriteFile(const char *path, const void *data, unsigned int n)
{
    char tmp[4200];
    FILE *f;
    int ok;
    if (strlen(path) + 5 > sizeof(tmp)) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = HostFile_Open(tmp, "wb");
    if (!f) {
        return -1;
    }
    ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        HostFile_Remove(tmp);
        return -1;
    }
    HostFile_Remove(path);     /* Windows' rename does not replace */
    if (HostFile_Rename(tmp, path) != 0) {
        HostFile_Remove(tmp);
        return -1;
    }
    return 0;
}

HOST unsigned int HostState_Ms(void)
{
#ifdef _WIN32
    return (unsigned int)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}
