#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

const char *const dlna_svc_type[DLNA_SVC_COUNT] = {
    "urn:schemas-upnp-org:service:AVTransport:1",
    "urn:schemas-upnp-org:service:RenderingControl:1",
    "urn:schemas-upnp-org:service:ConnectionManager:1"
};

const char *const dlna_svc_id[DLNA_SVC_COUNT] = {
    "urn:upnp-org:serviceId:AVTransport",
    "urn:upnp-org:serviceId:RenderingControl",
    "urn:upnp-org:serviceId:ConnectionManager"
};

const char *const dlna_svc_dir[DLNA_SVC_COUNT] = {
    "AVTransport",
    "RenderingControl",
    "ConnectionManager"
};

static uint64_t fnv1a(const char *s, uint64_t seed)
{
    uint64_t h = seed;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void uuid_from_hash(const char *material, uint64_t seed,
                           char *out, size_t outsz)
{
    uint64_t a = fnv1a(material, seed);
    uint64_t b = fnv1a(material, seed ^ 0x9e3779b97f4a7c15ULL);
    unsigned char bytes[16];
    int i;

    for (i = 0; i < 8; i++)
        bytes[i] = (unsigned char)(a >> (56 - 8 * i));
    for (i = 0; i < 8; i++)
        bytes[8 + i] = (unsigned char)(b >> (56 - 8 * i));

    bytes[6] = (unsigned char)((bytes[6] & 0x0f) | 0x50); /* version 5 shape */
    bytes[8] = (unsigned char)((bytes[8] & 0x3f) | 0x80); /* RFC 4122 variant */

    snprintf(out, outsz,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
}

int dlna_uuid_normalise(const char *in, char *out, size_t outsz)
{
    char tmp[DLNA_MAX_UUID];
    size_t n;
    size_t i;

    if (!in || !*in)
        return -1;
    while (*in == ' ' || *in == '\t')
        in++;
    if (strncasecmp(in, "uuid:", 5) == 0)
        in += 5;
    n = strlen(in);
    if (n != 36)
        return -1;
    for (i = 0; i < n; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (in[i] != '-')
                return -1;
        } else if (!isxdigit((unsigned char)in[i])) {
            return -1;
        }
    }
    if (n >= sizeof(tmp))
        return -1;
    for (i = 0; i < n; i++)
        tmp[i] = (char)tolower((unsigned char)in[i]);
    tmp[n] = '\0';
    if (n + 1 > outsz)
        return -1;
    memcpy(out, tmp, n + 1);
    return 0;
}

int dlna_uuid_from_mac(char *out, size_t outsz)
{
    struct ifaddrs *ifa0 = NULL;
    struct ifaddrs *ifa;
    char mac[96];
    int found = 0;

    if (getifaddrs(&ifa0) != 0)
        return -1;

    for (ifa = ifa0; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_ll *sll;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET)
            continue;
        if (strcmp(ifa->ifa_name, "lo") == 0)
            continue;
        sll = (struct sockaddr_ll *)ifa->ifa_addr;
        if (sll->sll_halen != 6)
            continue;
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 sll->sll_addr[0], sll->sll_addr[1], sll->sll_addr[2],
                 sll->sll_addr[3], sll->sll_addr[4], sll->sll_addr[5]);
        found = 1;
        break;
    }
    freeifaddrs(ifa0);

    if (!found) {
        char host[64];

        if (gethostname(host, sizeof(host)) != 0)
            return -1;
        host[sizeof(host) - 1] = '\0';
        snprintf(mac, sizeof(mac), "host:%s", host);
    }

    uuid_from_hash(mac, 1469598103934665603ULL, out, outsz);
    return 0;
}

int dlna_uuid_random(char *out, size_t outsz)
{
    struct timespec ts;
    char material[96];

    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(material, sizeof(material), "%lld.%ld:%ld:%d",
             (long long)ts.tv_sec, ts.tv_nsec, (long)getpid(),
             (int)(now_ms() & 0xffff));
    uuid_from_hash(material, 0xcbf29ce484222325ULL, out, outsz);
    return 0;
}

int dlna_detect_ip(char *out, size_t outsz)
{
    struct ifaddrs *ifa0 = NULL;
    struct ifaddrs *ifa;
    char fallback[DLNA_MAX_IP] = "";

    if (getifaddrs(&ifa0) != 0)
        return -1;

    for (ifa = ifa0; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;
        char buf[DLNA_MAX_IP];

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, "lo") == 0)
            continue;
        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            continue;
        if (strncmp(buf, "169.254.", 8) == 0) {
            if (!fallback[0])
                snprintf(fallback, sizeof(fallback), "%s", buf);
            continue;
        }
        snprintf(out, outsz, "%s", buf);
        freeifaddrs(ifa0);
        return 0;
    }

    if (fallback[0]) {
        snprintf(out, outsz, "%s", fallback);
        freeifaddrs(ifa0);
        return 0;
    }
    freeifaddrs(ifa0);
    return -1;
}

int dlna_config_init(struct dlna_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->bind_addr, sizeof(cfg->bind_addr), "0.0.0.0");
    cfg->http_port = DLNA_HTTP_PORT_DEFAULT;
    cfg->ssdp_port = DLNA_SSDP_PORT_DEFAULT;
    cfg->cache_control = DLNA_CACHE_CONTROL;
    cfg->announce_interval = DLNA_CACHE_CONTROL / 2;
    snprintf(cfg->friendly_name, sizeof(cfg->friendly_name), "Moggie Play:5");
    cfg->log_level = DLNA_LOG_INFO;
    cfg->boot_id = (int)time(NULL);

    if (dlna_uuid_from_mac(cfg->uuid, sizeof(cfg->uuid)) != 0)
        dlna_uuid_random(cfg->uuid, sizeof(cfg->uuid));
    return 0;
}

