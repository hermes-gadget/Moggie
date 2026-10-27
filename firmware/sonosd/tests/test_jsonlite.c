/*
 * test_jsonlite.c -- the command-payload reader.
 */
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "tests.h"

#include "jsonlite.h"

#include <string.h>

static void case_ints(void)
{
    int v = 0;
    const char *doc = "{\"volume\":42,\"bass\":-3,\"treble\":0,\"big\":2147483647}";

    t_suite("jsonlite: integers");

    T_EQ(1, json_get_int(doc, "volume", &v));
    T_EQ(42, v);
    T_EQ(1, json_get_int(doc, "bass", &v));
    T_EQ(-3, v);
    T_EQ(1, json_get_int(doc, "treble", &v));
    T_EQ(0, v);
    T_EQ(1, json_get_int(doc, "big", &v));
    T_EQ(2147483647L, v);

    T_EQ(0, json_get_int(doc, "missing", &v));
    T_EQ(0, json_get_int("", "volume", &v));
    T_EQ(0, json_get_int(NULL, "volume", &v));
    T_EQ(0, json_get_int(doc, NULL, &v));

    /* A NULL out-pointer still reports presence. */
    T_EQ(1, json_get_int(doc, "volume", NULL));
}

static void case_bools(void)
{
    int v = -1;

    t_suite("jsonlite: booleans");

    T_EQ(1, json_get_bool("{\"mute\":true}", "mute", &v));
    T_EQ(1, v);

    T_EQ(1, json_get_bool("{\"mute\":false}", "mute", &v));
    T_EQ(0, v);

    T_EQ(1, json_get_bool("{\"mute\":1}", "mute", &v));
    T_EQ(1, v);

    T_EQ(1, json_get_bool("{\"mute\":0}", "mute", &v));
    T_EQ(0, v);

    T_EQ(0, json_get_bool("{\"mute\":true}", "loudness", &v));
    T_EQ(-1, json_get_bool("{\"mute\":\"yes\"}", "mute", &v));
}

static void case_strings(void)
{
    char s[32];

    t_suite("jsonlite: strings");

    T_EQ(1, json_get_str("{\"cmd\":\"volume_up\"}", "cmd", s, sizeof(s)));
    T_STREQ("volume_up", s);

    T_EQ(1, json_get_str("{\"cmd\":\"mute_toggle\"}", "cmd", s, sizeof(s)));
    T_STREQ("mute_toggle", s);

    /* Escapes */
    T_EQ(1, json_get_str("{\"cmd\":\"a\\\"b\"}", "cmd", s, sizeof(s)));
    T_STREQ("a\"b", s);

    /* Not a string */
    T_EQ(-1, json_get_str("{\"cmd\":42}", "cmd", s, sizeof(s)));

    /* Unterminated */
    T_EQ(-1, json_get_str("{\"cmd\":\"oops}", "cmd", s, sizeof(s)));

    /* Does not fit: must fail rather than truncate silently */
    T_EQ(-1, json_get_str("{\"cmd\":\"0123456789012345678901234567890123456789\"}",
                          "cmd", s, sizeof(s)));

    T_EQ(0, json_get_str("{\"cmd\":\"x\"}", "other", s, sizeof(s)));
}

static void case_structure(void)
{
    int v = 0;
    const char *doc = "{\"a\":{\"volume\":9},\"volume\":5}";

    t_suite("jsonlite: structure");

    /* First match wins. Flat command payloads only; documented limitation. */
    T_EQ(1, json_get_int(doc, "volume", &v));
    T_EQ(9, v);

    /* Whitespace around the colon and value. */
    T_EQ(1, json_get_int("{ \"volume\" :  7 }", "volume", &v));
    T_EQ(7, v);

    /* Key must be followed by a colon, not appear as a value. */
    T_EQ(0, json_get_int("{\"name\":\"volume\"}", "volume", &v));
}

void suite_jsonlite(void)
{
    case_ints();
    case_bools();
    case_strings();
    case_structure();
}
