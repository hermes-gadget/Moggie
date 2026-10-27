/*
 * jsonlite.h -- just enough JSON to read a command payload.
 *
 * The daemon never parses a document: it reads flat key/value pairs out of
 * {"volume":42,"mute":true}. A full parser would cost flash and RAM we do not
 * have, and a hostile payload only needs to be rejected, not understood.
 */
#ifndef SONOSD_JSONLITE_H
#define SONOSD_JSONLITE_H

#include <stddef.h>

/* All return 1 when the key was found and parsed, 0 when absent, -1 when the
 * value is malformed. Keys are matched exactly, including quotes. */
int json_get_int(const char *json, const char *key, int *out);
int json_get_bool(const char *json, const char *key, int *out);
int json_get_str(const char *json, const char *key, char *out, size_t outsz);

#endif /* SONOSD_JSONLITE_H */
