/* SHA-256 (FIPS 180-4), small and dependency-free.
 *
 * The target is a 300 MHz PowerPC with 32 MB of RAM: no libcrypto, no malloc
 * churn, one struct on the stack. */
#ifndef MOGGIE_SHA256_H
#define MOGGIE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32

struct sha256_ctx {
    uint32_t h[8];
    uint64_t total;
    unsigned char buf[64];
    size_t buflen;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, unsigned char out[SHA256_DIGEST_LEN]);
void sha256_hex(const unsigned char digest[SHA256_DIGEST_LEN], char out[65]);

#endif /* MOGGIE_SHA256_H */
