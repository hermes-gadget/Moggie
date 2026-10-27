#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------- sbuf ---------------------------------------------------------- */

void sb_init(struct sbuf *b, char *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = 0;
    if (cap > 0)
        buf[0] = '\0';
}

void sb_putc(struct sbuf *b, char c)
{
    if (b->overflow)
        return;
    if (b->len + 1 >= b->cap) {
        b->overflow = 1;
        return;
    }
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}

void sb_putn(struct sbuf *b, const char *s, size_t n)
{
    size_t room;
    size_t take;

    if (b->overflow || n == 0)
        return;
    room = b->cap - b->len;
    if (room <= 1) {
        b->overflow = 1;
        return;
    }
    take = (n < room - 1) ? n : room - 1;
    memcpy(b->buf + b->len, s, take);
    b->len += take;
    b->buf[b->len] = '\0';
    if (take < n)
        b->overflow = 1;
}

void sb_puts(struct sbuf *b, const char *s)
{
    if (s)
        sb_putn(b, s, strlen(s));
}

void sb_printf(struct sbuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room;

    if (b->overflow)
        return;

    room = b->cap - b->len;
    if (room <= 1) {
        b->overflow = 1;
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(b->buf + b->len, room, fmt, ap);
    va_end(ap);

    if (n < 0) {
        b->overflow = 1;
        return;
    }
    if ((size_t)n >= room) {
        b->len = b->cap - 1;
        b->overflow = 1;
        return;
    }
    b->len += (size_t)n;
}

static void sb_escape_char(struct sbuf *b, unsigned char c)
{
    switch (c) {
    case '&':  sb_puts(b, "&amp;");  break;
    case '<':  sb_puts(b, "&lt;");   break;
    case '>':  sb_puts(b, "&gt;");   break;
    case '"':  sb_puts(b, "&quot;"); break;
    case '\'': sb_puts(b, "&apos;"); break;
    case '\r':
    case '\n': sb_puts(b, " ");      break;  /* never split an XML attribute */
    default:   sb_putc(b, (char)c);  break;
    }
}

void sb_escape(struct sbuf *b, const char *s)
{
    if (!s)
        return;
    while (*s) {
        sb_escape_char(b, (unsigned char)*s);
        s++;
    }
}

int sb_ok(const struct sbuf *b)
{
    return !b->overflow;
}

/* -------- XML ----------------------------------------------------------- */

static int utf8_put(unsigned long v, char *out, size_t outsz, size_t *used)
{
    unsigned char tmp[4];
    size_t n;
    size_t i;

    if (v < 0x80) {
        tmp[0] = (unsigned char)v;
        n = 1;
    } else if (v < 0x800) {
        tmp[0] = (unsigned char)(0xc0 | (v >> 6));
        tmp[1] = (unsigned char)(0x80 | (v & 0x3f));
        n = 2;
    } else if (v < 0x10000) {
        tmp[0] = (unsigned char)(0xe0 | (v >> 12));
        tmp[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3f));
        tmp[2] = (unsigned char)(0x80 | (v & 0x3f));
        n = 3;
    } else {
        tmp[0] = (unsigned char)(0xf0 | (v >> 18));
        tmp[1] = (unsigned char)(0x80 | ((v >> 12) & 0x3f));
        tmp[2] = (unsigned char)(0x80 | ((v >> 6) & 0x3f));
        tmp[3] = (unsigned char)(0x80 | (v & 0x3f));
        n = 4;
    }

    if (*used + n >= outsz)
        return -1;
    for (i = 0; i < n; i++)
        out[(*used)++] = (char)tmp[i];
    return 0;
}

/* Decode one entity. Returns 0 when decoded, 1 when this is not a known
 * entity (the caller copies the '&' literally), -1 on output overflow. */
