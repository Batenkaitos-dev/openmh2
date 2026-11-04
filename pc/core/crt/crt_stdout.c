/*
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/* printf's stream (stdout) on PC: re-hosted from the earlier port's libc_stdout.c
 * (printf's stdout), which is taken from newlib 1.8.2's
 * libc/stdio/fvwrite.c (the line-buffered branch of __sfvwrite) and
 * fflush.c, adapted; listed in THIRD_PARTY.md.
 *
 * stdout is line buffered with a BUFSIZ (1024) buffer that the first printf
 * takes from the C library's heap (_malloc_r with the reent), and every
 * line, or a full buffer, is a write(1, ...) (crt_k_write: the IOP's fileio
 * write, which blocks). The stream's error flag, once a write has failed,
 * makes every later printf return EOF; the console's write (api/console.c)
 * returns the count offline, as the original's, so the flag stays clear. */
#include "crt_heap.h"
#include "crt_kernel.h"
#include "crt_stdout.h"

#define SO_BUFSIZ 1024 /* BUFSIZ */
#define SO_FD 1        /* stdout */

static struct {
    int setup;   /* the first printf has taken the buffer */
    char *base;  /* the buffer (NULL: the allocator failed) */
    u32 pend;    /* bytes waiting in it */
    int err;     /* the stream's error flag (a write failed) */
    int failed;  /* a write failed in the current printf: the rest is dropped */
} S;

void crt_stdout_state_reset(void)
{
    S.setup = 0;
    S.base = NULL;
    S.pend = 0;
    S.err = 0;
    S.failed = 0;
}

static int so_fflush(void)
{
    s32 n = (s32)S.pend;
    const char *p = S.base;
    S.pend = 0;
    while (n > 0) {
        s32 t = crt_k_write(SO_FD, p, (u32)n);
        if (t <= 0) {
            S.err = 1;
            return -1;
        }
        p += t;
        n -= t;
    }
    return 0;
}

static void so_copy(const char *p, u32 n)
{
    for (u32 i = 0; i < n; i++)
        S.base[S.pend + i] = p[i];
    S.pend += n;
}

void crt_so_begin(void)
{
    S.failed = 0;
    if (S.setup)
        return;
    S.setup = 1;
    S.base = (char *)(uintptr_t)crt_malloc_r(CRT_REENT, SO_BUFSIZ);
    if (!S.base)
        CRT_UNSEEN("printf", "the C library's allocator gave no buffer for stdout");
}

void crt_so_piece(const char *p, u32 len)
{
    if (S.failed || !len)
        return;
    if (!S.base)
        return; /* no buffer: noted at crt_so_begin */
    int nlknown = 0;
    u32 nldist = 0;
    while (len) {
        if (!nlknown) {
            const char *nl = NULL;
            for (u32 i = 0; i < len; i++) {
                if (p[i] == '\n') {
                    nl = p + i;
                    break;
                }
            }
            nldist = nl ? (u32)(nl + 1 - p) : len + 1;
            nlknown = 1;
        }
        u32 s = len < nldist ? len : nldist;
        u32 w = SO_BUFSIZ - S.pend;
        if (S.pend > 0 && s > w) {
            so_copy(p, w);
            if (so_fflush())
                goto err;
        } else if (s >= (w = SO_BUFSIZ)) {
            /* The C library writes these bytes from where they are; here
             * they go through the (empty) buffer: the same write. */
            CRT_UNSEEN("printf", "a piece of %u bytes without a newline", s);
            so_copy(p, w);
            if (so_fflush())
                goto err;
        } else {
            w = s;
            so_copy(p, w);
        }
        if ((nldist -= w) == 0) {
            /* copied the newline: flush and forget */
            if (so_fflush())
                goto err;
            nlknown = 0;
        }
        p += w;
        len -= w;
    }
    return;
err:
    S.err = 1;
    S.failed = 1;
}

/* What the C library's vfprintf returns: EOF once the stream's error flag
 * is set. */
int crt_so_end(void)
{
    return S.err;
}
