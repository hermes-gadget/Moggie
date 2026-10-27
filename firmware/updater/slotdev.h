/* The A/B slot as a byte range that can be written and read back.
 *
 * Two backends:
 *
 *   device  - spawns `nandwrite` / `nanddump` (mtd-utils) for one 8 MB slot
 *             inside mtd3.  The image is streamed through a pipe in 64 KiB
 *             chunks, never buffered: 32 MB of RAM cannot hold an 8 MB image
 *             plus a decompressor plus everything else.
 *
 *   regular - a plain file standing in for mtd3 (build with -DMOGGIE_TEST_MODE
 *             and pass --target).  Same code path, pwrite/pread instead of a
 *             child process, so the whole flow is exercisable on x86 without a
 *             NAND device ever being touched.
 */
#ifndef MOGGIE_SLOTDEV_H
#define MOGGIE_SLOTDEV_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#define SLOT_BYTES (8ULL * 1024ULL * 1024ULL)
#define FLASH_ROOTFS_BYTES (2ULL * SLOT_BYTES)

enum slot_id {
    SLOT_A = 0,
    SLOT_B = 1
};

struct slotdev {
    int fd;                 /* regular: target fd; device: pipe to nandwrite */
    int regular;
    int child;              /* device: nandwrite/nanddump pid, else -1 */
    uint64_t base;          /* byte offset of this slot inside the device */
    uint64_t slot_bytes;
    char path[256];
};

/* Opens `path` and selects `slot`.  Returns 0 on success, -1 on failure with a
 * reason in `err`.  In a build without MOGGIE_TEST_MODE, a regular file is
 * refused outright. */
int slotdev_open(struct slotdev *d, const char *path, enum slot_id slot,
                 int test_mode, char *err, size_t errlen);

/* Streams exactly `size` bytes from `imgfd` into the slot, hashing as it goes.
 * Returns 0 on success; -1 if the image was short, the target refused a write,
 * or the writer process failed. */
int slotdev_write_stream(struct slotdev *d, int imgfd, uint64_t size,
                         unsigned char digest_out[SHA256_DIGEST_LEN],
                         char *err, size_t errlen);

/* Reads the first `size` bytes of the slot back and hashes them.  `size` is the
 * image length, not the slot length: nandwrite pads to the erase boundary with
 * 0xFF and the rest of the slot holds whatever was there before, so comparing
 * the whole slot against an image hash would never match.  Returns 0 on
 * success. */
int slotdev_read_hash(struct slotdev *d, uint64_t size,
                      unsigned char digest_out[SHA256_DIGEST_LEN],
                      char *err, size_t errlen);

void slotdev_close(struct slotdev *d);

#endif /* MOGGIE_SLOTDEV_H */