static int entity_decode(const char **pp, const char *end,
                         char *out, size_t outsz, size_t *used)
{
    const char *p = *pp;
    const char *q;
    char *num_end;
    unsigned long v;
    size_t remain = (size_t)(end - p);
    int matched = 0;

    if (remain >= 5 && memcmp(p, "&amp;", 5) == 0) {
        v = '&';
        p += 5;
        matched = 1;
    } else if (remain >= 4 && memcmp(p, "&lt;", 4) == 0) {
        v = '<';
        p += 4;
        matched = 1;
    } else if (remain >= 4 && memcmp(p, "&gt;", 4) == 0) {
        v = '>';
        p += 4;
        matched = 1;
    } else if (remain >= 6 && memcmp(p, "&quot;", 6) == 0) {
        v = '"';
        p += 6;
        matched = 1;
    } else if (remain >= 6 && memcmp(p, "&apos;", 6) == 0) {
        v = '\'';
        p += 6;
        matched = 1;
    } else if (remain >= 4 && p[1] == '#') {
        int base = 10;

        q = p + 2;
        if (q < end && (*q == 'x' || *q == 'X')) {
            base = 16;
            q++;
        }
        if (q >= end || !isxdigit((unsigned char)*q))
            return 1;
        v = strtoul(q, &num_end, base);
        if (num_end >= end || *num_end != ';' || v == 0 || v > 0x10ffffUL)
            return 1;
        p = num_end + 1;
        matched = 1;
    }

    if (!matched)
        return 1;
    if (utf8_put(v, out, outsz, used) != 0)
        return -1;
    *pp = p;
    return 0;
}

int xml_unescape_n(const char *in, size_t inlen, char *out, size_t outsz)
{
    const char *p = in;
    const char *end = in + inlen;
    size_t used = 0;
    int rc = 0;

    if (outsz == 0)
        return -1;

    while (p < end) {
        if (*p == '&') {
            int r = entity_decode(&p, end, out, outsz, &used);
            if (r < 0) {
                rc = -1;
                break;
            }
            if (r == 1) {
                if (used + 1 >= outsz) {
                    rc = -1;
                    break;
                }
                out[used++] = *p++;
            }
        } else {
            if (used + 1 >= outsz) {
                rc = -1;
                break;
            }
            out[used++] = *p++;
        }
    }
    out[used] = '\0';
    return rc;
}

int xml_unescape(const char *in, char *out, size_t outsz)
{
    if (!in)
        return -1;
    return xml_unescape_n(in, strlen(in), out, outsz);
}

/* Return the local name length of a tag that starts at p (after '<').
 * Accepts "u:Tag". */
static int tag_local(const char *p, const char **local, size_t *len)
{
    const char *start = p;
    const char *colon = NULL;

    while (*p && *p != '>' && *p != '/' && !isspace((unsigned char)*p)) {
        if (*p == ':')
            colon = p;
        p++;
    }
    if (p == start)
        return -1;
    *local = colon ? colon + 1 : start;
    *len = (size_t)(p - *local);
    return 0;
}

int xml_get(const char *xml, const char *tag, char *out, size_t outsz)
{
    size_t taglen = strlen(tag);
    const char *p = xml;

    if (outsz == 0)
        return -1;
    out[0] = '\0';

    while (p && (p = strchr(p, '<')) != NULL) {
        const char *local;
        size_t len;
        const char *q;

        p++;
        if (*p == '/' || *p == '!' || *p == '?') {
            p++;
            continue;
        }
        if (tag_local(p, &local, &len) != 0) {
            p++;
            continue;
        }
        if (len != taglen || strncmp(local, tag, taglen) != 0) {
            p++;
            continue;
        }

        /* Skip attributes, honour an empty element. */
        q = p;
        while (*q && *q != '>')
            q++;
        if (*q != '>') {
            p++;
            continue;
        }
        if (q > p && q[-1] == '/') {
            return 0;
        }
        q++;

        {
            const char *content = q;
            const char *close = strchr(content, '<');
            const char *cp;
            const char *clocal;
            size_t clen;

            if (!close) {
                p = content;
                continue;
            }
            /* The value itself must not contain unescaped markup. */
            cp = close + 1;
            if (*cp != '/') {
                p = close;
                continue;
            }
            cp++;
            if (tag_local(cp, &clocal, &clen) != 0 ||
                clen != taglen || strncmp(clocal, tag, taglen) != 0) {
                p = close;
                continue;
            }

            return xml_unescape_n(content, (size_t)(close - content),
                                  out, outsz);
        }
    }
    return -1;
}

