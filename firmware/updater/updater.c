/* moggie-updater — apply a signed OTA image to the inactive rootfs slot.
 *
 * Order of operations (each step only starts if the previous one passed):
 *
 *   1. verify the manifest signature against the key compiled into firmware
 *   2. check model, size and the squashfs magic of the image
 *   3. select the slot that is NOT the one /jffs/bootslot points at
 *   4. mark that slot "writing" in /jffs and stream the image into it
 *      (nandwrite, 64 KiB chunks — the image is never held in RAM)
 *   5. hash what we wrote; abort if it differs from the manifest
 *   6. read the slot back with nanddump and hash it again; abort if it differs
 *   7. write slot_sha256.<slot>, slot_status.<slot>=ok, then /jffs/bootslot
 *   8. sync and reboot
 *
 * Invariants: mtd0 is never opened, the running slot is never written, and
 * bootslot only changes after a verified read-back.
 *
 * Exit codes: 0 ok, 1 usage, 2 manifest, 3 image, 4 device, 5 read-back,
 *             6 state, 7 write failure.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "manifest.h"
#include "rsa.h"
#include "sha256.h"
#include "slotdev.h"

#define EX_OK 0
#define EX_USAGE 1
#define EX_MANIFEST 2
#define EX_IMAGE 3
#define EX_DEVICE 4
#define EX_READBACK 5
#define EX_STATE 6
#define EX_WRITE 7

#define DEFAULT_DEVICE "/dev/mtd3"
#define DEFAULT_STATE_DIR "/jffs"
#define STATE_BOOTSLOT "bootslot"
/* Which slot the chooser actually booted. Distinct from `bootslot`, which means
 * "boot this one next" and is rewritten at commit time -- so reading bootslot
 * as the running slot lets a second update target the slot that is still
 * running. The chooser writes this file immediately before switch_root. */
#define STATE_RUNNING_SLOT "running_slot"
#define STATE_BOOT_OK "boot_ok"
#define SQUASHFS_MAGIC "hsqs"

/* Embedded by default; the test build can load a throwaway key instead, and
 * release builds point MOGGIE_OTA_PUBKEY_HEADER at the real generated header. */
#ifdef MOGGIE_OTA_PUBKEY_HEADER
#include MOGGIE_OTA_PUBKEY_HEADER
#else
#include "pubkey_dev.h"
#endif

static int verbose;

