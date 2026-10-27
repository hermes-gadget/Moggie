/* OTA manifest: read, verify structure, verify signature.
 *
 * Wire format (one line, trailing newline allowed):
 *
 *   {"version":"1.0.0","model":8,"size":4123456,"sha256":"<64 hex>",
 *    "signature":"<base64 RSA-2048 sig>"}
 *
 * The signature covers the manifest bytes up to the comma before
 * `"signature"`, plus the object's closing brace — i.e. the canonical
 * serialisation of the other four fields.  Verification therefore needs no
 * JSON canonicaliser: hash the bytes we found, and check those bytes *are* the
 * canonical form of the values we parsed.
 */
#ifndef MOGGIE_MANIFEST_H
#define MOGGIE_MANIFEST_H

#include <stddef.h>

#include "rsa.h"

#define OTA_VERSION_MAX 64
#define OTA_MAX_IMAGE_BYTES 7864320ULL /* 7.5 MiB: half of mtd3, minus slack */
#define OTA_MODEL_ID 8u                /* Play:5 Gen 1 */

struct ota_manifest {
    char version[OTA_VERSION_MAX];
    unsigned model;
    unsigned long long size;
    unsigned char sha256[32];
    char sha256_hex[65];
};

enum manifest_status {
    MF_OK = 0,
    MF_ERR_IO = -1,        /* cannot read the file */
    MF_ERR_FORMAT = -2,    /* not the wire format (no signature member, etc.) */
    MF_ERR_FIELDS = -3,    /* bad/missing/duplicate/unknown field, bad type */
    MF_ERR_CANON = -4,     /* body is not the canonical form of its fields */
    MF_ERR_SIG = -5,       /* signature does not verify */
    MF_ERR_TOOBIG = -6     /* manifest file itself is absurdly large */
};

/* Loads and fully verifies `path` against `key`.  On failure returns a
 * negative manifest_status and, if `err` is non-NULL, writes a human-readable
 * reason into it. */
int ota_manifest_load(const char *path, const struct rsa_pub *key,
                      struct ota_manifest *out, char *err, size_t errlen);

const char *manifest_status_str(int status);

#endif /* MOGGIE_MANIFEST_H */
