#define _POSIX_C_SOURCE 200809L

#include "arbiter.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SONOSD_DEFAULT_LOCK_PATH
#define SONOSD_DEFAULT_LOCK_PATH "/run/sonosd.audio.lock"
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

struct arb_waiter {
    char          name[ARB_SOURCE_MAX];
    unsigned long gen;      /* bumped when this slot is granted */
};

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             inited;
    int             fd;

    char            path[96];
    char            holder[ARB_SOURCE_MAX];
    int             refs;

    struct arb_waiter q[ARB_MAX_WAITERS];
    int             q_head;
    int             q_tail;
    int             q_len;

    unsigned long   grants;
    unsigned long   releases;
} g = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .inited = 0,
    .fd = -1
};

/* -------- lock file ---------------------------------------------------- */

static int file_lock(int block)
{
    struct flock fl;

    if (g.fd < 0)
        return -1;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    for (;;) {
        if (fcntl(g.fd, block ? F_SETLKW : F_SETLK, &fl) == 0)
            return 0;
        if (errno == EINTR && block)
            continue;
        if (!block && (errno == EACCES || errno == EAGAIN))
            return 1;    /* someone else holds it */
        return -1;
    }
}

static void file_unlock(void)
{
    struct flock fl;

    if (g.fd < 0)
        return;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    (void)fcntl(g.fd, F_SETLK, &fl);
}

/* Best effort: the file is a diagnostic aid, not the source of truth. */
static void write_holder_locked(void)
{
    char buf[ARB_SOURCE_MAX + 2];
    size_t n;
    int rc;

    if (g.fd < 0)
        return;

    if (g.holder[0] == '\0') {
        rc = ftruncate(g.fd, 0);
        (void)rc;
        return;
    }

    n = strlen(g.holder);
    memcpy(buf, g.holder, n);
    buf[n++] = '\n';
    rc = ftruncate(g.fd, 0);
    (void)rc;
    if (pwrite(g.fd, buf, n, 0) < 0)
        log_debug("arbiter: could not write holder name: %s", strerror(errno));
}

/* -------- state helpers (all called with g.mu held) -------------------- */

static void grant_locked(const char *source)
{
    memset(g.holder, 0, sizeof(g.holder));
    memcpy(g.holder, source, strlen(source));
    g.refs = 1;
    g.grants++;
    write_holder_locked();
    log_info("arbiter: granted to '%s'", g.holder);
}

static int valid_source(const char *source)
{
    size_t n;

    if (!source || !*source)
        return 0;
    n = strlen(source);
    return n > 0 && n < ARB_SOURCE_MAX;
}

/* -------- public ------------------------------------------------------- */