static void logmsg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void vlog(const char *fmt, ...)
{
    va_list ap;

    if (!verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* --- /jffs state files ---------------------------------------------------- */

static int state_path(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

/* Reads a small state file, trimming trailing whitespace.  Returns 0 if the
 * file exists and was read, -1 otherwise. */
static int state_read(const char *dir, const char *name, char *buf, size_t cap)
{
    char path[512];
    FILE *fh;
    size_t n;

    if (state_path(path, sizeof path, dir, name) != 0)
        return -1;
    fh = fopen(path, "rb");
    if (fh == NULL)
        return -1;
    n = fread(buf, 1, cap - 1, fh);
    fclose(fh);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'
                     || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        n--;
    buf[n] = '\0';
    return 0;
}

/* Atomic replace: write a temp file, fsync it, rename, fsync the directory.
 * A power cut leaves either the old contents or the new ones, never a mix. */
static int state_write(const char *dir, const char *name, const char *text)
{
    char path[512], tmp[544];
    int fd, dfd;
    size_t len = strlen(text);
    size_t off = 0;

    if (state_path(path, sizeof path, dir, name) != 0)
        return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        logmsg("moggie-updater: cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            logmsg("moggie-updater: cannot write %s: %s", tmp, strerror(errno));
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) {
        logmsg("moggie-updater: fsync %s: %s", tmp, strerror(errno));
        close(fd);
        return -1;
    }
    if (close(fd) != 0) {
        logmsg("moggie-updater: close %s: %s", tmp, strerror(errno));
        return -1;
    }
    if (rename(tmp, path) != 0) {
        logmsg("moggie-updater: rename %s: %s", path, strerror(errno));
        return -1;
    }
    dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return 0;
}

static int state_write_slot(const char *dir, const char *prefix, enum slot_id slot,
                            const char *text)
{
    char name[64];
    snprintf(name, sizeof name, "%s.%c", prefix, slot == SLOT_B ? 'b' : 'a');
    return state_write(dir, name, text);
}

/* --- keys ----------------------------------------------------------------- */

static void key_from_embedded(struct rsa_pub *key)
{
    memcpy(key->n, MOGGIE_OTA_PUBKEY_N, RSA_MODULUS_BYTES);
    key->e = MOGGIE_OTA_PUBKEY_E;
}

#ifdef MOGGIE_TEST_MODE
/* Test-only: n=<hex> / e=<dec>, two lines.  Keeps ASN.1 parsing out of the
 * firmware; the production path embeds the modulus as a C array. */
static int key_from_text(const char *path, struct rsa_pub *key, char *err, size_t errlen)
{
    FILE *fh;
    char line[600];
    int have_n = 0, have_e = 0;

    fh = fopen(path, "r");
    if (fh == NULL) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, fh) != NULL) {
        char *nl = strpbrk(line, "\r\n");
        if (nl != NULL)
            *nl = '\0';
        if (strncmp(line, "n=", 2) == 0) {
            const char *hex = line + 2;
            size_t i, len = strlen(hex);
            if (len != RSA_MODULUS_BYTES * 2) {
                snprintf(err, errlen, "%s: modulus is %zu hex chars, expected %d",
                         path, len, RSA_MODULUS_BYTES * 2);
                fclose(fh);
                return -1;
            }
            for (i = 0; i < RSA_MODULUS_BYTES; i++) {
                unsigned v;
                if (sscanf(hex + i * 2, "%2x", &v) != 1) {
                    snprintf(err, errlen, "%s: bad hex in modulus", path);
                    fclose(fh);
                    return -1;
                }
                key->n[i] = (unsigned char)v;
            }
            have_n = 1;
        } else if (strncmp(line, "e=", 2) == 0) {
            key->e = (uint32_t)strtoul(line + 2, NULL, 10);
            have_e = 1;
        }
    }
    fclose(fh);
    if (!have_n || !have_e || key->e < 3) {
        snprintf(err, errlen, "%s: expected n=<hex> and e=<decimal>", path);
        return -1;
    }
    return 0;
}
#endif

/* --- helpers -------------------------------------------------------------- */

static int read_magic(const char *image, char out[5], char *err, size_t errlen)
{
    FILE *fh = fopen(image, "rb");
    size_t n;

    if (fh == NULL) {
        snprintf(err, errlen, "cannot open %s: %s", image, strerror(errno));
        return -1;
    }
    n = fread(out, 1, 4, fh);
    fclose(fh);
    out[4] = '\0';
    if (n != 4) {
        snprintf(err, errlen, "%s is shorter than 4 bytes", image);
        return -1;
    }
    return 0;
}

static int hash_fd_stream(int fd, uint64_t size, unsigned char out[SHA256_DIGEST_LEN],
                          char *err, size_t errlen)
{
    unsigned char buf[65536];
    struct sha256_ctx ctx;
    uint64_t done = 0;

    sha256_init(&ctx);
    while (done < size) {
        size_t want = (size - done < sizeof buf) ? (size_t)(size - done) : sizeof buf;
        ssize_t n = read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            snprintf(err, errlen, "read image: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            snprintf(err, errlen, "image ended after %llu of %llu bytes",
                     (unsigned long long)done, (unsigned long long)size);
            return -1;
        }
        sha256_update(&ctx, buf, (size_t)n);
        done += (uint64_t)n;
    }
    sha256_final(&ctx, out);
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out,
        "usage: moggie-updater --manifest FILE --image FILE [options]\n"
        "\n"
        "  -m, --manifest FILE   signed manifest.json\n"
        "  -i, --image FILE      rootfs squashfs to install\n"
        "  -d, --device PATH     rootfs MTD device (default " DEFAULT_DEVICE ")\n"
        "  -s, --state-dir DIR   where bootslot/boot_ok live (default " DEFAULT_STATE_DIR ")\n"
        "      --slot a|b|auto   target slot; auto = the inactive one (default)\n"
        "      --check           verify only, write nothing\n"
        "      --no-reboot       do not reboot after a successful install\n"
        "  -v, --verbose         more detail on stderr\n"
        "  -h, --help            this text\n"
#ifdef MOGGIE_TEST_MODE
        "\n"
        "test build only:\n"
        "      --test-mode       allow --target to be a regular file\n"
        "      --target FILE     stand-in for the MTD device (>= 16 MiB)\n"
        "      --pubkey-txt FILE public key as n=<hex>/e=<dec>\n"
#endif
        );
}

