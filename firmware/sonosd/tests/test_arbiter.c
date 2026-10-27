/*
 * test_arbiter.c -- ownership, mutual exclusion, FIFO hand-off, refcounting.
 *
 * These are real threads contending on the real arbiter. The lock file lives in
 * build/test, never /run, so the tests run unprivileged.
 */
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "tests.h"

#include "arbiter.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOCK_PATH "build/test/arb-test.lock"

struct targ {
    const char *src;
    int rc;
    int release_immediately;
};

static pthread_mutex_t g_order_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_order[ARB_MAX_WAITERS + 2][ARB_SOURCE_MAX];
static int g_order_n;

static void order_reset(void)
{
    pthread_mutex_lock(&g_order_mu);
    g_order_n = 0;
    memset(g_order, 0, sizeof(g_order));
    pthread_mutex_unlock(&g_order_mu);
}

static void order_add(const char *src)
{
    pthread_mutex_lock(&g_order_mu);
    if (g_order_n < (int)(sizeof(g_order) / sizeof(g_order[0])))
        snprintf(g_order[g_order_n++], ARB_SOURCE_MAX, "%s", src);
    pthread_mutex_unlock(&g_order_mu);
}

static void *req_thread(void *p)
{
    struct targ *a = p;

    a->rc = arb_request(a->src);
    if (a->rc == ARB_OK) {
        order_add(a->src);
        if (a->release_immediately)
            arb_release(a->src);
    }
    return NULL;
}

