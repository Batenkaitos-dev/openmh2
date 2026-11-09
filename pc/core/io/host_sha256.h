#ifndef MH2_IO_HOST_SHA256_H
#define MH2_IO_HOST_SHA256_H

/* SHA-256 (FIPS 180-4) for the platform layer's files (a pack's hash, host_disc.c). Host code: system headers only. */
#include <stddef.h>
#include <stdint.h>

typedef struct HostSha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint32_t fill;
    unsigned char buf[64];
} HostSha256;

void HostSha256_Init(HostSha256 *s);
void HostSha256_Update(HostSha256 *s, const void *data, size_t len);
/* The digest as 64 lower-case hex digits and a NUL. */
void HostSha256_Final(HostSha256 *s, char hex[65]);

#endif
