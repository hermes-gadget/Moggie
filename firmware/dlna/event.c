#define _POSIX_C_SOURCE 200809L

#include "event.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* -------- event bodies -------------------------------------------------- */

static void build_avt_lastchange(struct dlna_renderer *r, struct sbuf *b)
{
    struct dlna_playback pb;
    const char *actions;
    char hms[24];
    int track;

    renderer_snapshot(r, &pb);
    track = renderer_has_uri(r) ? 1 : 0;
    switch (pb.state) {
    case DLNA_PLAYING: actions = "Stop,Pause,Seek"; break;
    case DLNA_PAUSED:  actions = "Play,Stop,Seek";  break;
    case DLNA_STOPPED: actions = track ? "Play,Seek" : "Play"; break;
    default:           actions = "";                break;
    }

    sb_puts(b, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">");
    sb_puts(b, "<InstanceID val=\"0\">");
    sb_printf(b, "<TransportState val=\"%s\"/>", dlna_transport_name(pb.state));
    sb_puts(b, "<TransportStatus val=\"OK\"/>");
    sb_printf(b, "<CurrentTransportActions val=\"%s\"/>", actions);
    sb_puts(b, "<AVTransportURI val=\"");
    sb_escape(b, renderer_uri(r));
    sb_puts(b, "\"/>");
    sb_puts(b, "<CurrentTrackURI val=\"");
    sb_escape(b, renderer_uri(r));
    sb_puts(b, "\"/>");
    format_hms(hms, sizeof(hms), pb.duration_s);
    sb_printf(b, "<CurrentTrackDuration val=\"%s\"/>", hms);
    format_hms(hms, sizeof(hms), pb.position_s);
    sb_printf(b, "<RelativeTimePosition val=\"%s\"/>", hms);
    sb_printf(b, "<CurrentTrack val=\"%d\"/>", track);
    sb_printf(b, "<NumberOfTracks val=\"%d\"/>", track);
    sb_puts(b, "</InstanceID></Event>");
}

static void build_rcs_lastchange(struct dlna_renderer *r, struct sbuf *b)
{
    struct dlna_playback pb;
    int db;

    renderer_snapshot(r, &pb);
    db = (pb.volume - 100) * 40 / 100;   /* -40..0 dB, matches the SCPD range */

    sb_puts(b, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\">");
    sb_puts(b, "<InstanceID val=\"0\">");
    sb_printf(b, "<Volume channel=\"Master\" val=\"%d\"/>", pb.volume);
    sb_printf(b, "<Mute channel=\"Master\" val=\"%d\"/>", pb.mute ? 1 : 0);
    sb_printf(b, "<VolumeDB channel=\"Master\" val=\"%d\"/>", db * 256);
    sb_puts(b, "</InstanceID></Event>");
}

