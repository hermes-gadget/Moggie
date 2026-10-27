/*
 * ssdp.c -- discovery.
 *
 * One UDP socket, bound to the SSDP port and joined to the SSDP multicast
 * group. It answers M-SEARCH with an MX-delayed unicast reply, announces
 * ssdp:alive at startup and before max-age expires, and says ssdp:byebye on
 * shutdown. If the group cannot be joined (some containers forbid it) the
 * daemon keeps running: unicast M-SEARCH still works and the failure is loud.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "ssdp.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SSDP_GROUP "239.255.255.250"

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void http_date(char *out, size_t outsz)
{
    time_t t = time(NULL);
    struct tm tm;

    gmtime_r(&t, &tm);
    strftime(out, outsz, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static void build_targets(struct ssdp *s)
{
    const struct dlna_config *cfg = s->cfg;

    s->count = 0;
    snprintf(s->nt[s->count], sizeof(s->nt[0]), "upnp:rootdevice");
    snprintf(s->usn[s->count], sizeof(s->usn[0]), "uuid:%s::upnp:rootdevice",
             cfg->uuid);
    s->count++;

    snprintf(s->nt[s->count], sizeof(s->nt[0]), "uuid:%s", cfg->uuid);
    snprintf(s->usn[s->count], sizeof(s->usn[0]), "uuid:%s", cfg->uuid);
    s->count++;

    snprintf(s->nt[s->count], sizeof(s->nt[0]),
             "urn:schemas-upnp-org:device:MediaRenderer:1");
    snprintf(s->usn[s->count], sizeof(s->usn[0]),
             "uuid:%s::urn:schemas-upnp-org:device:MediaRenderer:1",
             cfg->uuid);
    s->count++;

    snprintf(s->nt[s->count], sizeof(s->nt[0]), "%s", dlna_svc_type[DLNA_SVC_AVT]);
    snprintf(s->usn[s->count], sizeof(s->usn[0]), "uuid:%s::%s",
             cfg->uuid, dlna_svc_type[DLNA_SVC_AVT]);
    s->count++;

    snprintf(s->nt[s->count], sizeof(s->nt[0]), "%s", dlna_svc_type[DLNA_SVC_RCS]);
    snprintf(s->usn[s->count], sizeof(s->usn[0]), "uuid:%s::%s",
             cfg->uuid, dlna_svc_type[DLNA_SVC_RCS]);
    s->count++;

    snprintf(s->nt[s->count], sizeof(s->nt[0]), "%s", dlna_svc_type[DLNA_SVC_CM]);
    snprintf(s->usn[s->count], sizeof(s->usn[0]), "uuid:%s::%s",
             cfg->uuid, dlna_svc_type[DLNA_SVC_CM]);
    s->count++;
}

static void send_to(struct ssdp *s, const struct sockaddr_in *to,
                    const char *buf, size_t len)
{
    ssize_t n = sendto(s->fd, buf, len, 0,
                       (const struct sockaddr *)to, sizeof(*to));

    if (n < 0)
        log_debug("ssdp: sendto: %s", strerror(errno));
}

static void send_notify(struct ssdp *s, int target, int alive)
{
    struct sockaddr_in to;
    char buf[768];
    int n;

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)s->cfg->ssdp_port);
    (void)inet_pton(AF_INET, SSDP_GROUP, &to.sin_addr);

    if (alive) {
        n = snprintf(buf, sizeof(buf),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "CACHE-CONTROL: max-age=%d\r\n"
            "LOCATION: http://%s:%d/description.xml\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:alive\r\n"
            "SERVER: %s\r\n"
            "USN: %s\r\n"
            "BOOTID.UPNP.ORG: %d\r\n"
            "CONFIGID.UPNP.ORG: 1\r\n"
            "\r\n",
            SSDP_GROUP, s->cfg->ssdp_port, s->cfg->cache_control,
            s->cfg->advertise_ip, s->cfg->http_port,
            s->nt[target], DLNA_SERVER, s->usn[target], s->cfg->boot_id);
    } else {
        n = snprintf(buf, sizeof(buf),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:byebye\r\n"
            "USN: %s\r\n"
            "BOOTID.UPNP.ORG: %d\r\n"
            "CONFIGID.UPNP.ORG: 1\r\n"
            "\r\n",
            SSDP_GROUP, s->cfg->ssdp_port, s->nt[target],
            s->usn[target], s->cfg->boot_id);
    }
    if (n < 0 || (size_t)n >= sizeof(buf))
        return;
    send_to(s, &to, buf, (size_t)n);
}

static void send_search_response(struct ssdp *s, int target,
                                 const struct sockaddr_in *to)
{
    char buf[768];
    char date[40];
    int n;

    http_date(date, sizeof(date));
    n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=%d\r\n"
        "DATE: %s\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%d/description.xml\r\n"
        "SERVER: %s\r\n"
        "ST: %s\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n",
        s->cfg->cache_control, date, s->cfg->advertise_ip,
        s->cfg->http_port, DLNA_SERVER, s->nt[target], s->usn[target],
        s->cfg->boot_id);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return;
    send_to(s, to, buf, (size_t)n);
}

static int match_targets(struct ssdp *s, const char *st, int *out)
{
    int n = 0;
    int i;

    if (strcmp(st, "ssdp:all") == 0) {
        for (i = 0; i < s->count; i++)
            out[n++] = i;
        return n;
    }
    if (strcmp(st, "upnp:rootdevice") == 0) {
        out[n++] = 0;
        return n;
    }
    if (strcmp(st, s->nt[1]) == 0) {
        out[n++] = 1;
        return n;
    }
    for (i = 2; i < s->count; i++) {
        if (strcmp(st, s->nt[i]) == 0) {
            out[n++] = i;
            return n;
        }
    }
    return 0;
}

static int header_lookup(const char *req, const char *name, char *out,
                         size_t outsz)
{
    const char *p = strstr(req, "\r\n");
    size_t nlen = strlen(name);

    while (p) {
        const char *line = p + 2;
        const char *end = strstr(line, "\r\n");
        const char *colon;

        if (!end)
            return -1;
        if (end == line)
            return -1;   /* blank line ends the header block */
        colon = memchr(line, ':', (size_t)(end - line));
        if (colon) {
            size_t n = (size_t)(colon - line);

            if (n == nlen && strncasecmp(line, name, nlen) == 0) {
                const char *v = colon + 1;
                size_t vn;

                while (v < end && (*v == ' ' || *v == '\t'))
                    v++;
                vn = (size_t)(end - v);
                while (vn > 0 && (v[vn - 1] == ' ' || v[vn - 1] == '\t'))
                    vn--;
                if (vn >= outsz)
                    vn = outsz - 1;
                memcpy(out, v, vn);
                out[vn] = '\0';
                return 0;
            }
        }
        p = end;
    }
    return -1;
}

