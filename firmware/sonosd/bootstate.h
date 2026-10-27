/*
 * bootstate.h -- commit a successful boot.
 *
 * The initramfs chooser (firmware/bootchooser/init) charges a boot attempt to
 * the current slot *before* switch_root, and only clears that charge when
 * userspace writes the slot's name into <state_dir>/boot_ok. Two uncommitted
 * attempts and the chooser falls back to the other slot.
 *
 * Which means: if the daemon never writes boot_ok, a perfectly healthy speaker
 * rolls itself back on the third boot. This module is that write.
 *
 * It is deliberately narrow. It reads the slot from <state_dir>/bootslot, which
 * the chooser owns, and refuses to invent one: writing "a" when the chooser
 * booted "b" would commit the wrong slot and defeat the whole rollback story.
 */
#ifndef SONOSD_BOOTSTATE_H
#define SONOSD_BOOTSTATE_H

#include <stddef.h>

#define BOOT_OK_OK          0
#define BOOT_OK_NO_STATE   -1   /* state dir unusable                      */
#define BOOT_OK_NO_SLOT    -2   /* bootslot missing or unreadable          */
#define BOOT_OK_BAD_SLOT   -3   /* bootslot is not "a" or "b"              */
#define BOOT_OK_WRITE      -4   /* could not write boot_ok                 */

/*
 * Read the current slot from state_dir/bootslot and, if it is valid, write the
 * same value to state_dir/boot_ok via a temporary file plus rename, so a power
 * cut cannot leave a half-written flag.
 *
 * If slot_out is non-NULL it receives the committed slot ("a"/"b") on
 * BOOT_OK_OK, or an empty string otherwise.
 *
 * Returns BOOT_OK_* above.
 */
int boot_ok_commit(const char *state_dir, char *slot_out, size_t slot_out_sz);

/* The reason string for a BOOT_OK_* code, for logging. */
const char *boot_ok_strerror(int code);

#endif /* SONOSD_BOOTSTATE_H */