static int parse_int(const char *s, int min, int max, int *out)
{
    char *end;
    long v;

    if (!s || !*s)
        return -1;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

void dlna_config_usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --http-port N          HTTP port (default %d)\n"
        "  --http-addr ADDR       HTTP bind address (default 0.0.0.0)\n"
        "  --ssdp-port N          SSDP port (default %d)\n"
        "  --ip ADDR              address to advertise in LOCATION\n"
        "  --uuid UUID            device UUID (default: derived from MAC)\n"
        "  --friendly-name NAME   friendlyName (default \"Moggie Play:5\")\n"
        "  --cache-control N      SSDP max-age seconds (default %d)\n"
        "  --announce-interval N  seconds between alive rounds (default %d)\n"
        "  --backend NAME         stub | mock-sonosd (default stub)\n"
        "  --mock-deny            mock backend reports the audio path busy\n"
        "  --log-level N          0=error 1=warn 2=info 3=debug\n"
        "  --version              print version and exit\n"
        "  --help                 this text\n",
        argv0, DLNA_HTTP_PORT_DEFAULT, DLNA_SSDP_PORT_DEFAULT,
        DLNA_CACHE_CONTROL, DLNA_CACHE_CONTROL / 2);
}

int dlna_config_parse(struct dlna_config *cfg, int argc, char **argv)
{
    int i;
    int announce_given = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            dlna_config_usage(argv[0]);
            return 1;
        }
        if (strcmp(a, "--version") == 0) {
            printf("moggie-dlna %s\n", DLNA_VERSION);
            return 1;
        }
        if (strcmp(a, "--mock-deny") == 0) {
            cfg->mock_deny = 1;
            continue;
        }
        if (!v) {
            log_error("%s needs a value", a);
            return -1;
        }

        if (strcmp(a, "--http-port") == 0) {
            if (parse_int(v, 1, 65535, &cfg->http_port) != 0) {
                log_error("bad --http-port '%s'", v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--http-addr") == 0) {
            if (strlen(v) >= sizeof(cfg->bind_addr)) {
                log_error("--http-addr too long");
                return -1;
            }
            snprintf(cfg->bind_addr, sizeof(cfg->bind_addr), "%s", v);
            i++;
        } else if (strcmp(a, "--ssdp-port") == 0) {
            if (parse_int(v, 1, 65535, &cfg->ssdp_port) != 0) {
                log_error("bad --ssdp-port '%s'", v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--ip") == 0) {
            struct in_addr addr;

            if (strlen(v) >= sizeof(cfg->advertise_ip) ||
                inet_pton(AF_INET, v, &addr) != 1) {
                log_error("bad --ip '%s' (want an IPv4 address)", v);
                return -1;
            }
            snprintf(cfg->advertise_ip, sizeof(cfg->advertise_ip), "%s", v);
            i++;
        } else if (strcmp(a, "--uuid") == 0) {
            if (dlna_uuid_normalise(v, cfg->uuid, sizeof(cfg->uuid)) != 0) {
                log_error("bad --uuid '%s' (want 8-4-4-4-12 hex)", v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--friendly-name") == 0) {
            if (strlen(v) >= sizeof(cfg->friendly_name)) {
                log_error("--friendly-name too long (max %zu)",
                          sizeof(cfg->friendly_name) - 1);
                return -1;
            }
            snprintf(cfg->friendly_name, sizeof(cfg->friendly_name), "%s", v);
            i++;
        } else if (strcmp(a, "--cache-control") == 0) {
            if (parse_int(v, DLNA_MAX_AGE_MIN, DLNA_MAX_AGE_MAX,
                          &cfg->cache_control) != 0) {
                log_error("bad --cache-control '%s'", v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--announce-interval") == 0) {
            if (parse_int(v, 1, DLNA_MAX_AGE_MAX, &cfg->announce_interval) != 0) {
                log_error("bad --announce-interval '%s'", v);
                return -1;
            }
            announce_given = 1;
            i++;
        } else if (strcmp(a, "--backend") == 0) {
            if (strcmp(v, "stub") == 0) {
                cfg->backend_mock = 0;
            } else if (strcmp(v, "mock-sonosd") == 0) {
                cfg->backend_mock = 1;
            } else {
                log_error("unknown backend '%s' (stub|mock-sonosd)", v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--log-level") == 0) {
            if (parse_int(v, 0, 3, &cfg->log_level) != 0) {
                log_error("bad --log-level '%s'", v);
                return -1;
            }
            i++;
        } else {
            log_error("unknown option '%s'", a);
            return -1;
        }
    }

    if (!announce_given) {
        cfg->announce_interval = cfg->cache_control / 2;
        if (cfg->announce_interval < 1)
            cfg->announce_interval = 1;
    }
    if (cfg->announce_interval > cfg->cache_control)
        cfg->announce_interval = cfg->cache_control;
    if (cfg->announce_interval < 1)
        cfg->announce_interval = 1;

    if (!cfg->advertise_ip[0]) {
        if (dlna_detect_ip(cfg->advertise_ip,
                           sizeof(cfg->advertise_ip)) != 0) {
            log_error("no IPv4 address found; pass --ip");
            return -1;
        }
    }
    return 0;
}
