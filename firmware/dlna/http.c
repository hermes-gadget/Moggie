#define _POSIX_C_SOURCE 200809L

#include "http.h"
#include "description.h"
#include "log.h"
#include "scpd.h"
#include "soap.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define HTTP_REQUEST_TIMEOUT_MS 5000
#define HTTP_SEND_TIMEOUT_MS    5000

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_conn(struct http_conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

static void http_date(char *out, size_t outsz)
{
    time_t t = time(NULL);
    struct tm tm;

    gmtime_r(&t, &tm);
    strftime(out, outsz, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static void resp_start(struct http_conn *c, int status, const char *ctype,
                       const char *body, size_t body_len, const char *extra)
{
    char date[40];

    http_date(date, sizeof(date));
    c->static_body = body;
    c->static_len = body_len;
    c->hdr_sent = 0;
    c->body_sent = 0;
    c->sending = 1;
    c->hdr_len = (size_t)snprintf(c->hdr, sizeof(c->hdr),
        "HTTP/1.1 %d %s\r\n"
        "DATE: %s\r\n"
        "SERVER: %s\r\n"
        "EXT:\r\n"
        "CONTENT-TYPE: %s\r\n"
        "CONTENT-LENGTH: %zu\r\n"
        "CONNECTION: close\r\n"
        "%s"
        "\r\n",
        status, reason_phrase(status), date, DLNA_SERVER,
        ctype ? ctype : "text/plain; charset=\"utf-8\"",
        body_len, extra ? extra : "");
    if (c->hdr_len >= sizeof(c->hdr)) {
        /* Cannot happen with the fixed extra strings, but never emit a
         * truncated response header. */
        c->hdr_len = 0;
        c->static_body = NULL;
        c->static_len = 0;
        c->sending = 0;
        close_conn(c);
    }
}

static void resp_text(struct http_conn *c, int status, const char *msg)
{
    int n = snprintf(c->body, sizeof(c->body), "%s\n", msg);

    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof(c->body))
        n = (int)sizeof(c->body) - 1;
    resp_start(c, status, "text/plain; charset=\"utf-8\"", c->body,
               (size_t)n, NULL);
}

static void conn_send(struct http_conn *c)
{
    struct iovec iov[2];
    size_t body_total = c->static_len;
    int iovcnt = 0;
    ssize_t n;

    if (!c->sending)
        return;

    if (c->hdr_sent < c->hdr_len) {
        iov[iovcnt].iov_base = c->hdr + c->hdr_sent;
        iov[iovcnt].iov_len = c->hdr_len - c->hdr_sent;
        iovcnt++;
    }
    if (c->body_sent < body_total) {
        iov[iovcnt].iov_base = (void *)(c->static_body + c->body_sent);
        iov[iovcnt].iov_len = body_total - c->body_sent;
        iovcnt++;
    }
    if (iovcnt == 0 || (iovcnt == 1 && iov[0].iov_len == 0)) {
        close_conn(c);
        return;
    }

    n = writev(c->fd, iov, iovcnt);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        close_conn(c);
        return;
    }

    {
        size_t left = (size_t)n;

        while (left > 0 && c->hdr_sent < c->hdr_len) {
            size_t take = c->hdr_len - c->hdr_sent;

            if (take > left)
                take = left;
            c->hdr_sent += take;
            left -= take;
        }
        while (left > 0 && c->body_sent < body_total) {
            size_t take = body_total - c->body_sent;

            if (take > left)
                take = left;
            c->body_sent += take;
            left -= take;
        }
    }

    if (c->hdr_sent == c->hdr_len && c->body_sent == body_total)
        close_conn(c);
}

/* -------- request parsing ------------------------------------------------ */

static int parse_request(char *buf, size_t hdr_end, struct http_req_info *ri)
{
    char *line;
    char *end;
    char *body;
    int lines = 0;

    memset(ri, 0, sizeof(*ri));
    /* Keep the last header's CRLF intact so the line scanner can find it;
     * terminate the header block at the blank line's CR instead. */
    buf[hdr_end + 2] = '\0';

    line = buf;
    end = strstr(line, "\r\n");
    if (!end) {
        ri->bad = 1;
        return -1;
    }
    *end = '\0';

    {
        char version[16];
        int n = sscanf(line, "%15s %191s %15s", ri->method, ri->path, version);

        if (n != 3 || strncmp(version, "HTTP/1.", 7) != 0) {
            ri->bad = 1;
            return -1;
        }
    }

    body = strchr(ri->path, '?');
    if (body)
        *body = '\0';

    line = end + 2;
    while (*line) {
        char *colon;
        char *value;
        char name[48];

        end = strstr(line, "\r\n");
        if (!end)
            break;
        *end = '\0';

        if (++lines > 48) {
            ri->bad = 1;
            return -1;
        }

        colon = strchr(line, ':');
        if (!colon) {
            line = end + 2;
            continue;
        }
        *colon = '\0';
        value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        trim_ws(line);
        snprintf(name, sizeof(name), "%s", line);
        trim_ws(value);

        if (str_ieq(name, "SOAPACTION")) {
            if (header_value(value, ri->soap_action,
                             sizeof(ri->soap_action)) != 0)
                ri->bad = 1;
        } else if (str_ieq(name, "CALLBACK")) {
            snprintf(ri->callback, sizeof(ri->callback), "%s", value);
        } else if (str_ieq(name, "NT")) {
            if (header_value(value, ri->nt, sizeof(ri->nt)) != 0)
                ri->bad = 1;
        } else if (str_ieq(name, "SID")) {
            if (header_value(value, ri->sid, sizeof(ri->sid)) != 0)
                ri->bad = 1;
        } else if (str_ieq(name, "TIMEOUT")) {
            if (header_value(value, ri->timeout_hdr,
                             sizeof(ri->timeout_hdr)) != 0)
                ri->bad = 1;
        } else if (str_ieq(name, "CONTENT-TYPE")) {
            snprintf(ri->content_type, sizeof(ri->content_type), "%s", value);
        } else if (str_ieq(name, "CONTENT-LENGTH")) {
            char *num_end;
            unsigned long v;

            v = strtoul(value, &num_end, 10);
            if (num_end == value || *num_end != '\0' || value[0] == '-') {
                ri->bad = 1;
            } else {
                ri->content_length = (size_t)v;
                ri->has_content_length = 1;
            }
        } else if (str_ieq(name, "TRANSFER-ENCODING")) {
            ri->bad = 1;   /* chunked request bodies are not supported */
        }

        line = end + 2;
    }

    return ri->bad ? -1 : 0;
}

/* -------- routing -------------------------------------------------------- */

static int service_for_path(const char *path, const char *leaf)
{
    int svc;

    for (svc = 0; svc < DLNA_SVC_COUNT; svc++) {
        char want[96];

        snprintf(want, sizeof(want), "/%s/%s", dlna_svc_dir[svc], leaf);
        if (strcmp(path, want) == 0)
            return svc;
    }
    return -1;
}

static void split_soap_action(const char *hdr, char *service, size_t ssz,
                              char *action, size_t asz)
{
    const char *hash;
    size_t n;

    service[0] = '\0';
    action[0] = '\0';
    if (!hdr || !*hdr)
        return;
    hash = strchr(hdr, '#');
    if (!hash) {
        n = strlen(hdr);
        if (n >= ssz)
            n = ssz - 1;
        memcpy(service, hdr, n);
        service[n] = '\0';
        return;
    }
    n = (size_t)(hash - hdr);
    if (n >= ssz)
        n = ssz - 1;
    memcpy(service, hdr, n);
    service[n] = '\0';
    snprintf(action, asz, "%s", hash + 1);
}

static void handle_request(struct http_server *s, struct http_conn *c,
                           const struct http_req_info *ri)
{
    int svc;
    int status = 200;

    if (strcmp(ri->method, "GET") == 0) {
        if (strcmp(ri->path, "/description.xml") == 0) {
            int n = description_build(s->cfg, c->body, sizeof(c->body));

            if (n < 0) {
                resp_text(c, 500, "description too large");
                return;
            }
            resp_start(c, 200, "text/xml; charset=\"utf-8\"",
                       c->body, (size_t)n, NULL);
            return;
        }
        svc = service_for_path(ri->path, "scpd.xml");
        if (svc >= 0) {
            resp_start(c, 200, "text/xml; charset=\"utf-8\"",
                       scpd_get(svc), scpd_len(svc), NULL);
            return;
        }
        resp_text(c, 404, "not found");
        return;
    }

    if (strcmp(ri->method, "POST") == 0) {
        char service[96];
        char action[128];
        int n;

        svc = service_for_path(ri->path, "control");
        if (svc < 0) {
            resp_text(c, 404, "not found");
            return;
        }
        split_soap_action(ri->soap_action, service, sizeof(service),
                          action, sizeof(action));
        n = soap_handle(s->r, s->g, svc, service, action,
                        c->req + c->body_start,
                        c->body, sizeof(c->body), &status);
        if (n < 0) {
            resp_text(c, 500, "response too large");
            return;
        }
        resp_start(c, status, "text/xml; charset=\"utf-8\"",
                   c->body, (size_t)n, NULL);
        return;
    }

    if (strcmp(ri->method, "SUBSCRIBE") == 0 ||
        strcmp(ri->method, "UNSUBSCRIBE") == 0) {
        char sid[DLNA_GENA_SID_MAX];
        char extra[160];
        int timeout = DLNA_GENA_TIMEOUT_DEF;

        svc = service_for_path(ri->path, "event");
        if (svc < 0) {
            resp_text(c, 404, "not found");
            return;
        }

        if (strcmp(ri->method, "SUBSCRIBE") == 0) {
            int rc = gena_subscribe(s->g, svc, ri->callback, ri->nt,
                                    ri->timeout_hdr, ri->sid,
                                    sid, sizeof(sid), &timeout);

            if (rc != 200) {
                resp_text(c, rc, "subscription refused");
                return;
            }
            snprintf(extra, sizeof(extra),
                     "SID: %s\r\nTIMEOUT: Second-%d\r\n", sid, timeout);
            resp_start(c, 200, "text/plain; charset=\"utf-8\"",
                       NULL, 0, extra);
            return;
        }

        status = gena_unsubscribe(s->g, svc, ri->sid);
        if (status != 200) {
            resp_text(c, status, "unknown subscription");
            return;
        }
        resp_start(c, 200, "text/plain; charset=\"utf-8\"", NULL, 0, NULL);
        return;
    }

    resp_text(c, 405, "method not allowed");
}

/* -------- connection I/O ------------------------------------------------- */

static int read_request(struct http_server *s, struct http_conn *c,
                        int64_t now)
{
    (void)s;

    for (;;) {
        ssize_t n;

        if (c->req_len >= DLNA_REQ_MAX) {
            resp_text(c, 413, "request too large");
            c->deadline_ms = now + HTTP_SEND_TIMEOUT_MS;
            return 1;
        }
        n = recv(c->fd, c->req + c->req_len, DLNA_REQ_MAX - c->req_len, 0);
        if (n > 0) {
            c->req_len += (size_t)n;
            c->req[c->req_len] = '\0';
        } else if (n == 0) {
            if (!c->parsed || c->req_len < c->body_total)
                close_conn(c);
            return 0;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        } else {
            close_conn(c);
            return 0;
        }

        if (!c->parsed) {
            char *delim = strstr(c->req, "\r\n\r\n");

            if (!delim)
                continue;

            {
                size_t hdr_end = (size_t)(delim - c->req);

                c->body_start = hdr_end + 4;
                if (parse_request(c->req, hdr_end, &c->request) != 0) {
                    resp_text(c, 400, "bad request");
                    c->deadline_ms = now + HTTP_SEND_TIMEOUT_MS;
                    return 1;
                }
                c->parsed = 1;
                /* Bound the declared length *before* adding it to body_start.
                 * content_length comes from an unchecked strtoul(), and this
                 * addition is size_t: a length near SIZE_MAX wraps to a small
                 * value, sails past the 413 test below, and satisfies
                 * `req_len >= body_total`, so the request is dispatched with a
                 * truncated or empty body. Rejecting on the raw value closes
                 * both the wraparound and the oversized-body case. */
                if (c->request.content_length > DLNA_REQ_MAX) {
                    resp_text(c, 413, "request body too large");
                    c->deadline_ms = now + HTTP_SEND_TIMEOUT_MS;
                    return 1;
                }
                c->body_total = c->body_start + c->request.content_length;
                log_debug("http: request %s %s content-length=%zu body_total=%zu",
                          c->request.method, c->request.path,
                          c->request.content_length, c->body_total);
                if (c->body_total > DLNA_REQ_MAX) {
                    resp_text(c, 413, "request body too large");
                    c->deadline_ms = now + HTTP_SEND_TIMEOUT_MS;
                    return 1;
                }
                if (c->req_len >= c->body_total) {
                    handle_request(s, c, &c->request);
                    c->deadline_ms = now + HTTP_SEND_TIMEOUT_MS;
                    return 1;
                }
            }
        } else if (c->req_len >= c->body_total) {
            /* Body completed on a later read; the parsed headers are still in
             * c->request, so dispatch without touching the buffer again. */
            handle_request(s, c, &c->request);
            c->deadline_ms = now + HTTP_SEND_TIMEOUT_MS;
            return 1;
        }
    }
}

int http_init(struct http_server *s, const struct dlna_config *cfg,
              struct dlna_renderer *r, struct gena *g)
{
    struct sockaddr_in addr;
    int fd;
    int i;

    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->cfg = cfg;
    s->r = r;
    s->g = g;
    for (i = 0; i < DLNA_MAX_CLIENTS; i++)
        s->conns[i].fd = -1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("http: socket: %s", strerror(errno));
        return -1;
    }
    {
        int one = 1;

        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg->http_port);
    if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) != 1) {
        log_error("http: bad bind address '%s'", cfg->bind_addr);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("http: bind %s:%d: %s", cfg->bind_addr, cfg->http_port,
                  strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        log_error("http: listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    s->listen_fd = fd;
    log_info("http: listening on %s:%d", cfg->bind_addr, cfg->http_port);
    return 0;
}

void http_shutdown(struct http_server *s)
{
    int i;

    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    for (i = 0; i < DLNA_MAX_CLIENTS; i++) {
        if (s->conns[i].used)
            close_conn(&s->conns[i]);
    }
}

void http_accept(struct http_server *s, int64_t now)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(s->listen_fd, (struct sockaddr *)&peer, &plen);
        int i;

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            log_warn("http: accept: %s", strerror(errno));
            return;
        }
        set_nonblock(fd);

        for (i = 0; i < DLNA_MAX_CLIENTS; i++) {
            if (!s->conns[i].used)
                break;
        }
        if (i == DLNA_MAX_CLIENTS) {
            log_warn("http: connection pool full, dropping client");
            close(fd);
            continue;
        }

        memset(&s->conns[i], 0, sizeof(s->conns[i]));
        s->conns[i].fd = fd;
        s->conns[i].used = 1;
        s->conns[i].deadline_ms = now + HTTP_REQUEST_TIMEOUT_MS;
        log_debug("http: accepted client %d", i);
    }
}

