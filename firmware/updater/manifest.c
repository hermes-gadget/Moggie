#include "manifest.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "sha256.h"

#define MANIFEST_MAX_BYTES 8192
#define SIG_MEMBER ",\"signature\":\""
#define SIG_MEMBER_LEN (sizeof SIG_MEMBER - 1)

const char *manifest_status_str(int status)
{
    switch (status) {
    case MF_OK:          return "ok";
    case MF_ERR_IO:      return "cannot read manifest";
    case MF_ERR_FORMAT:  return "manifest is not in the expected wire format";
    case MF_ERR_FIELDS:  return "manifest fields are missing, duplicated or invalid";
    case MF_ERR_CANON:   return "manifest body is not canonical";
    case MF_ERR_SIG:     return "manifest signature does not verify";
    case MF_ERR_TOOBIG:  return "manifest file is too large";
    default:             return "unknown manifest error";
    }
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* --- a very small, very strict JSON reader ------------------------------- */

struct cur {
    const char *p;
    const char *end;
};

static int at_end(const struct cur *c)
{
    return c->p >= c->end;
}

static int expect(struct cur *c, char ch)
{
    if (at_end(c) || *c->p != ch)
        return -1;
    c->p++;
    return 0;
}

/* Strings: no escapes, printable ASCII only.  That is all the canonical form
 * can contain, and refusing escapes keeps the byte-for-byte comparison honest. */
static int parse_string(struct cur *c, char *out, size_t cap)
{
    size_t n = 0;

    if (expect(c, '"') != 0)
        return -1;
    while (!at_end(c) && *c->p != '"') {
        unsigned char ch = (unsigned char)*c->p;
        if (ch < 0x20 || ch > 0x7e || ch == '\\')
            return -1;
        if (n + 1 >= cap)
            return -1;
        out[n++] = (char)ch;
        c->p++;
    }
    if (expect(c, '"') != 0)
        return -1;
    out[n] = '\0';
    return 0;
}

/* Unsigned integers: digits only, no sign, no leading zeros (canonical JSON). */
static int parse_uint(struct cur *c, unsigned long long *out)
{
    unsigned long long v = 0;
    size_t digits = 0;

    if (at_end(c) || *c->p < '0' || *c->p > '9')
        return -1;
    if (*c->p == '0' && (c->p + 1) < c->end && c->p[1] >= '0' && c->p[1] <= '9')
        return -1;
    while (!at_end(c) && *c->p >= '0' && *c->p <= '9') {
        unsigned d = (unsigned)(*c->p - '0');
        if (v > (0xffffffffffffffffULL - d) / 10ULL)
            return -1;
        v = v * 10ULL + d;
        c->p++;
        digits++;
    }
    if (digits == 0 || digits > 20)
        return -1;
    *out = v;
    return 0;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1; /* uppercase is not canonical */
}

static int parse_sha256_hex(const char *hex, unsigned char out[32])
{
    int i;

    for (i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return hex[64] == '\0' ? 0 : -1;
}

/* Parses the four signed fields.  Rejects duplicates, unknowns and trailing
 * junk; key order is irrelevant here because the canonical check downstream
 * re-serialises and compares bytes. */
static int parse_body(const char *body, size_t len, struct ota_manifest *out,
                      char *err, size_t errlen)
{
    struct cur c;
    int seen_version = 0, seen_model = 0, seen_size = 0, seen_sha = 0;
    int done = 0;

    c.p = body;
    c.end = body + len;

    if (expect(&c, '{') != 0) {
        set_err(err, errlen, "manifest body does not start with '{'");
        return MF_ERR_FIELDS;
    }

    while (!done) {
        char key[32];

        if (parse_string(&c, key, sizeof key) != 0) {
            set_err(err, errlen, "expected a field name");
            return MF_ERR_FIELDS;
        }
        if (expect(&c, ':') != 0) {
            set_err(err, errlen, "expected ':' after \"%s\"", key);
            return MF_ERR_FIELDS;
        }

        if (strcmp(key, "version") == 0) {
            if (seen_version++) { set_err(err, errlen, "duplicate \"version\""); return MF_ERR_FIELDS; }
            if (parse_string(&c, out->version, sizeof out->version) != 0) {
                set_err(err, errlen, "\"version\" is not a plain string");
                return MF_ERR_FIELDS;
            }
        } else if (strcmp(key, "model") == 0) {
            unsigned long long v;
            if (seen_model++) { set_err(err, errlen, "duplicate \"model\""); return MF_ERR_FIELDS; }
            if (parse_uint(&c, &v) != 0 || v > 0xffffffffULL) {
                set_err(err, errlen, "\"model\" is not a valid integer");
                return MF_ERR_FIELDS;
            }
            out->model = (unsigned)v;
        } else if (strcmp(key, "size") == 0) {
            if (seen_size++) { set_err(err, errlen, "duplicate \"size\""); return MF_ERR_FIELDS; }
            if (parse_uint(&c, &out->size) != 0) {
                set_err(err, errlen, "\"size\" is not a valid integer");
                return MF_ERR_FIELDS;
            }
        } else if (strcmp(key, "sha256") == 0) {
            char hex[80];
            if (seen_sha++) { set_err(err, errlen, "duplicate \"sha256\""); return MF_ERR_FIELDS; }
            if (parse_string(&c, hex, sizeof hex) != 0 || parse_sha256_hex(hex, out->sha256) != 0) {
                set_err(err, errlen, "\"sha256\" is not 64 lowercase hex characters");
                return MF_ERR_FIELDS;
            }
            memcpy(out->sha256_hex, hex, 65);
        } else {
            set_err(err, errlen, "unknown field \"%s\"", key);
            return MF_ERR_FIELDS;
        }

        if (at_end(&c)) {
            set_err(err, errlen, "manifest body is truncated");
            return MF_ERR_FIELDS;
        }
        if (*c.p == ',') {
            c.p++;
            continue;
        }
        if (*c.p == '}') {
            c.p++;
            done = 1;
            break;
        }
        set_err(err, errlen, "expected ',' or '}' after \"%s\"", key);
        return MF_ERR_FIELDS;
    }

    if (!at_end(&c)) {
        set_err(err, errlen, "trailing data after manifest object");
        return MF_ERR_FIELDS;
    }
    if (!seen_version || !seen_model || !seen_size || !seen_sha) {
        set_err(err, errlen, "manifest body is missing one of version/model/size/sha256");
        return MF_ERR_FIELDS;
    }
    if (out->version[0] == '\0') {
        set_err(err, errlen, "\"version\" is empty");
        return MF_ERR_FIELDS;
    }
    return MF_OK;
}

/* Rebuild the canonical bytes and require an exact match. */
static int check_canonical(const char *body, size_t len,
                           const struct ota_manifest *m, char *err, size_t errlen)
{
    char *want;
    size_t want_len;
    int rc = MF_OK;

    want = (char *)malloc(len + 1);
    if (want == NULL) {
        set_err(err, errlen, "out of memory");
        return MF_ERR_IO;
    }
    want_len = (size_t)snprintf(want, len + 1, "{\"version\":\"%s\",\"model\":%u,"
                                "\"size\":%llu,\"sha256\":\"%s\"}",
                                m->version, m->model, m->size, m->sha256_hex);
    if (want_len != len || memcmp(want, body, len) != 0)
        rc = MF_ERR_CANON;
    free(want);
    if (rc != MF_OK)
        set_err(err, errlen, "manifest body is not in canonical form "
                "(whitespace, key order or escaping differ from the signed bytes)");
    return rc;
}

static int read_file(const char *path, char **buf, size_t *lenp,
                     char *err, size_t errlen)
{
    FILE *fh;
    long size;
    char *buf_;

    fh = fopen(path, "rb");
    if (fh == NULL) {
        set_err(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return MF_ERR_IO;
    }
    if (fseek(fh, 0, SEEK_END) != 0 || (size = ftell(fh)) < 0) {
        set_err(err, errlen, "cannot seek %s: %s", path, strerror(errno));
        fclose(fh);
        return MF_ERR_IO;
    }
    if (size == 0) {
        set_err(err, errlen, "manifest is empty: %s", path);
        fclose(fh);
        return MF_ERR_FORMAT;
    }
    if (size > MANIFEST_MAX_BYTES) {
        set_err(err, errlen, "manifest is %ld bytes (limit %d)", size, MANIFEST_MAX_BYTES);
        fclose(fh);
        return MF_ERR_TOOBIG;
    }
    rewind(fh);
    buf_ = (char *)malloc((size_t)size + 1);
    if (buf_ == NULL) {
        set_err(err, errlen, "out of memory");
        fclose(fh);
        return MF_ERR_IO;
    }
    if (fread(buf_, 1, (size_t)size, fh) != (size_t)size) {
        set_err(err, errlen, "short read on %s", path);
        free(buf_);
        fclose(fh);
        return MF_ERR_IO;
    }
    fclose(fh);
    buf_[size] = '\0';
    *buf = buf_;
    *lenp = (size_t)size;
    return MF_OK;
}

int ota_manifest_load(const char *path, const struct rsa_pub *key,
                      struct ota_manifest *out, char *err, size_t errlen)
{
    char *raw = NULL;
    size_t rawlen = 0;
    size_t end;
    size_t p;
    size_t body_len;
    char *body = NULL;
    unsigned char digest[32];
    unsigned char sig[RSA_MODULUS_BYTES];
    long siglen;
    struct sha256_ctx ctx;
    int rc;

    if (path == NULL || key == NULL || out == NULL)
        return MF_ERR_IO;

    memset(out, 0, sizeof *out);
    rc = read_file(path, &raw, &rawlen, err, errlen);
    if (rc != MF_OK)
        return rc;

    /* Trailing whitespace is allowed; nothing else is. */
    end = rawlen;
    while (end > 0 && (raw[end - 1] == '\n' || raw[end - 1] == '\r'
                       || raw[end - 1] == ' ' || raw[end - 1] == '\t'))
        end--;
    if (end < 3 || raw[end - 1] != '}' || raw[end - 2] != '"') {
        set_err(err, errlen, "manifest must end with the signature member: "
                ",\"signature\":\"<base64>\"}");
        free(raw);
        return MF_ERR_FORMAT;
    }

    /* Find the last occurrence of the signature marker, which must end exactly
     * at the closing quote of the final member. */
    if (end < 2 + SIG_MEMBER_LEN) {
        set_err(err, errlen, "manifest is too short to carry a signature");
        free(raw);
        return MF_ERR_FORMAT;
    }
    {
        size_t start = end - 2 - SIG_MEMBER_LEN;
        int found = 0;

        for (;;) {
            if (memcmp(raw + start, SIG_MEMBER, SIG_MEMBER_LEN) == 0) {
                found = 1;
                break;
            }
            if (start == 0)
                break;
            start--;
        }
        if (!found) {
            set_err(err, errlen, "manifest has no signature member");
            free(raw);
            return MF_ERR_FORMAT;
        }
        p = start;
    }

    body_len = p + 1; /* body + closing brace */
    body = (char *)malloc(body_len + 1);
    if (body == NULL) {
        set_err(err, errlen, "out of memory");
        free(raw);
        return MF_ERR_IO;
    }
    memcpy(body, raw, p);
    body[p] = '}';
    body[body_len] = '\0';

    if (strstr(body, "\"signature\"") != NULL) {
        set_err(err, errlen, "manifest has more than one signature member");
        free(body);
        free(raw);
        return MF_ERR_FORMAT;
    }

    rc = parse_body(body, body_len, out, err, errlen);
    if (rc != MF_OK)
        goto out;
    rc = check_canonical(body, body_len, out, err, errlen);
    if (rc != MF_OK)
        goto out;

    /* Hash the signed bytes, then the signature over them. */
    sha256_init(&ctx);
    sha256_update(&ctx, body, body_len);
    sha256_final(&ctx, digest);

    siglen = b64_decode(raw + p + SIG_MEMBER_LEN, end - 2 - (p + SIG_MEMBER_LEN),
                        sig, sizeof sig);
    if (siglen != (long)RSA_MODULUS_BYTES) {
        set_err(err, errlen, "signature is not a 256-byte RSA-2048 value");
        rc = MF_ERR_FORMAT;
        goto out;
    }
    if (rsa_verify_sha256(key, digest, sig, (size_t)siglen) != 0) {
        set_err(err, errlen, "signature does not verify against the built-in key");
        rc = MF_ERR_SIG;
        goto out;
    }

    rc = MF_OK;

out:
    free(body);
    free(raw);
    return rc;
}
