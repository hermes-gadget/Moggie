/*
 * event.h -- GENA subscriptions and non-blocking NOTIFY delivery.
 *
 * Subscribers live in a fixed table. A NOTIFY never blocks the event loop: it
 * gets a non-blocking socket and a 2 second deadline, and a subscriber that
 * misses three deliveries is dropped. The renderer is single-threaded, so the
 * subscribe/flush/tick calls all run on that one loop.
 */
#ifndef MOGGIE_DLNA_EVENT_H
#define MOGGIE_DLNA_EVENT_H

#include "dlna.h"
#include "renderer.h"

#include <netinet/in.h>

struct gena_sub {
    int      used;
    int      service;
    char     sid[DLNA_GENA_SID_MAX];
    char     callback[DLNA_GENA_CALLBACK_MAX];
    char     path[128];
    struct sockaddr_in addr;
    unsigned seq;
    int      timeout_s;
    int64_t  expires_ms;
    int      failures;
};

struct gena_out {
    int      used;
    int      fd;
    int      stage;          /* 0=connecting 1=sending 2=reading reply */
    char     buf[DLNA_GENA_OUT_BUF];
    size_t   len;
    size_t   sent;
    int64_t  deadline_ms;
    char     sid[DLNA_GENA_SID_MAX];
};

struct gena {
    struct gena_sub subs[DLNA_GENA_MAX_SUBS];
    struct gena_out out[DLNA_GENA_MAX_OUT];
    struct dlna_renderer *r;
    const struct dlna_config *cfg;
    unsigned sid_counter;
};

void gena_init(struct gena *g, const struct dlna_config *cfg,
               struct dlna_renderer *r);
void gena_shutdown(struct gena *g);

/* Returns an HTTP status: 200 on success, 412 for a bad/unknown SID or
 * missing CALLBACK/NT, 503 when the table is full. */
int gena_subscribe(struct gena *g, int service,
                   const char *callback, const char *nt,
                   const char *timeout_hdr, const char *sid_hdr,
                   char *sid_out, size_t sid_outsz, int *timeout_out);
int gena_unsubscribe(struct gena *g, int service, const char *sid);

/* Publish LastChange for every service the renderer marked dirty. */
void gena_flush(struct gena *g);

/* Expire subscriptions and abort slow NOTIFY deliveries. */
void gena_tick(struct gena *g, int64_t now);
int  gena_next_timeout_ms(const struct gena *g, int64_t now);

/* Poll integration, one entry per out[] slot. */
int   gena_out_fd(const struct gena *g, int i);
short gena_out_events(const struct gena *g, int i);
int   gena_out_used(const struct gena *g, int i);
void  gena_out_ready(struct gena *g, int i, short revents, int64_t now);

#endif /* MOGGIE_DLNA_EVENT_H */
