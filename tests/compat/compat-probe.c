/*
 * compat-probe.c -- does this libc actually work on this kernel?
 *
 * A toolchain can compile, link, and still produce a binary that dies on the
 * device because its libc assumes a newer kernel. This program exercises the
 * facilities Moggie actually depends on and reports, one line each, whether
 * they work here.
 *
 * Build it with the toolchain under test and run it in three places:
 *
 *   host, under qemu-ppc   -> the architecture is fine
 *   target rootfs, qemu    -> same libc, emulated kernel
 *   the real Play:5        -> the only result that counts
 *
 * Exit status is the number of failures, so it is usable as a gate.
 *
 * Deliberately no third-party dependencies: pthread and libc only.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

static void ok(const char *what)
{
    printf("  ok    %s\n", what);
    g_pass++;
}

static void bad(const char *what, const char *why)
{
    printf("  FAIL  %s: %s\n", what, why);
    g_fail++;
}

/* ------------------------------------------------------------------ heap -- */

static void test_heap(void)
{
    size_t n = 1024 * 1024;
    unsigned char *p = malloc(n);
    size_t i;

    if (!p) {
        bad("heap", "malloc(1 MiB) returned NULL");
        return;
    }
    for (i = 0; i < n; i += 4096)
        p[i] = (unsigned char)i;
    free(p);
    ok("heap: malloc/free 1 MiB");
}

/* -------------------------------------------------------------- filesystem -- */

static void test_filesystem(void)
{
    const char *dir = "/tmp/moggie-probe";
    const char *path = "/tmp/moggie-probe/data";
    char buf[32];
    struct stat st;
    int fd;
    ssize_t n;

    (void)mkdir(dir, 0755);

    fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        bad("fs: open(O_CREAT)", strerror(errno));
        return;
    }
    ok("fs: open(O_CREAT|O_TRUNC|O_RDWR)");

    n = write(fd, "moggie", 6);
    if (n != 6) {
        bad("fs: write", n < 0 ? strerror(errno) : "short write");
        close(fd);
        return;
    }
    ok("fs: write");

    if (lseek(fd, 0, SEEK_SET) != 0) {
        bad("fs: lseek", strerror(errno));
    } else {
        n = read(fd, buf, sizeof(buf));
        if (n == 6 && memcmp(buf, "moggie", 6) == 0)
            ok("fs: lseek + read back");
        else
            bad("fs: read", n < 0 ? strerror(errno) : "content mismatch");
    }
    close(fd);

    if (stat(path, &st) == 0)
        ok("fs: stat");
    else
        bad("fs: stat", strerror(errno));

    if (rename(path, "/tmp/moggie-probe/data2") == 0)
        ok("fs: rename");
    else
        bad("fs: rename", strerror(errno));

    unlink("/tmp/moggie-probe/data2");
    rmdir(dir);
}

/* --------------------------------------------------------------- threading -- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_counter;

static void *worker(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 1000; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter++;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t t[4];
    int i;
    int created = 0;

    for (i = 0; i < 4; i++) {
        if (pthread_create(&t[i], NULL, worker, NULL) != 0)
            break;
        created++;
    }
    if (created != 4) {
        bad("threads: pthread_create", "could not start 4 threads");
        return;
    }
    for (i = 0; i < 4; i++)
        pthread_join(t[i], NULL);

    if (g_counter == 4000)
        ok("threads: 4 threads, mutex-protected counter = 4000");
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "counter = %d, expected 4000", g_counter);
        bad("threads: mutex", msg);
    }
}

/* --------------------------------------------------------------- networking -- */

