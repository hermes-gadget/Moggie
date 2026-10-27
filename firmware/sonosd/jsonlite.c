#define _POSIX_C_SOURCE 200809L

#include "jsonlite.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Find "key" followed by optional whitespace and a colon. Returns a pointer to
 * the first character of the value, or NULL. */
static const char *find_value(const char *json, const char *key)
{
    char pattern[48];
    const char *p;
    size_t keylen;

    if (!json || !key)
        return NULL;
    keylen = strlen(key);
    if (keylen == 0 || keylen + 3 > sizeof(pattern))
        return NULL;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = p + strlen(pattern);
        /* Reject a match inside a string value, e.g. {"name":"\"volume\""}. */
        while (*q && isspace((unsigned char)*q))
            q++;
        if (*q == ':') {
            q++;
            while (*q && isspace((unsigned char)*q))
                q++;
            return q;
        }
        p += strlen(pattern);
    }
    return NULL;
}

int json_get_int(const char *json, const char *key, int *out)
{
    const char *v = find_value(json, key);
    char *end = NULL;
    long n;

    if (!v)
        return 0;
    if (!*v)
        return -1;

    n = strtol(v, &end, 10);
    if (end == v)
        return -1;
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end != ',' && *end != '}' && *end != '\0')
        return -1;
    if (n < -2147483647L || n > 2147483647L)
        return -1;

    if (out)
        *out = (int)n;
    return 1;
}

int json_get_bool(const char *json, const char *key, int *out)
{
    const char *v = find_value(json, key);

    if (!v)
        return 0;

    if (strncmp(v, "true", 4) == 0) {
        if (out)
            *out = 1;
        return 1;
    }
    if (strncmp(v, "false", 5) == 0) {
        if (out)
            *out = 0;
        return 1;
    }
    /* Accept 0/1 too: hand-written MQTT payloads are common. */
    if (*v == '0' || *v == '1') {
        if (out)
            *out = (*v == '1');
        return 1;
    }
    return -1;
}

int json_get_str(const char *json, const char *key, char *out, size_t outsz)
{
    const char *v = find_value(json, key);
    size_t n = 0;

    if (!v)
        return 0;
    if (*v != '"')
        return -1;
    v++;

    if (!out || outsz == 0)
        return -1;

    while (*v && *v != '"') {
        char c = *v;

        if (c == '\\' && v[1]) {
            v++;
            switch (*v) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            default:  c = *v; break;
            }
        }
        if (n + 1 >= outsz)
            return -1;
        out[n++] = c;
        v++;
    }

    if (*v != '"')
        return -1;   /* unterminated */
    out[n] = '\0';
    return 1;
}
