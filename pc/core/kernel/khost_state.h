#ifndef MH2_KERNEL_KHOST_STATE_H
#define MH2_KERNEL_KHOST_STATE_H

/* The save states' part of the host seam (host_state.c; host.h's convention: cdecl): the image's layout, the
 * compressor, the file and the clock. */
#include "host.h"

/* A growing block of the C heap. */
typedef struct HostBuf {
    unsigned char *p;
    unsigned int n, cap;
} HostBuf;
HOST int HostBuf_Append(HostBuf *b, const void *data, unsigned int n);   /* 0 when good */
HOST void HostBuf_Free(HostBuf *b);

#define HOSTSTATE_RANGES 16
typedef struct HostStateLayout {
    unsigned int dataLo[HOSTSTATE_RANGES], dataHi[HOSTSTATE_RANGES];   /* the image's writable data, .data and .bss */
    int nData;
    unsigned int hostLo[HOSTSTATE_RANGES], hostHi[HOSTSTATE_RANGES];   /* inside it: the sections a state skips */
    int nHost;
    unsigned int codeLo, codeHi;                                       /* the image's code */
} HostStateLayout;
HOST void HostState_Layout(HostStateLayout *l);

/* Appends the zlib-deflated (raw deflate, no wrapper) bytes to out; returns their number, or -1. */
HOST int HostState_Deflate(const void *in, unsigned int n, HostBuf *out, int level);
/* Inflates exactly outN bytes; 0 when good. */
HOST int HostState_Inflate(const void *in, unsigned int n, void *out, unsigned int outN);
HOST unsigned int HostState_Adler(const void *data, unsigned int n);

HOST int HostState_ReadFile(const char *path, HostBuf *out);               /* 0 when good */
HOST int HostState_WriteFile(const char *path, const void *data, unsigned int n);
HOST unsigned int HostState_Ms(void);                                       /* milliseconds, for the log only */

#endif