static void test_sockets(void)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int srv, cli, acc;
    char c = 'x';

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        bad("net: socket", strerror(errno));
        return;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        bad("net: bind loopback", strerror(errno));
        close(srv);
        return;
    }
    if (getsockname(srv, (struct sockaddr *)&addr, &alen) != 0) {
        bad("net: getsockname", strerror(errno));
        close(srv);
        return;
    }
    if (listen(srv, 1) != 0) {
        bad("net: listen", strerror(errno));
        close(srv);
        return;
    }
    ok("net: socket/bind/listen on loopback");

    cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0) {
        bad("net: socket (client)", strerror(errno));
        close(srv);
        return;
    }
    if (connect(cli, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        bad("net: connect loopback", strerror(errno));
        close(cli);
        close(srv);
        return;
    }
    acc = accept(srv, NULL, NULL);
    if (acc < 0) {
        bad("net: accept", strerror(errno));
    } else {
        if (send(cli, &c, 1, 0) == 1 && recv(acc, &c, 1, 0) == 1)
            ok("net: connect/accept/send/recv round-trip");
        else
            bad("net: send/recv", strerror(errno));
        close(acc);
    }
    close(cli);
    close(srv);
}

/* ------------------------------------------------------------------ timing -- */

static void test_time(void)
{
    struct timespec a, b, req;
    struct timeval tv;
    struct tm tmv;
    long ms;

    if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
        bad("time: clock_gettime(CLOCK_MONOTONIC)", strerror(errno));
    } else {
        req.tv_sec = 0;
        req.tv_nsec = 50 * 1000 * 1000;
        nanosleep(&req, NULL);
        clock_gettime(CLOCK_MONOTONIC, &b);
        ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        if (ms >= 45 && ms < 2000)
            ok("time: clock_gettime + nanosleep(50ms) elapsed correctly");
        else {
            char msg[64];
            snprintf(msg, sizeof(msg), "elapsed %ld ms", ms);
            bad("time: nanosleep", msg);
        }
    }

    if (gettimeofday(&tv, NULL) == 0 && localtime_r(&tv.tv_sec, &tmv) != NULL)
        ok("time: gettimeofday + localtime_r");
    else
        bad("time: gettimeofday/localtime_r", strerror(errno));
}

/* ----------------------------------------------------------------- entropy -- */

static void test_entropy(void)
{
    unsigned char buf[16];
    int fd;
    ssize_t n;

    /* getrandom() is Linux 3.17+. On an older kernel glibc is supposed to fall
     * back to /dev/urandom; if that fallback is broken, this is where a 2.6.35
     * device will tell us. */
    n = getrandom(buf, sizeof(buf), 0);
    if (n == (ssize_t)sizeof(buf))
        ok("entropy: getrandom()");
    else if (n < 0 && (errno == ENOSYS || errno == EPERM))
        printf("  note  entropy: getrandom() -> %s (fallback path in use)\n",
               strerror(errno));
    else
        bad("entropy: getrandom", n < 0 ? strerror(errno) : "short read");

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        bad("entropy: /dev/urandom", strerror(errno));
        return;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n == (ssize_t)sizeof(buf))
        ok("entropy: /dev/urandom");
    else
        bad("entropy: /dev/urandom read", n < 0 ? strerror(errno) : "short read");
}

/* ------------------------------------------------------------------ memory -- */

static void test_mmap(void)
{
    size_t n = 64 * 1024;
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
        bad("mmap: anonymous", strerror(errno));
        return;
    }
    memset(p, 0xa5, n);
    munmap(p, n);
    ok("mmap: anonymous 64 KiB");
}

/* ----------------------------------------------------------------- signals -- */

static volatile sig_atomic_t g_got;

static void on_usr1(int sig)
{
    (void)sig;
    g_got = 1;
}

static void test_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        bad("signal: sigaction", strerror(errno));
        return;
    }
    if (raise(SIGUSR1) != 0 || !g_got) {
        bad("signal: raise(SIGUSR1)", "handler did not run");
        return;
    }
    ok("signal: sigaction + raise(SIGUSR1)");
}

/* ------------------------------------------------------------------- info -- */

static void report_env(void)
{
    printf("  info  %ld CPUs, %ld byte pages\n",
           sysconf(_SC_NPROCESSORS_ONLN), sysconf(_SC_PAGESIZE));
}

int main(void)
{
    printf("moggie compat probe\n\n");

    report_env();
    test_heap();
    test_filesystem();
    test_threads();
    test_sockets();
    test_time();
    test_entropy();
    test_mmap();
    test_signals();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("compat probe: PASS\n");
    else
        printf("compat probe: FAIL\n");

    return g_fail;
}
