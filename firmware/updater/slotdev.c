#include "slotdev.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MOGGIE_TEST_MODE
#include <sys/ioctl.h>
#endif

#define CHUNK_DEFAULT (64u * 1024u)
#define CHUNK_MAX CHUNK_DEFAULT

/* One staging buffer for all image I/O.  An 8 MB image plus a decompressor plus
 * the rest of userspace does not fit in 32 MB, so the image is never held. */
static unsigned char iobuf[CHUNK_MAX];

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static size_t chunk_bytes(void)
{
    size_t v = CHUNK_DEFAULT;
#ifdef MOGGIE_TEST_MODE
    const char *s = getenv("MOGGIE_TEST_CHUNK_BYTES");
    if (s != NULL && *s != '\0') {
        long n = strtol(s, NULL, 10);
        if (n >= 512 && n <= (long)CHUNK_MAX)
            v = (size_t)n;
    }
#endif
    return v;
}

static void test_delay(void)
{
#ifdef MOGGIE_TEST_MODE
    const char *s = getenv("MOGGIE_TEST_WRITE_DELAY_MS");
    if (s != NULL && *s != '\0') {
        long v = strtol(s, NULL, 10);
        if (v > 0 && v <= 5000) {
            struct timespec ts;
            ts.tv_sec = v / 1000;
            ts.tv_nsec = (v % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
    }
#endif
}

static int write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int pwrite_all(int fd, const unsigned char *buf, size_t len, uint64_t off)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = pwrite(fd, buf + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int pread_exact(int fd, unsigned char *buf, size_t len, uint64_t off)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, buf + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* short: the target shrank under us */
        done += (size_t)n;
    }
    return 0;
}

/* --- device backend ------------------------------------------------------ */

static int spawn_nandwrite(struct slotdev *d, uint64_t size, char *err, size_t errlen)
{
    int fds[2];
    pid_t pid;
    char base_s[32], size_s[32];

    if (pipe(fds) != 0) {
        set_err(err, errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    snprintf(base_s, sizeof base_s, "%llu", (unsigned long long)d->base);
    snprintf(size_s, sizeof size_s, "%llu", (unsigned long long)size);

    pid = fork();
    if (pid < 0) {
        set_err(err, errlen, "fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: stream stdin into the slot. */
        if (dup2(fds[0], STDIN_FILENO) < 0)
            _exit(127);
        close(fds[0]);
        close(fds[1]);
        execlp("nandwrite", "nandwrite", "-p", "-q", "-s", base_s, "-l", size_s,
               d->path, "-", (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    d->fd = fds[1];
    d->child = (int)pid;
    return 0;
}

static int spawn_nanddump(struct slotdev *d, uint64_t size, char *err, size_t errlen)
{
    int fds[2];
    pid_t pid;
    char base_s[32], size_s[32];

    if (pipe(fds) != 0) {
        set_err(err, errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    snprintf(base_s, sizeof base_s, "%llu", (unsigned long long)d->base);
    snprintf(size_s, sizeof size_s, "%llu", (unsigned long long)size);

    pid = fork();
    if (pid < 0) {
        set_err(err, errlen, "fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(fds[0]);
        close(fds[1]);
        execlp("nanddump", "nanddump", "-q", "-s", base_s, "-l", size_s,
               d->path, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    d->fd = fds[0];
    d->child = (int)pid;
    return 0;
}

static int wait_child(struct slotdev *d, const char *what, char *err, size_t errlen)
{
    int status = 0;

    if (d->child <= 0)
        return 0;
    while (waitpid((pid_t)d->child, &status, 0) < 0) {
        if (errno != EINTR) {
            set_err(err, errlen, "waitpid: %s", strerror(errno));
            d->child = -1;
            return -1;
        }
    }
    d->child = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_err(err, errlen, "%s failed (status %d%s)", what,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFEXITED(status) && WEXITSTATUS(status) == 127
                    ? ": mtd-utils not found on PATH" : "");
        return -1;
    }
    return 0;
}

/* --- public API ---------------------------------------------------------- */

int slotdev_open(struct slotdev *d, const char *path, enum slot_id slot,
                 int test_mode, char *err, size_t errlen)
{
    struct stat st;
    int fd;

#ifndef MOGGIE_TEST_MODE
    (void)test_mode;
#endif

    memset(d, 0, sizeof *d);
    d->child = -1;
    d->fd = -1;
    d->base = (slot == SLOT_B) ? SLOT_BYTES : 0;
    d->slot_bytes = SLOT_BYTES;
    if (path == NULL || strlen(path) >= sizeof d->path) {
        set_err(err, errlen, "device path is missing or too long");
        return -1;
    }
    snprintf(d->path, sizeof d->path, "%s", path);

    fd = open(path, O_RDWR);
    if (fd < 0) {
        set_err(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        set_err(err, errlen, "cannot stat %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
#ifndef MOGGIE_TEST_MODE
        set_err(err, errlen,
                "%s is a regular file; this build only writes NAND devices "
                "(rebuild with MOGGIE_TEST_MODE for the file backend)", path);
        close(fd);
        return -1;
#else
        if (!test_mode) {
            set_err(err, errlen,
                    "%s is a regular file but --test-mode was not given", path);
            close(fd);
            return -1;
        }
        if ((uint64_t)st.st_size < FLASH_ROOTFS_BYTES) {
            set_err(err, errlen,
                    "test target %s is %llu bytes; it must be at least %llu "
                    "(two %llu-byte slots)",
                    path, (unsigned long long)st.st_size,
                    (unsigned long long)FLASH_ROOTFS_BYTES,
                    (unsigned long long)SLOT_BYTES);
            close(fd);
            return -1;
        }
        d->regular = 1;
        d->fd = fd;
        return 0;
#endif
    }

#ifdef MOGGIE_TEST_MODE
    if (test_mode) {
        set_err(err, errlen,
                "%s is not a regular file; --test-mode expects a plain file",
                path);
        close(fd);
        return -1;
    }
#endif

    close(fd);
    d->regular = 0;
    return 0;
}

int slotdev_write_stream(struct slotdev *d, int imgfd, uint64_t size,
                         unsigned char digest_out[SHA256_DIGEST_LEN],
                         char *err, size_t errlen)
{
    struct sha256_ctx ctx;
    uint64_t written = 0;
    size_t chunk = chunk_bytes();
    int rc = -1;

    if (size == 0 || size > d->slot_bytes) {
        set_err(err, errlen, "refusing to write %llu bytes into a %llu-byte slot",
                (unsigned long long)size, (unsigned long long)d->slot_bytes);
        return -1;
    }
    if (chunk == 0 || chunk > CHUNK_MAX)
        chunk = CHUNK_MAX;

    sha256_init(&ctx);

    if (!d->regular) {
        if (spawn_nandwrite(d, size, err, errlen) != 0)
            return -1;
    }

    while (written < size) {
        size_t want = (size - written < (uint64_t)chunk) ? (size_t)(size - written) : chunk;
        size_t got = 0;

        while (got < want) {
            size_t remaining = want - got;
            ssize_t n;

            if (remaining > chunk)
                remaining = chunk;
            n = read(imgfd, iobuf + got, remaining);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                set_err(err, errlen, "read image: %s", strerror(errno));
                goto out;
            }
            if (n == 0)
                break;
            got += (size_t)n;
        }
        if (got == 0) {
            set_err(err, errlen,
                    "image ended after %llu of %llu bytes (truncated)",
                    (unsigned long long)written, (unsigned long long)size);
            goto out;
        }
        sha256_update(&ctx, iobuf, got);

        if (d->regular) {
            if (pwrite_all(d->fd, iobuf, got, d->base + written) != 0) {
                set_err(err, errlen, "write to %s at %llu: %s", d->path,
                        (unsigned long long)(d->base + written), strerror(errno));
                goto out;
            }
        } else {
            if (write_all(d->fd, iobuf, got) != 0) {
                set_err(err, errlen, "short write to nandwrite: %s", strerror(errno));
                goto out;
            }
        }
        written += got;
        test_delay();
    }

    if (!d->regular) {
        /* Closing nandwrite's stdin lets it finish and pad the tail. */
        close(d->fd);
        d->fd = -1;
        if (wait_child(d, "nandwrite", err, errlen) != 0)
            goto out;
    }

    sha256_final(&ctx, digest_out);
    rc = 0;

out:
    if (!d->regular && d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
        (void)wait_child(d, "nandwrite", err, errlen);
    }
    return rc;
}

int slotdev_read_hash(struct slotdev *d, uint64_t size,
                      unsigned char digest_out[SHA256_DIGEST_LEN],
                      char *err, size_t errlen)
{
    struct sha256_ctx ctx;
    uint64_t done = 0;
    size_t chunk = chunk_bytes();
    int rc = -1;

    if (chunk == 0 || chunk > CHUNK_MAX)
        chunk = CHUNK_MAX;
    if (size == 0 || size > d->slot_bytes) {
        set_err(err, errlen, "read-back length %llu is outside the %llu-byte slot",
                (unsigned long long)size, (unsigned long long)d->slot_bytes);
        return -1;
    }
    sha256_init(&ctx);

    if (!d->regular) {
        if (spawn_nanddump(d, size, err, errlen) != 0)
            return -1;
    }

    while (done < size) {
        size_t want = (size - done < (uint64_t)chunk)
                          ? (size_t)(size - done) : chunk;
        size_t got = 0;

        if (d->regular) {
            if (pread_exact(d->fd, iobuf, want, d->base + done) != 0) {
                set_err(err, errlen, "read back %s at %llu: %s", d->path,
                        (unsigned long long)(d->base + done), strerror(errno));
                goto out;
            }
            got = want;
        } else {
            while (got < want) {
                size_t remaining = want - got;
                ssize_t n;

                if (remaining > chunk)
                    remaining = chunk;
                n = read(d->fd, iobuf + got, remaining);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    set_err(err, errlen, "read from nanddump: %s", strerror(errno));
                    goto out;
                }
                if (n == 0) {
                    set_err(err, errlen,
                            "nanddump returned only %llu of %llu bytes",
                            (unsigned long long)(done + got),
                            (unsigned long long)size);
                    goto out;
                }
                got += (size_t)n;
            }
        }

#ifdef MOGGIE_TEST_MODE
        /* Fault injection: prove the read-back check actually gates the flip. */
        if (done == 0 && got > 128 && getenv("MOGGIE_TEST_READBACK_CORRUPT") != NULL)
            iobuf[100] ^= 0x01;
#endif
        sha256_update(&ctx, iobuf, got);
        done += got;
    }

    if (!d->regular) {
        close(d->fd);
        d->fd = -1;
        if (wait_child(d, "nanddump", err, errlen) != 0)
            goto out;
    }

    sha256_final(&ctx, digest_out);
    rc = 0;

out:
    if (!d->regular && d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
        (void)wait_child(d, "nanddump", err, errlen);
    }
    return rc;
}

void slotdev_close(struct slotdev *d)
{
    if (d == NULL)
        return;
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    if (d->child > 0) {
        (void)wait_child(d, "mtd-utils", NULL, 0);
    }
}
