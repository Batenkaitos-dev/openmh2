/*
 * SHA-1, written from FIPS 180-4 (the message schedule of 6.1.2, the functions and constants of 4.1.1 and 4.2.1, the
 * padding of 5.1.1). Nothing else was read for it.
 */
#include "sha1.h"

#include <string.h>

static uint32_t rol(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

static void compress(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int t = 0; t < 16; t++)
        w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
    for (int t = 16; t < 80; t++)
        w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20) {
            f = (b & c) ^ (~b & d);   /* Ch */
            k = 0x5a827999u;
        } else if (t < 40) {
            f = b ^ c ^ d;            /* Parity */
            k = 0x6ed9eba1u;
        } else if (t < 60) {
            f = (b & c) ^ (b & d) ^ (c & d);   /* Maj */
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        uint32_t tmp = rol(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xc3d2e1f0u;
    s->bytes = 0;
    s->fill = 0;
}

void sha1_update(Sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->bytes += len;
    if (s->fill) {
        size_t take = 64 - s->fill;
        if (take > len)
            take = len;
        memcpy(s->block + s->fill, p, take);
        s->fill += (unsigned)take;
        p += take;
        len -= take;
        if (s->fill < 64)
            return;
        compress(s->h, s->block);
        s->fill = 0;
    }
    for (; len >= 64; p += 64, len -= 64)
        compress(s->h, p);
    if (len) {
        memcpy(s->block, p, len);
        s->fill = (unsigned)len;
    }
}

void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->bytes * 8;
    uint8_t pad[72] = {0x80};
    size_t n = (s->fill < 56 ? 56 : 120) - s->fill;   /* up to 56 mod 64, then the length */
    for (int i = 0; i < 8; i++)
        pad[n + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, pad, n + 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}
