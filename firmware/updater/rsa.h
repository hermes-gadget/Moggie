/* RSA-2048 PKCS#1 v1.5 signature verification with SHA-256.
 *
 * The device only ever needs the *public* operation, so this file carries a
 * small fixed-width bignum and no secret-dependent branching.  It is not a
 * general-purpose crypto library: 2048-bit modulus, 32-bit exponent, one job.
 */
#ifndef MOGGIE_RSA_H
#define MOGGIE_RSA_H

#include <stddef.h>
#include <stdint.h>

#define RSA_MODULUS_BYTES 256

struct rsa_pub {
    unsigned char n[RSA_MODULUS_BYTES]; /* big-endian modulus */
    uint32_t e;                        /* public exponent (65537) */
};

/* Returns 0 if `sig` is a valid PKCS#1 v1.5 / SHA-256 signature over `hash`
 * (a 32-byte SHA-256 digest), -1 otherwise. */
int rsa_verify_sha256(const struct rsa_pub *key,
                      const unsigned char hash[32],
                      const unsigned char *sig,
                      size_t siglen);

#endif /* MOGGIE_RSA_H */