int xml_get_int(const char *xml, const char *tag, long *out)
{
    char tmp[32];
    char *end;
    long v;

    if (xml_get(xml, tag, tmp, sizeof(tmp)) != 0)
        return -1;
    trim_ws(tmp);
    if (!tmp[0])
        return -1;
    v = strtol(tmp, &end, 10);
    if (*end != '\0')
        return -1;
    *out = v;
    return 0;
}

int xml_get_bool(const char *xml, const char *tag, int *out)
{
    char tmp[16];

    if (xml_get(xml, tag, tmp, sizeof(tmp)) != 0)
        return -1;
    trim_ws(tmp);
    if (str_ieq(tmp, "1") || str_ieq(tmp, "true") ||
        str_ieq(tmp, "yes") || str_ieq(tmp, "on")) {
        *out = 1;
        return 0;
    }
    if (str_ieq(tmp, "0") || str_ieq(tmp, "false") ||
        str_ieq(tmp, "no") || str_ieq(tmp, "off")) {
        *out = 0;
        return 0;
    }
    return -1;
}

/* -------- time ---------------------------------------------------------- */

int parse_hms(const char *s, unsigned *seconds)
{
    unsigned h = 0, m = 0, sec = 0;
    const char *p;
    char *end;

    if (!s)
        return -1;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!isdigit((unsigned char)*s))
        return -1;

    p = s;
    h = (unsigned)strtoul(p, &end, 10);
    if (*end != ':')
        return -1;
    p = end + 1;
    if (!isdigit((unsigned char)*p))
        return -1;
    m = (unsigned)strtoul(p, &end, 10);
    if (*end != ':')
        return -1;
    p = end + 1;
    if (!isdigit((unsigned char)*p))
        return -1;
    sec = (unsigned)strtoul(p, &end, 10);

    /* optional fractional seconds */
    if (*end == '.') {
        end++;
        while (isdigit((unsigned char)*end))
            end++;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return -1;
    if (m > 59 || sec > 59)
        return -1;
    if (h > 999999u)
        return -1;

    *seconds = h * 3600u + m * 60u + sec;
    return 0;
}

void format_hms(char *out, size_t outsz, unsigned seconds)
{
    if (outsz == 0)
        return;
    snprintf(out, outsz, "%u:%02u:%02u",
             seconds / 3600u, (seconds / 60u) % 60u, seconds % 60u);
}

unsigned parse_didl_duration(const char *metadata)
{
    static const char key[] = "duration=\"";
    const char *p;
    unsigned value = 0;

    if (!metadata)
        return 0;

    p = strstr(metadata, key);
    if (!p)
        return 0;
    p += sizeof(key) - 1;

    {
        char tmp[32];
        size_t n = 0;

        while (*p && *p != '"' && n + 1 < sizeof(tmp))
            tmp[n++] = *p++;
        tmp[n] = '\0';
        if (parse_hms(tmp, &value) != 0)
            return 0;
    }
    return value;
}

int64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* -------- strings ------------------------------------------------------- */

void trim_ws(char *s)
{
    char *start = s;
    size_t n;

    if (!s)
        return;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

int str_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Copy a header value, stripping surrounding whitespace and one pair of
 * double quotes. Returns 0 on success, -1 if it does not fit. */
int header_value(const char *value, char *out, size_t outsz)
{
    size_t n;

    if (!value || outsz == 0)
        return -1;
    while (*value == ' ' || *value == '\t')
        value++;
    n = strlen(value);
    while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t' ||
                     value[n - 1] == '\r' || value[n - 1] == '\n'))
        n--;
    if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
        value++;
        n -= 2;
    }
    if (n >= outsz)
        return -1;
    memcpy(out, value, n);
    out[n] = '\0';
    return 0;
}