int main(int argc, char **argv)
{
    const char *manifest_path = NULL;
    const char *image_path = NULL;
    const char *device = DEFAULT_DEVICE;
    const char *state_dir = DEFAULT_STATE_DIR;
    const char *target_file = NULL;
    const char *pubkey_txt = NULL;
    const char *slot_arg = "auto";
    int check_only = 0;
    int no_reboot = 0;
    int test_mode = 0;

    struct rsa_pub key;
    struct ota_manifest mf;
    struct slotdev dev;
    char err[256];
    char cur_slot_buf[8];
    enum slot_id cur_slot, target;
    int imgfd = -1;
    struct stat st;
    unsigned char img_digest[SHA256_DIGEST_LEN];
    unsigned char written_digest[SHA256_DIGEST_LEN];
    unsigned char readback_digest[SHA256_DIGEST_LEN];
    char img_hex[65], rb_hex[65];

    static const struct option opts[] = {
        {"manifest", required_argument, NULL, 'm'},
        {"image", required_argument, NULL, 'i'},
        {"device", required_argument, NULL, 'd'},
        {"state-dir", required_argument, NULL, 's'},
        {"slot", required_argument, NULL, 'S'},
        {"check", no_argument, NULL, 'c'},
        {"no-reboot", no_argument, NULL, 'n'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
#ifdef MOGGIE_TEST_MODE
        {"test-mode", no_argument, NULL, 'T'},
        {"target", required_argument, NULL, 't'},
        {"pubkey-txt", required_argument, NULL, 'k'},
#endif
        {NULL, 0, NULL, 0}
    };
    int opt;

#ifdef MOGGIE_TEST_MODE
#define OPTSTRING "m:i:d:s:S:cnvhTt:k:"
#else
#define OPTSTRING "m:i:d:s:S:cnvh"
#endif

    while ((opt = getopt_long(argc, argv, OPTSTRING, opts, NULL)) != -1) {
        switch (opt) {
        case 'm': manifest_path = optarg; break;
        case 'i': image_path = optarg; break;
        case 'd': device = optarg; break;
        case 's': state_dir = optarg; break;
        case 'S': slot_arg = optarg; break;
        case 'c': check_only = 1; break;
        case 'n': no_reboot = 1; break;
        case 'v': verbose = 1; break;
        case 'h': usage(stdout); return EX_OK;
#ifdef MOGGIE_TEST_MODE
        case 'T': test_mode = 1; break;
        case 't': target_file = optarg; break;
        case 'k': pubkey_txt = optarg; break;
#endif
        default:
            usage(stderr);
            return EX_USAGE;
        }
    }

    /* If nandwrite dies, report a short write instead of dying of SIGPIPE with
     * the slot half-written and no explanation. */
    signal(SIGPIPE, SIG_IGN);

    memset(err, 0, sizeof err);
    memset(&dev, 0, sizeof dev);
    dev.fd = -1;
    dev.child = -1;

#ifndef MOGGIE_TEST_MODE
    /* These only exist in the test binary; reject them loudly if someone tries. */
    if (test_mode || target_file != NULL || pubkey_txt != NULL) {
        logmsg("moggie-updater: this build cannot write regular files");
        return EX_USAGE;
    }
#endif
    if (manifest_path == NULL || image_path == NULL) {
        usage(stderr);
        return EX_USAGE;
    }

    /* --- key --- */
    key_from_embedded(&key);
#ifdef MOGGIE_TEST_MODE
    if (pubkey_txt != NULL) {
        if (key_from_text(pubkey_txt, &key, err, sizeof err) != 0) {
            logmsg("moggie-updater: %s", err);
            return EX_USAGE;
        }
    }
#endif

    /* --- manifest --- */
    {
        int rc = ota_manifest_load(manifest_path, &key, &mf, err, sizeof err);
        if (rc != MF_OK) {
            logmsg("moggie-updater: manifest rejected: %s", err);
            return EX_MANIFEST;
        }
    }
    vlog("manifest ok: version %s, model %u, size %llu, sha256 %s",
         mf.version, mf.model, mf.size, mf.sha256_hex);

    if (mf.model != OTA_MODEL_ID) {
        logmsg("moggie-updater: manifest is for model %u, this firmware is model %u",
               mf.model, OTA_MODEL_ID);
        return EX_MANIFEST;
    }
    if (mf.size == 0 || mf.size > OTA_MAX_IMAGE_BYTES) {
        logmsg("moggie-updater: manifest size %llu is outside the allowed range "
               "(1..%llu bytes)", mf.size, OTA_MAX_IMAGE_BYTES);
        return EX_MANIFEST;
    }

    /* --- image, before anything is written --- */
    imgfd = open(image_path, O_RDONLY);
    if (imgfd < 0) {
        logmsg("moggie-updater: cannot open image %s: %s", image_path, strerror(errno));
        return EX_IMAGE;
    }
    if (fstat(imgfd, &st) != 0) {
        logmsg("moggie-updater: cannot stat image: %s", strerror(errno));
        close(imgfd);
        return EX_IMAGE;
    }
    if (!S_ISREG(st.st_mode)) {
        logmsg("moggie-updater: image %s is not a regular file", image_path);
        close(imgfd);
        return EX_IMAGE;
    }
    if ((unsigned long long)st.st_size != mf.size) {
        logmsg("moggie-updater: image is %llu bytes but the manifest says %llu "
               "(%s by %llu bytes)",
               (unsigned long long)st.st_size, mf.size,
               (unsigned long long)st.st_size < mf.size ? "truncated" : "oversize",
               (unsigned long long)(st.st_size < (off_t)mf.size
                   ? mf.size - (unsigned long long)st.st_size
                   : (unsigned long long)st.st_size - mf.size));
        close(imgfd);
        return EX_IMAGE;
    }
    {
        char magic[5];
        if (read_magic(image_path, magic, err, sizeof err) != 0) {
            logmsg("moggie-updater: %s", err);
            close(imgfd);
            return EX_IMAGE;
        }
        if (memcmp(magic, SQUASHFS_MAGIC, 4) != 0) {
            logmsg("moggie-updater: image does not start with the squashfs magic "
                   "'%s' (got '%s')", SQUASHFS_MAGIC, magic);
            close(imgfd);
            return EX_IMAGE;
        }
    }

    /* --- --check: verify and stop.  Needs no state and no device. --- */
    if (check_only) {
        if (hash_fd_stream(imgfd, mf.size, img_digest, err, sizeof err) != 0) {
            logmsg("moggie-updater: %s", err);
            close(imgfd);
            return EX_IMAGE;
        }
        sha256_hex(img_digest, img_hex);
        if (memcmp(img_digest, mf.sha256, SHA256_DIGEST_LEN) != 0) {
            logmsg("moggie-updater: image sha256 %s does not match the manifest %s",
                   img_hex, mf.sha256_hex);
            close(imgfd);
            return EX_IMAGE;
        }
        printf("verified %s: %llu bytes, sha256 %s, version %s\n",
               image_path, (unsigned long long)mf.size, mf.sha256_hex, mf.version);
        close(imgfd);
        return EX_OK;
    }

    /* --- slots --- */
    /* Fail closed. `bootslot` is NOT usable here: it names the slot to boot
     * next and is rewritten during commit, so after installing B (or after a
     * failed reboot) it says "b" while "a" is still running -- and an "auto"
     * update would then overwrite the running slot. The chooser records the
     * truth in running_slot. If that is missing we cannot tell which slot is
     * live, so we write nothing at all rather than risk the running rootfs. */
    if (state_read(state_dir, STATE_RUNNING_SLOT, cur_slot_buf, sizeof cur_slot_buf) != 0
        || cur_slot_buf[1] != '\0'
        || (cur_slot_buf[0] != 'a' && cur_slot_buf[0] != 'b')) {
        logmsg("moggie-updater: %s/%s missing or invalid; cannot tell which slot "
               "is running. %s records the next boot, not the current one. "
               "Refusing to write.", state_dir, STATE_RUNNING_SLOT, STATE_BOOTSLOT);
        close(imgfd);
        return EX_STATE;
    }
    cur_slot = (cur_slot_buf[0] == 'b') ? SLOT_B : SLOT_A;

    if (strcmp(slot_arg, "auto") == 0) {
        target = (cur_slot == SLOT_A) ? SLOT_B : SLOT_A;
    } else if (strcmp(slot_arg, "a") == 0) {
        target = SLOT_A;
    } else if (strcmp(slot_arg, "b") == 0) {
        target = SLOT_B;
    } else {
        logmsg("moggie-updater: --slot must be a, b or auto");
        close(imgfd);
        return EX_USAGE;
    }
    if (target == cur_slot) {
        logmsg("moggie-updater: slot %c is the running slot; refusing to write it",
               target == SLOT_B ? 'b' : 'a');
        close(imgfd);
        return EX_USAGE;
    }
    vlog("running slot %c, target slot %c", cur_slot == SLOT_B ? 'b' : 'a',
         target == SLOT_B ? 'b' : 'a');

    /* --- device allowlist: mtd0 is never a legal target --- */
    {
        int ok = 0;
#ifndef MOGGIE_TEST_MODE
        if (target_file != NULL)
            ok = 0;
#endif
        if (test_mode) {
            ok = (target_file != NULL);
        } else {
            ok = (strcmp(device, "/dev/mtd3") == 0
                  || strcmp(device, "/dev/mtdblock3") == 0);
        }
        if (!ok) {
            logmsg("moggie-updater: refusing to write %s: only the rootfs device "
                   "(/dev/mtd3) may be updated", test_mode ? (target_file ? target_file : "(none)") : device);
            close(imgfd);
            return EX_DEVICE;
        }
    }

    if (slotdev_open(&dev, test_mode ? target_file : device, target, test_mode,
                     err, sizeof err) != 0) {
        logmsg("moggie-updater: %s", err);
        close(imgfd);
        return EX_DEVICE;
    }

    /* From here the target slot is expendable: mark it so a crash leaves an
     * unmistakably untrustworthy slot behind. */
    if (state_write_slot(state_dir, "slot_status", target, "writing\n") != 0) {
        logmsg("moggie-updater: cannot mark slot %c as writing",
               target == SLOT_B ? 'b' : 'a');
        slotdev_close(&dev);
        close(imgfd);
        return EX_STATE;
    }

    logmsg("moggie-updater: writing %llu bytes to slot %c",
           (unsigned long long)mf.size, target == SLOT_B ? 'b' : 'a');

    if (slotdev_write_stream(&dev, imgfd, mf.size, written_digest, err, sizeof err) != 0) {
        logmsg("moggie-updater: write failed: %s", err);
        (void)state_write_slot(state_dir, "slot_status", target, "bad\n");
        slotdev_close(&dev);
        close(imgfd);
        return EX_WRITE;
    }
    close(imgfd);
    imgfd = -1;

    sha256_hex(written_digest, img_hex);
    if (memcmp(written_digest, mf.sha256, SHA256_DIGEST_LEN) != 0) {
        logmsg("moggie-updater: image changed while streaming: wrote %s, "
               "manifest says %s", img_hex, mf.sha256_hex);
        (void)state_write_slot(state_dir, "slot_status", target, "bad\n");
        slotdev_close(&dev);
        return EX_IMAGE;
    }
    vlog("streamed %llu bytes, sha256 %s", (unsigned long long)mf.size, img_hex);

    /* --- read back and compare before declaring the slot bootable --- */
    if (slotdev_read_hash(&dev, mf.size, readback_digest, err, sizeof err) != 0) {
        logmsg("moggie-updater: read-back failed: %s", err);
        (void)state_write_slot(state_dir, "slot_status", target, "bad\n");
        slotdev_close(&dev);
        return EX_READBACK;
    }
    sha256_hex(readback_digest, rb_hex);
    if (memcmp(readback_digest, mf.sha256, SHA256_DIGEST_LEN) != 0) {
        logmsg("moggie-updater: read-back mismatch: slot has %s, manifest says %s",
               rb_hex, mf.sha256_hex);
        (void)state_write_slot(state_dir, "slot_status", target, "bad\n");
        slotdev_close(&dev);
        return EX_READBACK;
    }
    vlog("read-back verified: %s", rb_hex);
    slotdev_close(&dev);

    /* --- commit: sha, then status, then bootslot; bootslot last --- */
    {
        char size_txt[32];
        snprintf(size_txt, sizeof size_txt, "%llu", mf.size);
        if (state_write_slot(state_dir, "slot_size", target, size_txt) != 0) {
            logmsg("moggie-updater: cannot record the size of slot %c",
                   target == SLOT_B ? 'b' : 'a');
            return EX_STATE;
        }
    }
    if (state_write_slot(state_dir, "slot_sha256", target, mf.sha256_hex) != 0
        || state_write_slot(state_dir, "slot_version", target, mf.version) != 0
        || state_write_slot(state_dir, "slot_status", target, "ok\n") != 0) {
        logmsg("moggie-updater: cannot record the new slot state in %s", state_dir);
        return EX_STATE;
    }
    {
        char slot_txt[3];
        slot_txt[0] = (target == SLOT_B) ? 'b' : 'a';
        slot_txt[1] = '\n';
        slot_txt[2] = '\0';
        if (state_write(state_dir, STATE_BOOTSLOT, slot_txt) != 0) {
            logmsg("moggie-updater: cannot write %s/%s", state_dir, STATE_BOOTSLOT);
            return EX_STATE;
        }
    }
    /* The new slot must not be blamed for the previous slot's boot_ok. */
    (void)state_write(state_dir, STATE_BOOT_OK, "");

    sync();
    logmsg("moggie-updater: slot %c installed and verified (version %s); "
           "bootslot updated", target == SLOT_B ? 'b' : 'a', mf.version);

    if (no_reboot) {
        logmsg("moggie-updater: --no-reboot, not rebooting");
        return EX_OK;
    }
    logmsg("moggie-updater: rebooting");
    execlp("reboot", "reboot", (char *)NULL);
    logmsg("moggie-updater: cannot exec reboot: %s", strerror(errno));
    return EX_OK;
}
