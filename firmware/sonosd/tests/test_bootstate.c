/*
 * test_bootstate.c -- the boot_ok commit that keeps the chooser from rolling
 * back a healthy speaker.
 *
 * Fixtures live in build/test underneath the source tree, never outside it.
 */

#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "tests.h"

#include "bootstate.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR "build/test/bootstate"

static void clean_dir(void)
{
    static const char *files[] = {
        "bootslot", "boot_ok", "boot_ok.tmp", NULL
    };
    char path[256];
    int i;

    (void)mkdir("build", 0755);
    (void)mkdir("build/test", 0755);
    (void)mkdir(DIR, 0755);

    for (i = 0; files[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", DIR, files[i]);
        unlink(path);
    }
}

static void put(const char *name, const char *content)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", DIR, name);
    f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static int exists(const char *name)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%s", DIR, name);
    return access(path, F_OK) == 0;
}

static void get(const char *name, char *out, size_t outsz)
{
    char path[256];
    FILE *f;

    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s", DIR, name);
    f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(out, (int)outsz, f))
        out[0] = '\0';
    fclose(f);
}

static void suite_commit(void)
{
    char slot[8];

    t_suite("bootstate: commit");

    clean_dir();
    put("bootslot", "a");
    T_EQ(BOOT_OK_OK, boot_ok_commit(DIR, slot, sizeof(slot)));
    T_STREQ("a", slot);
    T_ASSERT(exists("boot_ok"));

    clean_dir();
    put("bootslot", "b");
    T_EQ(BOOT_OK_OK, boot_ok_commit(DIR, slot, sizeof(slot)));
    T_STREQ("b", slot);

    /* The chooser writes the slot with no newline, but tolerate one: a trailing
     * newline parsed as part of the name would silently commit nothing. */
    clean_dir();
    put("bootslot", "a\n");
    T_EQ(BOOT_OK_OK, boot_ok_commit(DIR, slot, sizeof(slot)));
    T_STREQ("a", slot);

    /* Trailing whitespace is tolerated because a hand-edited or CRLF file is
     * plausible; the chooser itself strips \r\n before comparing. */
    clean_dir();
    put("bootslot", " b \r\n");
    T_EQ(BOOT_OK_BAD_SLOT, boot_ok_commit(DIR, slot, sizeof(slot)));
    T_ASSERT(!exists("boot_ok"));
}

static void suite_refusals(void)
{
    char slot[8];

    t_suite("bootstate: refusals");

    /* No bootslot at all: we cannot know which slot we are on, so we must not
     * write one. Committing the wrong slot defeats the rollback entirely. */
    clean_dir();
    T_EQ(BOOT_OK_NO_SLOT, boot_ok_commit(DIR, slot, sizeof(slot)));
    T_STREQ("", slot);
    T_ASSERT(!exists("boot_ok"));

    /* Garbage in bootslot must not be copied through. */
    clean_dir();
    put("bootslot", "z");
    T_EQ(BOOT_OK_BAD_SLOT, boot_ok_commit(DIR, slot, sizeof(slot)));
    T_ASSERT(!exists("boot_ok"));

    clean_dir();
    put("bootslot", "slot-a");
    T_EQ(BOOT_OK_BAD_SLOT, boot_ok_commit(DIR, slot, sizeof(slot)));
    T_ASSERT(!exists("boot_ok"));

    /* An empty state dir means "disabled", not "write to the cwd". */
    T_EQ(BOOT_OK_NO_STATE, boot_ok_commit("", slot, sizeof(slot)));
    T_EQ(BOOT_OK_NO_STATE, boot_ok_commit(NULL, slot, sizeof(slot)));
}

static void suite_shape(void)
{
    char slot[8];
    char content[16];

    t_suite("bootstate: on-disk shape");

    clean_dir();
    put("bootslot", "a");
    T_EQ(BOOT_OK_OK, boot_ok_commit(DIR, slot, sizeof(slot)));

    /* The chooser compares the file's contents to the slot name, so there must
     * be no trailing newline and nothing else in it. */
    get("boot_ok", content, sizeof(content));
    T_STREQ("a", content);

    /* The temporary file must not survive a successful commit. */
    T_ASSERT(!exists("boot_ok.tmp"));

    /* Committing twice is harmless and leaves the same answer. */
    T_EQ(BOOT_OK_OK, boot_ok_commit(DIR, slot, sizeof(slot)));
    get("boot_ok", content, sizeof(content));
    T_STREQ("a", content);

    /* A stale flag from the previous boot is replaced, not appended to. */
    put("boot_ok", "b");
    T_EQ(BOOT_OK_OK, boot_ok_commit(DIR, slot, sizeof(slot)));
    get("boot_ok", content, sizeof(content));
    T_STREQ("a", content);

    /* A caller with a tiny buffer still gets a terminated string. */
    slot[0] = '?';
    slot[1] = '?';
    T_EQ(BOOT_OK_OK, boot_ok_commit(DIR, slot, 1));
    T_STREQ("", slot);
}

void suite_bootstate(void)
{
    suite_commit();
    suite_refusals();
    suite_shape();
    clean_dir();
}