int http_client_fd(const struct http_server *s, int i)
{
    return s->conns[i].fd;
}

short http_client_events(const struct http_server *s, int i)
{
    if (!s->conns[i].used)
        return 0;
    return s->conns[i].sending ? POLLOUT : POLLIN;
}

void http_client_ready(struct http_server *s, int i, short revents,
                       int64_t now)
{
    struct http_conn *c = &s->conns[i];

    if (!c->used)
        return;

    if (revents & POLLNVAL) {
        close_conn(c);
        return;
    }
    if (c->sending) {
        if (revents & (POLLOUT | POLLERR | POLLHUP))
            conn_send(c);
        return;
    }
    if (revents & (POLLERR | POLLHUP)) {
        /* A half-close can still carry a complete request; try one read. */
        if (!(revents & POLLIN)) {
            close_conn(c);
            return;
        }
    }
    if (revents & POLLIN) {
        (void)read_request(s, c, now);
        if (c->sending)
            conn_send(c);
    }
}

void http_tick(struct http_server *s, int64_t now)
{
    int i;

    for (i = 0; i < DLNA_MAX_CLIENTS; i++) {
        if (s->conns[i].used && now >= s->conns[i].deadline_ms) {
            log_debug("http: client %d timed out", i);
            close_conn(&s->conns[i]);
        }
    }
}

int http_next_timeout_ms(const struct http_server *s, int64_t now)
{
    int64_t best = 1000;
    int i;

    for (i = 0; i < DLNA_MAX_CLIENTS; i++) {
        if (s->conns[i].used) {
            int64_t d = s->conns[i].deadline_ms - now;

            if (d < best)
                best = d;
        }
    }
    if (best < 0)
        best = 0;
    return (int)best;
}
