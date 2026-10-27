/*
 * util.h -- bounded string building, XML scraping and time helpers.
 *
 * There is no malloc here and there never should be: every builder writes into
 * a caller-owned buffer and latches an overflow flag instead of growing.
 */
#ifndef MOGGIE_DLNA_UTIL_H
#define MOGGIE_DLNA_UTIL_H

#include <stddef.h>
#include <stdint.h>

struct sbuf {
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
};

void sb_init(struct sbuf *b, char *buf, size_t cap);
void sb_putc(struct sbuf *b, char c);
void sb_puts(struct sbuf *b, const char *s);
void sb_putn(struct sbuf *b, const char *s, size_t n);
void sb_printf(struct sbuf *b, const char *fmt, ...);
void sb_escape(struct sbuf *b, const char *s);   /* XML text/attribute escape */
int  sb_ok(const struct sbuf *b);

/* Decode XML entities into out. Returns 0, or -1 when the value does not fit
 * (out is still NUL terminated and truncated at a character boundary). */
int  xml_unescape(const char *in, char *out, size_t outsz);
int  xml_unescape_n(const char *in, size_t inlen, char *out, size_t outsz);

/* Find <tag>...</tag> (namespace prefixes on the tag are accepted), decode
 * entities into out. Returns 0 on success, -1 when missing or oversized. */
int  xml_get(const char *xml, const char *tag, char *out, size_t outsz);
int  xml_get_int(const char *xml, const char *tag, long *out);
int  xml_get_bool(const char *xml, const char *tag, int *out);

/* "H:MM:SS", "HH:MM:SS" and optional ".mmm". */
int  parse_hms(const char *s, unsigned *seconds);
void format_hms(char *out, size_t outsz, unsigned seconds);

/* First duration="..." attribute in a DIDL-Lite document. 0 if absent. */
unsigned parse_didl_duration(const char *metadata);

int64_t now_ms(void);
void    trim_ws(char *s);
int     str_ieq(const char *a, const char *b);
int     header_value(const char *value, char *out, size_t outsz);

#endif /* MOGGIE_DLNA_UTIL_H */
