#ifndef MH2_SHA1_H
#define MH2_SHA1_H

/* SHA-1, written from FIPS 180-4 (section 6.1): the archive maker's check of each disc file against config/disc.sha1. */
#include <stddef.h>
#include <stdint.h>

typedef struct Sha1 {
    uint32_t h[5];
    uint64_t bytes;      /* the message's length so far */
    uint8_t block[64];
    unsigned fill;       /* bytes in block */
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, size_t len);
void sha1_final(Sha1 *s, uint8_t out[20]);

#endif
