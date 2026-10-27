/*
 * http.h -- the renderer's HTTP/1.1 server.
 *
 * One request per connection, `Connection: close`, non-blocking writes. The
 * connection pool and both buffers are static: no allocation after startup.
 */
#ifndef MOGGIE_DLNA_HTTP_H
#define MOGGIE_DLNA_HTTP_H

#include "dlna.h"
#include "event.h"
#include "renderer.h"

struct http_req_info {
    char   method[16];
    char   path[192];
    char   soap_action[192];
    char   callback[DLNA_GENA_CALLBACK_MAX];
    char   nt[32];
    char   sid[DLNA_GENA_SID_MAX];
    char   timeout_hdr[32];
    char   content_type[64];
    size_t content_length;
    int    has_content_length;
    int    bad;
};

struct http_conn {
    int      fd;
    int      used;
    char     req[DLNA_REQ_MAX + 1];
    size_t   req_len;
    int      parsed;
    size_t   body_start;
    size_t   body_total;
    struct http_req_info request;
    int      sending;
    char     hdr[DLNA_RESP_HDR_MAX];
    size_t   hdr_len;
    char     body[DLNA_RESP_BODY_MAX];
    size_t   body_len;
    const char *static_body;
    size_t   static_len;
    size_t   hdr_sent;
    size_t   body_sent;
    int64_t  deadline_ms;
};

struct http_server {
    int listen_fd;
    const struct dlna_config *cfg;
    struct dlna_renderer *r;
    struct gena *g;
    struct http_conn conns[DLNA_MAX_CLIENTS];
};

int  http_init(struct http_server *s, const struct dlna_config *cfg,
               struct dlna_renderer *r, struct gena *g);
void http_shutdown(struct http_server *s);
void http_accept(struct http_server *s, int64_t now);

int   http_client_fd(const struct http_server *s, int i);
short http_client_events(const struct http_server *s, int i);
void  http_client_ready(struct http_server *s, int i, short revents,
                        int64_t now);
void  http_tick(struct http_server *s, int64_t now);
int   http_next_timeout_ms(const struct http_server *s, int64_t now);

#endif /* MOGGIE_DLNA_HTTP_H */