static void handle_search(struct ssdp *s, const char *req,
                          const struct sockaddr_in *from, int64_t now)
{
    char st[128];
    char man[64];
    char mxbuf[16];
    int targets[SSDP_TARGETS];
    int ntargets;
    int mx = 1;
    int i;
    int64_t window;

    if (strncmp(req, "M-SEARCH * HTTP/1.", 18) != 0)
        return;
    /* MAN must be exactly "ssdp:discover" (quoted in the header). A substring
     * match accepted anything containing it, e.g. "xssdp:discovery", so a
     * malformed search was answered as if it were well formed. */
    if (header_lookup(req, "MAN", man, sizeof(man)) != 0)
        return;
    {
        char *m = man;
        char *e;

        while (*m == ' ' || *m == '\t') m++;
        e = m + strlen(m);
        while (e > m && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
        *e = '\0';
        if (m[0] == '"' && e > m + 1 && e[-1] == '"') {
            m++;
            e[-1] = '\0';
        }
        if (strcasecmp(m, "ssdp:discover") != 0)
            return;
    }
    if (header_lookup(req, "ST", st, sizeof(st)) != 0 || !st[0])
        return;

    if (header_lookup(req, "MX", mxbuf, sizeof(mxbuf)) == 0) {
        char *end;
        long v = strtol(mxbuf, &end, 10);

        if (end != mxbuf && *end == '\0' && v > 0)
            mx = (v > 5) ? 5 : (int)v;
    }
    if (mx > 3)
        mx = 3;   /* do not keep a control point waiting longer than this */
    window = (int64_t)mx * 1000;

    ntargets = match_targets(s, st, targets);
    if (ntargets == 0)
        return;

    for (i = 0; i < ntargets; i++) {
        int slot;
        int64_t delay;

        for (slot = 0; slot < DLNA_SSDP_MAX_REPLIES; slot++) {
            if (!s->replies[slot].used)
                break;
        }
        if (slot == DLNA_SSDP_MAX_REPLIES) {
            log_debug("ssdp: reply queue full");
            return;
        }
        delay = window > 0 ? (int64_t)(rand() % (int)(window + 1)) : 0;
        s->replies[slot].used = 1;
        s->replies[slot].due_ms = now + delay;
        s->replies[slot].to = *from;
        s->replies[slot].target = targets[i];
    }
    log_debug("ssdp: M-SEARCH for '%s' from %s (%d target%s, MX %d)",
              st, inet_ntoa(from->sin_addr), ntargets,
              ntargets == 1 ? "" : "s", mx);
}

int ssdp_init(struct ssdp *s, const struct dlna_config *cfg)
{
    struct sockaddr_in addr;
    struct ip_mreq mreq;
    int one = 1;
    int ttl = 2;
    int loop = 1;
    int fd;

    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->cfg = cfg;
    build_targets(s);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_error("ssdp: socket: %s", strerror(errno));
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg->ssdp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("ssdp: bind port %d: %s", cfg->ssdp_port, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    (void)inet_pton(AF_INET, SSDP_GROUP, &mreq.imr_multiaddr);
    if (inet_pton(AF_INET, cfg->advertise_ip, &mreq.imr_interface) != 1)
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                   sizeof(mreq)) != 0) {
        log_warn("ssdp: could not join %s on %s: %s (unicast M-SEARCH still works)",
                 SSDP_GROUP, cfg->advertise_ip, strerror(errno));
    } else {
        log_info("ssdp: joined %s on %s", SSDP_GROUP, cfg->advertise_ip);
    }

    {
        struct in_addr out_if;

        if (inet_pton(AF_INET, cfg->advertise_ip, &out_if) == 1)
            (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &out_if,
                             sizeof(out_if));
    }

    set_nonblock(fd);
    s->fd = fd;
    srand((unsigned)(time(NULL) ^ (long)getpid()));
    log_info("ssdp: listening on port %d", cfg->ssdp_port);
    ssdp_announce(s, now_ms());
    return 0;
}