static int build_property_set(struct gena *g, int service,
                              char *buf, size_t cap)
{
    struct sbuf b;
    char inner[DLNA_GENA_OUT_BUF];

    sb_init(&b, buf, cap);
    sb_puts(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    sb_puts(&b, "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">");

    if (service == DLNA_SVC_CM) {
        sb_puts(&b, "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>");
        sb_puts(&b, "<e:property><SinkProtocolInfo>");
        sb_escape(&b, DLNA_SINK_PROTOCOL_INFO);
        sb_puts(&b, "</SinkProtocolInfo></e:property>");
        sb_puts(&b, "<e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property>");
    } else {
        struct sbuf ib;

        sb_init(&ib, inner, sizeof(inner));
        if (service == DLNA_SVC_AVT)
            build_avt_lastchange(g->r, &ib);
        else
            build_rcs_lastchange(g->r, &ib);
        if (!sb_ok(&ib)) {
            log_error("event: LastChange body overflow");
            return -1;
        }
        sb_puts(&b, "<e:property><LastChange>");
        sb_escape(&b, inner);
        sb_puts(&b, "</LastChange></e:property>");
    }

    sb_puts(&b, "</e:propertyset>\n");
    if (!sb_ok(&b)) {
        log_error("event: property set overflow");
        return -1;
    }
    return (int)b.len;
}

/* -------- outbox -------------------------------------------------------- */

static struct gena_sub *find_sub(struct gena *g, const char *sid)
{
    int i;

    for (i = 0; i < DLNA_GENA_MAX_SUBS; i++) {
        if (g->subs[i].used && strcmp(g->subs[i].sid, sid) == 0)
            return &g->subs[i];
    }
    return NULL;
}

static void drop_sub(struct gena *g, struct gena_sub *s)
{
    log_warn("event: dropping subscriber %s after %d failed deliveries",
             s->sid, s->failures);
    memset(s, 0, sizeof(*s));
}

static void out_close(struct gena *g, struct gena_out *o)
{
    if (o->fd >= 0)
        close(o->fd);
    memset(o, 0, sizeof(*o));
    o->fd = -1;
}

static void out_fail(struct gena *g, struct gena_out *o, const char *why)
{
    struct gena_sub *s = find_sub(g, o->sid);

    log_debug("event: NOTIFY failed for %s: %s", o->sid, why);
    if (s) {
        s->failures++;
        if (s->failures >= DLNA_GENA_MAX_FAILURES)
            drop_sub(g, s);
    }
    out_close(g, o);
}

static void out_success(struct gena *g, struct gena_out *o)
{
    struct gena_sub *s = find_sub(g, o->sid);

    if (s)
        s->failures = 0;
    out_close(g, o);
}

static void out_try_send(struct gena *g, struct gena_out *o, int64_t now)
{
    while (o->sent < o->len) {
        ssize_t n = send(o->fd, o->buf + o->sent, o->len - o->sent,
                         MSG_NOSIGNAL);
        if (n > 0) {
            o->sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        out_fail(g, o, strerror(errno));
        return;
    }
    o->stage = 2;
    o->deadline_ms = now + DLNA_GENA_TIMEOUT_MS;

    /* The callback normally answers immediately; take it if it is there. */
    for (;;) {
        char tmp[256];
        ssize_t n = recv(o->fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            out_success(g, o);
            return;
        }
        if (n == 0) {
            out_success(g, o);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        out_fail(g, o, strerror(errno));
        return;
    }
}

static int queue_event(struct gena *g, struct gena_sub *s,
                       const char *body, size_t body_len, int64_t now)
{
    struct gena_out *o = NULL;
    char hdr[512];
    char host[64];
    int hn;
    int i;
    int fd;
    int rc;

    for (i = 0; i < DLNA_GENA_MAX_OUT; i++) {
        if (!g->out[i].used) {
            o = &g->out[i];
            break;
        }
    }
    if (!o) {
        log_warn("event: no NOTIFY slot free, dropping event for %s", s->sid);
        return -1;
    }

    if (!inet_ntop(AF_INET, &s->addr.sin_addr, host, sizeof(host)))
        snprintf(host, sizeof(host), "127.0.0.1");

    hn = snprintf(hdr, sizeof(hdr),
        "NOTIFY %s HTTP/1.1\r\n"
        "HOST: %s:%u\r\n"
        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
        "CONTENT-LENGTH: %zu\r\n"
        "NT: upnp:event\r\n"
        "NTS: upnp:propchange\r\n"
        "SID: %s\r\n"
        "SEQ: %u\r\n"
        "CONNECTION: close\r\n"
        "\r\n",
        s->path, host, (unsigned)ntohs(s->addr.sin_port),
        body_len, s->sid, s->seq);
    if (hn < 0 || (size_t)hn + body_len >= sizeof(o->buf)) {
        log_error("event: NOTIFY too large (%zu body)", body_len);
        return -1;
    }

    memset(o, 0, sizeof(*o));
    o->used = 1;
    o->fd = -1;
    memcpy(o->buf, hdr, (size_t)hn);
    memcpy(o->buf + hn, body, body_len);
    o->len = (size_t)hn + body_len;
    o->sent = 0;
    o->stage = 0;
    o->deadline_ms = now + DLNA_GENA_TIMEOUT_MS;
    snprintf(o->sid, sizeof(o->sid), "%s", s->sid);

    s->seq++;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        out_fail(g, o, "socket");
        return -1;
    }
    set_nonblock(fd);
    o->fd = fd;

    rc = connect(fd, (struct sockaddr *)&s->addr, sizeof(s->addr));
    if (rc == 0) {
        o->stage = 1;
        out_try_send(g, o, now);
    } else if (errno != EINPROGRESS) {
        out_fail(g, o, strerror(errno));
        return -1;
    }
    return 0;
}

/* -------- subscriptions ------------------------------------------------- */

static int parse_timeout(const char *hdr, int fallback)
{
    const char *p;
    char *end;
    long v;

    if (!hdr || !*hdr)
        return fallback;
    p = hdr;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncasecmp(p, "Second-", 7) != 0)
        return fallback;
    p += 7;
    if (strncasecmp(p, "infinite", 8) == 0)
        return DLNA_GENA_TIMEOUT_MAX;
    v = strtol(p, &end, 10);
    if (end == p || *end != '\0' || v <= 0)
        return fallback;
    if (v < DLNA_GENA_TIMEOUT_MIN)
        v = DLNA_GENA_TIMEOUT_MIN;
    if (v > DLNA_GENA_TIMEOUT_MAX)
        v = DLNA_GENA_TIMEOUT_MAX;
    return (int)v;
}

static int parse_callback(const char *hdr, struct sockaddr_in *addr,
                          char *path, size_t pathsz,
                          char *display, size_t displaysz)
{
    char tmp[DLNA_GENA_CALLBACK_MAX];
    char host[128];
    char *p;
    char *end;
    char *slash;
    char *colon;
    char *portstr = NULL;
    int port = 80;
    struct addrinfo hints;
    struct addrinfo *res = NULL;

    if (header_value(hdr, tmp, sizeof(tmp)) != 0)
        return -1;

    p = tmp;
    if (*p == '<') {
        end = strchr(p, '>');
        if (!end)
            return -1;
        *end = '\0';
        p++;
        end = strchr(p, ' ');
        if (end)
            *end = '\0';
    }
    if (strncasecmp(p, "http://", 7) != 0)
        return -1;
    p += 7;

    slash = strchr(p, '/');
    if (slash) {
        *slash = '\0';
        snprintf(path, pathsz, "/%s", slash + 1);
    } else {
        snprintf(path, pathsz, "/");
    }
    if (!path[0])
        snprintf(path, pathsz, "/");

    colon = strrchr(p, ':');
    if (colon) {
        char *port_end;
        long parsed;

        *colon = '\0';
        portstr = colon + 1;
        parsed = strtol(portstr, &port_end, 10);
        if (port_end == portstr || *port_end != '\0' ||
            parsed <= 0 || parsed > 65535)
            return -1;
        port = (int)parsed;
    }
    if (!p[0] || strlen(p) >= sizeof(host))
        return -1;
    snprintf(host, sizeof(host), "%s", p);

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
            return -1;
        addr->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    snprintf(display, displaysz, "%s:%d%s", host, port, path);
    return 0;
}

static void make_sid(struct gena *g, char *out, size_t outsz)
{
    int64_t ms = now_ms();
    unsigned counter = ++g->sid_counter;

    snprintf(out, outsz, "uuid:%08x-%04x-4%03x-%04x-%012llx",
             (unsigned)g->cfg->boot_id, counter & 0xffffu,
             counter & 0x0fffu,
             0x8000u | (unsigned)(ms & 0x3fffu),
             (unsigned long long)(ms * 2654435761ULL) & 0xffffffffffffULL);
}

void gena_init(struct gena *g, const struct dlna_config *cfg,
               struct dlna_renderer *r)
{
    int i;

    memset(g, 0, sizeof(*g));
    g->cfg = cfg;
    g->r = r;
    for (i = 0; i < DLNA_GENA_MAX_OUT; i++)
        g->out[i].fd = -1;
}

void gena_shutdown(struct gena *g)
{
    int i;

    for (i = 0; i < DLNA_GENA_MAX_OUT; i++) {
        if (g->out[i].used)
            out_close(g, &g->out[i]);
    }
}

int gena_subscribe(struct gena *g, int service,
                   const char *callback, const char *nt,
                   const char *timeout_hdr, const char *sid_hdr,
                   char *sid_out, size_t sid_outsz, int *timeout_out)
{
    struct gena_sub *s;
    struct sockaddr_in addr;
    char path[128];
    char display[DLNA_GENA_CALLBACK_MAX];
    char sid[DLNA_GENA_SID_MAX];
    int timeout;
    int i;
    int64_t now = now_ms();
    char body[DLNA_GENA_OUT_BUF];
    int blen;

    if (!sid_out || sid_outsz == 0)
        return 412;

    /* Renewal: SID only. */
    if (sid_hdr && *sid_hdr) {
        s = find_sub(g, sid_hdr);
        if (!s || s->service != service)
            return 412;
        timeout = parse_timeout(timeout_hdr, s->timeout_s);
        s->timeout_s = timeout;
        s->expires_ms = now + (int64_t)timeout * 1000;
        snprintf(sid_out, sid_outsz, "%s", s->sid);
        if (timeout_out)
            *timeout_out = timeout;
        log_debug("event: renewed %s for %ds", s->sid, timeout);
        return 200;
    }

    if (!callback || !*callback)
        return 412;
    if (!nt || !str_ieq(nt, "upnp:event"))
        return 412;
    if (parse_callback(callback, &addr, path, sizeof(path),
                       display, sizeof(display)) != 0) {
        log_warn("event: bad CALLBACK '%s'", callback);
        return 412;
    }

    s = NULL;
    for (i = 0; i < DLNA_GENA_MAX_SUBS; i++) {
        if (!g->subs[i].used) {
            s = &g->subs[i];
            break;
        }
    }
    if (!s) {
        log_warn("event: subscriber table full");
        return 503;
    }

    timeout = parse_timeout(timeout_hdr, DLNA_GENA_TIMEOUT_DEF);
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->service = service;
    s->addr = addr;
    snprintf(s->path, sizeof(s->path), "%s", path);
    snprintf(s->callback, sizeof(s->callback), "%s", display);
    s->timeout_s = timeout;
    s->expires_ms = now + (int64_t)timeout * 1000;
    s->seq = 0;
    s->failures = 0;
    make_sid(g, sid, sizeof(sid));
    snprintf(s->sid, sizeof(s->sid), "%s", sid);

    snprintf(sid_out, sid_outsz, "%s", s->sid);
    if (timeout_out)
        *timeout_out = timeout;
    log_info("event: %s subscribed to %s for %ds",
             s->sid, dlna_svc_dir[service], timeout);

    blen = build_property_set(g, service, body, sizeof(body));
    if (blen > 0)
        (void)queue_event(g, s, body, (size_t)blen, now);
    return 200;
}

int gena_unsubscribe(struct gena *g, int service, const char *sid)
{
    int i;

    if (!sid || !*sid)
        return 412;
    for (i = 0; i < DLNA_GENA_MAX_SUBS; i++) {
        struct gena_sub *s = &g->subs[i];

        if (s->used && s->service == service && strcmp(s->sid, sid) == 0) {
            log_info("event: %s unsubscribed from %s",
                     s->sid, dlna_svc_dir[service]);
            memset(s, 0, sizeof(*s));
            return 200;
        }
    }
    return 412;
}

/* -------- publication --------------------------------------------------- */

void gena_flush(struct gena *g)
{
    unsigned mask = renderer_take_dirty(g->r);
    int svc;
    int i;

    if (!mask)
        return;

    for (svc = 0; svc < DLNA_SVC_COUNT; svc++) {
        char body[DLNA_GENA_OUT_BUF];
        int blen;

        if ((mask & (1u << (unsigned)svc)) == 0)
            continue;
        blen = build_property_set(g, svc, body, sizeof(body));
        if (blen <= 0)
            continue;
        for (i = 0; i < DLNA_GENA_MAX_SUBS; i++) {
            if (g->subs[i].used && g->subs[i].service == svc)
                (void)queue_event(g, &g->subs[i], body, (size_t)blen,
                                  now_ms());
        }
    }
}

void gena_tick(struct gena *g, int64_t now)
{
    int i;

    for (i = 0; i < DLNA_GENA_MAX_OUT; i++) {
        struct gena_out *o = &g->out[i];

        if (o->used && now >= o->deadline_ms)
            out_fail(g, o, "timeout");
    }
    for (i = 0; i < DLNA_GENA_MAX_SUBS; i++) {
        struct gena_sub *s = &g->subs[i];

        if (s->used && now >= s->expires_ms) {
            log_info("event: subscription %s expired", s->sid);
            memset(s, 0, sizeof(*s));
        }
    }
}

int gena_next_timeout_ms(const struct gena *g, int64_t now)
{
    int64_t best = 1000;
    int i;

    for (i = 0; i < DLNA_GENA_MAX_OUT; i++) {
        if (g->out[i].used) {
            int64_t d = g->out[i].deadline_ms - now;

            if (d < best)
                best = d;
        }
    }
    for (i = 0; i < DLNA_GENA_MAX_SUBS; i++) {
        if (g->subs[i].used) {
            int64_t d = g->subs[i].expires_ms - now;

            if (d < best)
                best = d;
        }
    }
    if (best < 0)
        best = 0;
    return (int)best;
}

/* -------- poll integration ---------------------------------------------- */

int gena_out_fd(const struct gena *g, int i)
{
    return g->out[i].fd;
}

int gena_out_used(const struct gena *g, int i)
{
    return g->out[i].used;
}

short gena_out_events(const struct gena *g, int i)
{
    const struct gena_out *o = &g->out[i];

    if (!o->used)
        return 0;
    if (o->stage == 0 || o->stage == 1)
        return POLLOUT;
    return POLLIN;
}

void gena_out_ready(struct gena *g, int i, short revents, int64_t now)
{
    struct gena_out *o = &g->out[i];
    int err = 0;
    socklen_t errlen = sizeof(err);

    if (!o->used)
        return;

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (o->stage == 2 && (revents & POLLHUP)) {
            out_success(g, o);
            return;
        }
        (void)getsockopt(o->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        out_fail(g, o, err ? strerror(err) : "hangup");
        return;
    }

    if (o->stage == 0 && (revents & POLLOUT)) {
        if (getsockopt(o->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 ||
            err != 0) {
            out_fail(g, o, err ? strerror(err) : "connect");
            return;
        }
        o->stage = 1;
        out_try_send(g, o, now);
        return;
    }

    if (o->stage == 1 && (revents & POLLOUT)) {
        out_try_send(g, o, now);
        return;
    }

    if (o->stage == 2 && (revents & POLLIN)) {
        char tmp[256];
        ssize_t n = recv(o->fd, tmp, sizeof(tmp), 0);

        if (n > 0 || n == 0)
            out_success(g, o);
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            out_fail(g, o, strerror(errno));
    }
}
