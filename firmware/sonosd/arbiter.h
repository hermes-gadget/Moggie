/*
 * arbiter.h -- one audio source at a time.
 *
 * Two layers, both required:
 *
 *   1. in-process state machine (pthread mutex + condvar, FIFO waiter queue).
 *      This is what makes "second requester waits, release wakes the waiter"
 *      work between the decoder threads.
 *
 *   2. a lock file (fcntl write lock) so a second sonosd, or a test rig, can
 *      see that the audio path is taken. The file also carries the holder's
 *      name in plain text, which is the first thing to look at when audio
 *      "mysteriously" will not play.
 *
 * Contract: only the current holder may write PCM. dsp_write_pcm() enforces it.
 * Requests are reentrant per source name (refcounted), so a helper deep in the
 * call chain can take a reference without deadlocking.
 */
#ifndef SONOSD_ARBITER_H
#define SONOSD_ARBITER_H

#include <stddef.h>

#define ARB_SOURCE_MAX   24
#define ARB_MAX_WAITERS  8

enum arb_result {
    ARB_OK          = 0,
    ARB_ERR_INIT    = -1,   /* arb_init() was not called or failed        */
    ARB_ERR_INVAL   = -2,   /* NULL/empty/oversized source name           */
    ARB_ERR_BUSY    = -3,   /* arb_try_request() only: someone else holds */
    ARB_ERR_FULL    = -4,   /* more than ARB_MAX_WAITERS queued           */
    ARB_ERR_LOCK    = -5    /* lock file could not be opened or locked    */
};

/* lock_path may be NULL for the compiled-in default. Opens (O_CREAT) the file
 * but does not lock it until the first grant. Safe to call again after
 * arb_shutdown(). */
int  arb_init(const char *lock_path);

/* Release the lock file and reset state. Only call when no thread is blocked in
 * arb_request(); the daemon calls it after joining its workers. */
void arb_shutdown(void);

/* Blocks until granted. Returns ARB_OK, or a negative arb_result. */
int  arb_request(const char *source);

/* Non-blocking variant. Returns ARB_OK, ARB_ERR_BUSY, or another negative. */
int  arb_try_request(const char *source);

/* Drops one reference. Releasing a source you do not hold is a logged no-op,
 * not a crash: a buggy endpoint must not take the daemon down. */
void arb_release(const char *source);

int  arb_is_holder(const char *source);
int  arb_holder_copy(char *dst, size_t dstsz);   /* 0 on success, -1 if none */

unsigned arb_waiting(void);          /* queued requesters                  */
unsigned long arb_grant_count(void);
unsigned long arb_release_count(void);

#endif /* SONOSD_ARBITER_H */