void ssdp_shutdown(struct ssdp *s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

void ssdp_announce(struct ssdp *s, int64_t now)
{
    int i;

    for (i = 0; i < s->count; i++)
        send_notify(s, i, 1);
    s->next_announce_ms = now + (int64_t)s->cfg->announce_interval * 1000;
    log_debug("ssdp: alive round (%d targets), next in %ds",
              s->count, s->cfg->announce_interval);
}

void ssdp_byebye(struct ssdp *s)
{
    int i;

    for (i = 0; i < s->count; i++)
        send_notify(s, i, 0);
    log_info("ssdp: byebye sent");
}

void ssdp_read(struct ssdp *s, int64_t now)
{
    for (;;) {
        char buf[2048];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(s->fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                log_debug("ssdp: recvfrom: %s", strerror(errno));
            return;
        }
        buf[n] = '\0';
        handle_search(s, buf, &from, now);
    }
}

void ssdp_tick(struct ssdp *s, int64_t now)
{
    int i;

    if (now >= s->next_announce_ms)
        ssdp_announce(s, now);

    for (i = 0; i < DLNA_SSDP_MAX_REPLIES; i++) {
        if (s->replies[i].used && now >= s->replies[i].due_ms) {
            send_search_response(s, s->replies[i].target,
                                 &s->replies[i].to);
            s->replies[i].used = 0;
        }
    }
}

int ssdp_next_timeout_ms(const struct ssdp *s, int64_t now)
{
    int64_t best = 1000;
    int i;

    if (s->next_announce_ms - now < best)
        best = s->next_announce_ms - now;
    for (i = 0; i < DLNA_SSDP_MAX_REPLIES; i++) {
        if (s->replies[i].used) {
            int64_t d = s->replies[i].due_ms - now;

            if (d < best)
                best = d;
        }
    }
    if (best < 0)
        best = 0;
    return (int)best;
}