/* Poll until the arbiter reports n queued waiters. */
static int wait_waiters(unsigned n, int timeout_ms)
{
    int i;
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;   /* 1 ms */

    for (i = 0; i < timeout_ms; i++) {
        if (arb_waiting() == n)
            return 1;
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void case_ownership(void)
{
    t_suite("arbiter: ownership");

    T_EQ(ARB_OK, arb_init(LOCK_PATH));
    T_EQ(ARB_ERR_INVAL, arb_request(""));
    T_EQ(ARB_ERR_INVAL, arb_try_request("a-name-that-is-far-too-long-for-the-array"));

    T_EQ(ARB_OK, arb_try_request("airplay"));
    T_ASSERT(arb_is_holder("airplay"));
    T_ASSERT(!arb_is_holder("spotify"));
    T_EQ(ARB_ERR_BUSY, arb_try_request("spotify"));
    T_EQ(1, arb_grant_count());

    arb_release("airplay");
    T_ASSERT(!arb_is_holder("airplay"));

    /* Someone else may take it now. */
    T_EQ(ARB_OK, arb_try_request("spotify"));
    T_ASSERT(arb_is_holder("spotify"));
    arb_release("spotify");

    arb_shutdown();
}

static void case_reentrant(void)
{
    t_suite("arbiter: reentrant requests");

    T_EQ(ARB_OK, arb_init(LOCK_PATH));

    T_EQ(ARB_OK, arb_request("cspot"));
    T_EQ(ARB_OK, arb_request("cspot"));   /* nested caller takes a reference */
    T_EQ(ARB_OK, arb_request("cspot"));

    arb_release("cspot");
    T_ASSERT(arb_is_holder("cspot"));     /* still one reference left */
    arb_release("cspot");
    T_ASSERT(arb_is_holder("cspot"));
    arb_release("cspot");
    T_ASSERT(!arb_is_holder("cspot"));

    /* And the path is genuinely free. */
    T_EQ(ARB_OK, arb_try_request("dlna"));
    arb_release("dlna");

    arb_shutdown();
}

static void case_waiter_blocks(void)
{
    struct targ b;
    pthread_t th;
    int rc;

    t_suite("arbiter: second requester waits");

    order_reset();
    T_EQ(ARB_OK, arb_init(LOCK_PATH));
    T_EQ(ARB_OK, arb_request("airplay"));

    memset(&b, 0, sizeof(b));
    b.src = "spotify";
    b.rc = 999;
    b.release_immediately = 0;

    rc = pthread_create(&th, NULL, req_thread, &b);
    T_EQ(0, rc);

    T_ASSERT(wait_waiters(1, 1000));
    T_ASSERT(!arb_is_holder("spotify"));          /* it has not been granted */
    T_EQ(1, arb_waiting());

    /* Releasing the holder must wake it. */
    arb_release("airplay");
    pthread_join(th, NULL);

    T_EQ(ARB_OK, b.rc);
    T_ASSERT(arb_is_holder("spotify"));
    T_EQ(0, arb_waiting());

    arb_release("spotify");
    T_ASSERT(!arb_is_holder("spotify"));

    arb_shutdown();
}

static void case_fifo_order(void)
{
    struct targ args[3];
    pthread_t th[3];
    const char *names[3] = { "spotify", "dlna", "snapcast" };
    int i;

    t_suite("arbiter: FIFO hand-off order");

    order_reset();
    T_EQ(ARB_OK, arb_init(LOCK_PATH));
    T_EQ(ARB_OK, arb_request("airplay"));

    for (i = 0; i < 3; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].src = names[i];
        args[i].rc = 999;
        args[i].release_immediately = 1;
        T_EQ(0, pthread_create(&th[i], NULL, req_thread, &args[i]));
        /* Queue them in a known order: wait for each to be enqueued. */
        T_ASSERT(wait_waiters((unsigned)i + 1, 1000));
    }

    T_EQ(3, arb_waiting());
    arb_release("airplay");

    for (i = 0; i < 3; i++)
        pthread_join(th[i], NULL);

    for (i = 0; i < 3; i++)
        T_EQ(ARB_OK, args[i].rc);

    T_EQ(3, g_order_n);
    for (i = 0; i < 3; i++)
        T_STREQ(names[i], g_order[i]);

    T_ASSERT(!arb_is_holder("snapcast"));
    arb_shutdown();
}

static void case_queue_full(void)
{
    struct targ args[ARB_MAX_WAITERS];
    pthread_t th[ARB_MAX_WAITERS];
    char names[ARB_MAX_WAITERS][12];
    int i;

    t_suite("arbiter: waiter queue bound");

    T_EQ(ARB_OK, arb_init(LOCK_PATH));
    T_EQ(ARB_OK, arb_request("airplay"));

    for (i = 0; i < ARB_MAX_WAITERS; i++) {
        snprintf(names[i], sizeof(names[i]), "w%d", i);
        memset(&args[i], 0, sizeof(args[i]));
        args[i].src = names[i];
        args[i].release_immediately = 1;
        T_EQ(0, pthread_create(&th[i], NULL, req_thread, &args[i]));
    }
    T_ASSERT(wait_waiters(ARB_MAX_WAITERS, 2000));

    /* One more must be refused instead of growing the queue. */
    T_EQ(ARB_ERR_FULL, arb_request("one-too-many"));

    arb_release("airplay");
    for (i = 0; i < ARB_MAX_WAITERS; i++)
        pthread_join(th[i], NULL);

    for (i = 0; i < ARB_MAX_WAITERS; i++)
        T_EQ(ARB_OK, args[i].rc);

    T_EQ(0, arb_waiting());
    arb_shutdown();
}

static void case_misuse(void)
{
    char buf[ARB_SOURCE_MAX];

    t_suite("arbiter: misuse is logged, not fatal");

    T_EQ(ARB_OK, arb_init(LOCK_PATH));

    T_EQ(-1, arb_holder_copy(buf, sizeof(buf)));

    arb_release("ghost");               /* nobody holds it */
    T_EQ(ARB_OK, arb_try_request("real"));
    arb_release("someone-else");        /* not the holder */
    T_ASSERT(arb_is_holder("real"));    /* must still hold */

    T_EQ(0, arb_holder_copy(buf, sizeof(buf)));
    T_STREQ("real", buf);

    arb_release("real");
    arb_release("real");                /* double release */

    T_EQ(ARB_OK, arb_try_request("next"));
    arb_release("next");

    arb_shutdown();
}

void suite_arbiter(void)
{
    log_init(LOG_LEVEL_ERROR, 0, "test");

    case_ownership();
    case_reentrant();
    case_waiter_blocks();
    case_fifo_order();
    case_queue_full();
    case_misuse();
}