int arb_init(const char *lock_path)
{
    if (g.inited)
        return ARB_OK;

    if (pthread_mutex_init(&g.mu, NULL) != 0)
        return ARB_ERR_INIT;
    if (pthread_cond_init(&g.cv, NULL) != 0) {
        pthread_mutex_destroy(&g.mu);
        return ARB_ERR_INIT;
    }

    memset(g.holder, 0, sizeof(g.holder));
    g.refs = 0;
    g.q_head = g.q_tail = g.q_len = 0;
    g.grants = g.releases = 0;
    memset(g.q, 0, sizeof(g.q));

    snprintf(g.path, sizeof(g.path), "%s",
             (lock_path && *lock_path) ? lock_path : SONOSD_DEFAULT_LOCK_PATH);

    g.fd = open(g.path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (g.fd < 0) {
        log_error("arbiter: cannot open lock file %s: %s", g.path, strerror(errno));
        pthread_cond_destroy(&g.cv);
        pthread_mutex_destroy(&g.mu);
        return ARB_ERR_LOCK;
    }

    g.inited = 1;
    log_debug("arbiter: lock file %s", g.path);
    return ARB_OK;
}

void arb_shutdown(void)
{
    if (!g.inited)
        return;

    pthread_mutex_lock(&g.mu);
    if (g.q_len > 0)
        log_warn("arbiter: shutting down with %d waiter(s) still queued", g.q_len);
    g.holder[0] = '\0';
    g.refs = 0;
    g.q_head = g.q_tail = g.q_len = 0;
    pthread_mutex_unlock(&g.mu);

    if (g.fd >= 0) {
        file_unlock();
        close(g.fd);
        g.fd = -1;
    }

    pthread_cond_destroy(&g.cv);
    pthread_mutex_destroy(&g.mu);
    g.inited = 0;
}

int arb_try_request(const char *source)
{
    int rc;

    if (!g.inited)
        return ARB_ERR_INIT;
    if (!valid_source(source))
        return ARB_ERR_INVAL;

    pthread_mutex_lock(&g.mu);
    if (g.holder[0] && strcmp(g.holder, source) == 0) {
        g.refs++;
        pthread_mutex_unlock(&g.mu);
        return ARB_OK;
    }
    if (g.holder[0] != '\0') {
        pthread_mutex_unlock(&g.mu);
        return ARB_ERR_BUSY;
    }

    rc = file_lock(0);
    if (rc != 0) {
        pthread_mutex_unlock(&g.mu);
        return (rc > 0) ? ARB_ERR_BUSY : ARB_ERR_LOCK;
    }
    grant_locked(source);
    pthread_mutex_unlock(&g.mu);
    return ARB_OK;
}

int arb_request(const char *source)
{
    struct arb_waiter *w;
    unsigned long mygen;
    int rc;

    if (!g.inited)
        return ARB_ERR_INIT;
    if (!valid_source(source))
        return ARB_ERR_INVAL;

    pthread_mutex_lock(&g.mu);

    if (g.holder[0] && strcmp(g.holder, source) == 0) {
        g.refs++;
        pthread_mutex_unlock(&g.mu);
        return ARB_OK;
    }

    if (g.holder[0] == '\0') {
        /* Blocking file lock while holding the mutex: another process owns the
         * audio path. Threads of this process cannot queue behind it, which is
         * correct -- they would all be waiting for the same device anyway. */
        rc = file_lock(1);
        if (rc != 0) {
            pthread_mutex_unlock(&g.mu);
            return ARB_ERR_LOCK;
        }
        grant_locked(source);
        pthread_mutex_unlock(&g.mu);
        return ARB_OK;
    }

    if (g.q_len >= ARB_MAX_WAITERS) {
        pthread_mutex_unlock(&g.mu);
        log_warn("arbiter: waiter queue full, refusing '%s'", source);
        return ARB_ERR_FULL;
    }

    w = &g.q[g.q_tail];
    memset(w->name, 0, sizeof(w->name));
    memcpy(w->name, source, strlen(source));
    mygen = w->gen;

    g.q_tail = (g.q_tail + 1) % ARB_MAX_WAITERS;
    g.q_len++;

    log_info("arbiter: '%s' waiting for '%s' (%d queued)", source, g.holder, g.q_len);

    while (w->gen == mygen)
        pthread_cond_wait(&g.cv, &g.mu);

    /* arb_release() handed us the baton: holder == source, refs == 1. */
    pthread_mutex_unlock(&g.mu);
    return ARB_OK;
}

void arb_release(const char *source)
{
    struct arb_waiter *w;

    if (!g.inited) {
        log_warn("arbiter: release('%s') before arb_init()", source ? source : "(null)");
        return;
    }
    if (!valid_source(source)) {
        log_warn("arbiter: release() with invalid source name");
        return;
    }

    pthread_mutex_lock(&g.mu);

    if (g.holder[0] == '\0') {
        pthread_mutex_unlock(&g.mu);
        log_warn("arbiter: '%s' released audio but nobody held it", source);
        return;
    }
    if (strcmp(g.holder, source) != 0) {
        char had[ARB_SOURCE_MAX];
        memcpy(had, g.holder, sizeof(had));
        pthread_mutex_unlock(&g.mu);
        log_warn("arbiter: '%s' tried to release audio held by '%s'", source, had);
        return;
    }

    g.releases++;

    if (g.refs > 1) {
        g.refs--;
        pthread_mutex_unlock(&g.mu);
        return;
    }

    g.refs = 0;
    g.holder[0] = '\0';

    if (g.q_len > 0) {
        /* Hand the baton over while still holding the file lock: the audio path
         * never becomes free between the two owners. */
        w = &g.q[g.q_head];
        g.q_head = (g.q_head + 1) % ARB_MAX_WAITERS;
        g.q_len--;

        memcpy(g.holder, w->name, sizeof(g.holder) - 1);
        g.holder[sizeof(g.holder) - 1] = '\0';
        g.refs = 1;
        g.grants++;
        w->gen++;
        write_holder_locked();
        log_info("arbiter: handed over to '%s'", g.holder);
        pthread_cond_broadcast(&g.cv);
    } else {
        write_holder_locked();
        file_unlock();
        log_info("arbiter: released, audio path idle");
        pthread_cond_broadcast(&g.cv);
    }

    pthread_mutex_unlock(&g.mu);
}

int arb_is_holder(const char *source)
{
    int yes;

    if (!g.inited || !valid_source(source))
        return 0;

    pthread_mutex_lock(&g.mu);
    yes = (g.holder[0] != '\0') && (strcmp(g.holder, source) == 0);
    pthread_mutex_unlock(&g.mu);
    return yes;
}

int arb_holder_copy(char *dst, size_t dstsz)
{
    int rc = -1;

    if (!dst || dstsz == 0)
        return -1;

    if (!g.inited) {
        dst[0] = '\0';
        return -1;
    }

    pthread_mutex_lock(&g.mu);
    if (g.holder[0] != '\0') {
        snprintf(dst, dstsz, "%s", g.holder);
        rc = 0;
    } else {
        dst[0] = '\0';
    }
    pthread_mutex_unlock(&g.mu);
    return rc;
}

unsigned arb_waiting(void)
{
    unsigned n;

    if (!g.inited)
        return 0;
    pthread_mutex_lock(&g.mu);
    n = (unsigned)g.q_len;
    pthread_mutex_unlock(&g.mu);
    return n;
}

unsigned long arb_grant_count(void)
{
    unsigned long n;

    if (!g.inited)
        return 0;
    pthread_mutex_lock(&g.mu);
    n = g.grants;
    pthread_mutex_unlock(&g.mu);
    return n;
}

unsigned long arb_release_count(void)
{
    unsigned long n;

    if (!g.inited)
        return 0;
    pthread_mutex_lock(&g.mu);
    n = g.releases;
    pthread_mutex_unlock(&g.mu);
    return n;
}
