/*
 * bootstate.c -- commit a successful boot to the initramfs chooser.
 *
 * See bootstate.h for why this exists. Short version: the chooser charges an
 * attempt before switch_root and only clears it when we write boot_ok. Without
 * this, a healthy speaker rolls back after two boots.
 */

#define _POSIX_C_SOURCE 200809L

#include "bootstate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SLOT_MAX 8

static void trim(char *s)
{
    size_t n;

    if (!s)
        return;
    n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
            break;
        s[--n] = '\0';
    }
}

/* fsync a directory so the rename itself is on the medium. */
static int fsync_dir(const char *dir)
{
    int fd;
    int rc;

    fd = open(dir, O_RDONLY);
    if (fd < 0)
        return -1;
    rc = fsync(fd);
    close(fd);
    return rc;
}

const char *boot_ok_strerror(int code)
{
    switch (code) {
    case BOOT_OK_OK:       return "ok";
    case BOOT_OK_NO_STATE: return "state directory unusable";
    case BOOT_OK_NO_SLOT:  return "bootslot missing or unreadable";
    case BOOT_OK_BAD_SLOT: return "bootslot is not a or b";
    case BOOT_OK_WRITE:    return "could not write boot_ok";
    default:               return "unknown";
    }
}

int boot_ok_commit(const char *state_dir, char *slot_out, size_t slot_out_sz)
{
    char slot_path[256];
    char ok_path[256];
    char tmp_path[256];
    char slot[SLOT_MAX];
    FILE *f;
    int fd;
    int n;

    if (slot_out && slot_out_sz)
        slot_out[0] = '\0';

    if (!state_dir || !*state_dir)
        return BOOT_OK_NO_STATE;

    n = snprintf(slot_path, sizeof(slot_path), "%s/bootslot", state_dir);
    if (n < 0 || (size_t)n >= sizeof(slot_path))
        return BOOT_OK_NO_STATE;

    f = fopen(slot_path, "r");
    if (!f)
        return BOOT_OK_NO_SLOT;

    if (!fgets(slot, sizeof(slot), f)) {
        fclose(f);
        return BOOT_OK_NO_SLOT;
    }
    fclose(f);

    trim(slot);

    if (strcmp(slot, "a") != 0 && strcmp(slot, "b") != 0)
        return BOOT_OK_BAD_SLOT;

    n = snprintf(ok_path, sizeof(ok_path), "%s/boot_ok", state_dir);
    if (n < 0 || (size_t)n >= sizeof(ok_path))
        return BOOT_OK_NO_STATE;

    n = snprintf(tmp_path, sizeof(tmp_path), "%s/boot_ok.tmp", state_dir);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return BOOT_OK_NO_STATE;

    /* Write to a temporary file, flush it to the medium, then rename. A rename
     * within one directory is atomic, so a power cut either leaves the old flag
     * or the new one — never a half-written slot name. */
    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return BOOT_OK_WRITE;

    if (write(fd, slot, strlen(slot)) != (ssize_t)strlen(slot)) {
        close(fd);
        unlink(tmp_path);
        return BOOT_OK_WRITE;
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return BOOT_OK_WRITE;
    }

    if (close(fd) != 0) {
        unlink(tmp_path);
        return BOOT_OK_WRITE;
    }

    if (rename(tmp_path, ok_path) != 0) {
        unlink(tmp_path);
        return BOOT_OK_WRITE;
    }

    /* Make the rename itself durable. Not fatal if it fails: the flag is
     * already in place, and the next boot's charge is cheap. */
    (void)fsync_dir(state_dir);

    if (slot_out && slot_out_sz) {
        size_t len = strlen(slot);
        if (len >= slot_out_sz)
            len = slot_out_sz - 1;
        memcpy(slot_out, slot, len);
        slot_out[len] = '\0';
    }

    return BOOT_OK_OK;
}
