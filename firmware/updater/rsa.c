#include "rsa.h"

#include <string.h>

/* A 2048-bit unsigned integer: 64 little-endian 32-bit limbs. */
#define BN_LIMBS (RSA_MODULUS_BYTES / 4)
#define BN_BITS (BN_LIMBS * 32)

struct bn {
    uint32_t l[BN_LIMBS];
};

static void bn_zero(struct bn *a)
{
    memset(a, 0, sizeof *a);
}

static void bn_from_be(struct bn *a, const unsigned char *be, size_t len)
{
    size_t i;

    bn_zero(a);
    if (len > RSA_MODULUS_BYTES)
        len = RSA_MODULUS_BYTES;
    for (i = 0; i < len; i++) {
        unsigned char byte = be[len - 1 - i];
        a->l[i / 4] |= (uint32_t)byte << (8 * (i % 4));
    }
}

static void bn_to_be(const struct bn *a, unsigned char out[RSA_MODULUS_BYTES])
{
    size_t i;

    for (i = 0; i < RSA_MODULUS_BYTES; i++) {
        unsigned limb = (unsigned)(i / 4);
        unsigned shift = 8 * (unsigned)(i % 4);
        out[RSA_MODULUS_BYTES - 1 - i] =
            (unsigned char)((a->l[limb] >> shift) & 0xffu);
    }
}

static int bn_cmp(const struct bn *a, const struct bn *b)
{
    int i;

    for (i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->l[i] != b->l[i])
            return a->l[i] > b->l[i] ? 1 : -1;
    }
    return 0;
}

/* r = a - b, assuming a >= b.  Aliasing r with a or b is fine. */
static void bn_sub(struct bn *r, const struct bn *a, const struct bn *b)
{
    uint32_t borrow = 0;
    int i;

    for (i = 0; i < BN_LIMBS; i++) {
        uint64_t sub = (uint64_t)a->l[i] - (uint64_t)b->l[i] - borrow;
        r->l[i] = (uint32_t)sub;
        borrow = (uint32_t)((sub >> 32) & 1u);
    }
}

/* r = a + b, returns the carry out of the top limb. */
static uint32_t bn_add(struct bn *r, const struct bn *a, const struct bn *b)
{
    uint32_t carry = 0;
    int i;

    for (i = 0; i < BN_LIMBS; i++) {
        uint64_t sum = (uint64_t)a->l[i] + b->l[i] + carry;
        r->l[i] = (uint32_t)sum;
        carry = (uint32_t)(sum >> 32);
    }
    return carry;
}

/* r = (a + b) mod n, for a, b < n.  a+b < 2n so one conditional subtract is
 * enough; when the addition carried out of 2048 bits the wrapped result minus
 * n is exactly (a+b)-n (proved by a+b < 2n). */
static void bn_addmod(struct bn *r, const struct bn *a, const struct bn *b,
                      const struct bn *n)
{
    uint32_t carry = bn_add(r, a, b);

    if (carry || bn_cmp(r, n) >= 0)
        bn_sub(r, r, n);
}

/* r = a*b mod n by double-and-add.  Slow in the abstract, but the firmware only
 * ever raises to 65537: 17 multiplies, each 2048 cheap limb passes. */
static void bn_modmul(struct bn *r, const struct bn *a, const struct bn *b,
                      const struct bn *n)
{
    struct bn acc;
    int i;

    bn_zero(&acc);
    for (i = BN_BITS - 1; i >= 0; i--) {
        bn_addmod(&acc, &acc, &acc, n);
        if (b->l[i / 32] & (1u << (i % 32)))
            bn_addmod(&acc, &acc, a, n);
    }
    *r = acc;
}

static void bn_modexp(struct bn *r, const struct bn *base, uint32_t e,
                      const struct bn *n)
{
    struct bn acc;
    int i;
    int top = 31;

    /* skip leading zero bits of the exponent */
    while (top > 0 && !(e & (1u << top)))
        top--;

    bn_zero(&acc);
    acc.l[0] = 1;
    for (i = top; i >= 0; i--) {
        bn_modmul(&acc, &acc, &acc, n);
        if (e & (1u << i))
            bn_modmul(&acc, &acc, base, n);
    }
    *r = acc;
}

int rsa_verify_sha256(const struct rsa_pub *key,
                      const unsigned char hash[32],
                      const unsigned char *sig,
                      size_t siglen)
{
    /* DigestInfo prefix for SHA-256 (RFC 8017 §9.2, note 2). */
    static const unsigned char sha256_digest_info[19] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    struct bn n, s, m;
    unsigned char em[RSA_MODULUS_BYTES];
    unsigned char want[RSA_MODULUS_BYTES];
    size_t pad_len;
    int nonzero = 0;
    int i;

    if (key == NULL || hash == NULL || sig == NULL)
        return -1;
    if (siglen != RSA_MODULUS_BYTES)
        return -1;
    for (i = 0; i < RSA_MODULUS_BYTES; i++)
        nonzero |= key->n[i];
    if (!nonzero)
        return -1;

    bn_from_be(&n, key->n, RSA_MODULUS_BYTES);
    bn_from_be(&s, sig, siglen);

    /* A signature integer at or above the modulus is invalid by definition. */
    if (bn_cmp(&s, &n) >= 0)
        return -1;

    bn_modexp(&m, &s, key->e, &n);
    bn_to_be(&m, em);

    /* EM = 0x00 || 0x01 || 0xFF * (>=8) || 0x00 || DigestInfo || H */
    pad_len = RSA_MODULUS_BYTES - 3 - sizeof sha256_digest_info - 32;
    if (pad_len < 8)
        return -1;
    memset(want, 0xff, sizeof want);
    want[0] = 0x00;
    want[1] = 0x01;
    want[2 + pad_len] = 0x00;
    memcpy(want + 3 + pad_len, sha256_digest_info, sizeof sha256_digest_info);
    memcpy(want + 3 + pad_len + sizeof sha256_digest_info, hash, 32);

    /* One comparison over the whole block: no partial-match early exit. */
    {
        unsigned diff = 0;
        for (i = 0; i < RSA_MODULUS_BYTES; i++)
            diff |= (unsigned)(em[i] ^ want[i]);
        return diff == 0 ? 0 : -1;
    }
}
