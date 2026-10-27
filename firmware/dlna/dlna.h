/*
 * dlna.h -- shared constants and types for the Moggie UPnP AV MediaRenderer.
 *
 * The whole daemon is C99 + POSIX, single-threaded, and allocates nothing after
 * startup. Every buffer below is sized at compile time; 32 MiB of RAM is the
 * budget and the renderer's steady-state footprint is a few hundred KiB.
 */
#ifndef MOGGIE_DLNA_H
#define MOGGIE_DLNA_H

#include <stddef.h>
#include <stdint.h>

#define DLNA_VERSION             "0.1"
#define DLNA_SERVER              "Linux/2.6.35 UPnP/1.0 Moggie/" DLNA_VERSION

#define DLNA_HTTP_PORT_DEFAULT   49494
#define DLNA_SSDP_PORT_DEFAULT   1900
#define DLNA_CACHE_CONTROL       1800
#define DLNA_MAX_AGE_MIN         1
#define DLNA_MAX_AGE_MAX         86400

#define DLNA_MAX_URI             512
#define DLNA_MAX_METADATA        1024
#define DLNA_MAX_FRIENDLY        64
#define DLNA_MAX_UUID            40
#define DLNA_MAX_IP              46

#define DLNA_MAX_CLIENTS         8
#define DLNA_REQ_MAX             8192
#define DLNA_RESP_HDR_MAX        512
#define DLNA_RESP_BODY_MAX       8192

#define DLNA_GENA_MAX_SUBS       12
#define DLNA_GENA_MAX_OUT        8
#define DLNA_GENA_CALLBACK_MAX   384
#define DLNA_GENA_SID_MAX        48
#define DLNA_GENA_TIMEOUT_DEF    1800
#define DLNA_GENA_TIMEOUT_MAX    1800
#define DLNA_GENA_TIMEOUT_MIN    1
#define DLNA_GENA_OUT_BUF        4096
#define DLNA_GENA_MAX_FAILURES   3
#define DLNA_GENA_TIMEOUT_MS     2000

#define DLNA_SSDP_MAX_REPLIES    12

enum dlna_service {
    DLNA_SVC_AVT = 0,
    DLNA_SVC_RCS = 1,
    DLNA_SVC_CM  = 2,
    DLNA_SVC_COUNT
};

extern const char *const dlna_svc_type[DLNA_SVC_COUNT];
extern const char *const dlna_svc_id[DLNA_SVC_COUNT];
extern const char *const dlna_svc_dir[DLNA_SVC_COUNT];

/* UPnP MIME list advertised in ConnectionManager and the SCPD. Honest list:
 * these are the formats the DLNA endpoint is specified to accept. Decoding is
 * sonosd's job; anything not listed here is refused, not quietly accepted. */
#define DLNA_SINK_PROTOCOL_INFO \
    "http-get:*:audio/mpeg:*," \
    "http-get:*:audio/flac:*," \
    "http-get:*:audio/wav:*," \
    "http-get:*:audio/x-wav:*," \
    "http-get:*:audio/aac:*," \
    "http-get:*:audio/ogg:*"

struct dlna_config {
    char bind_addr[DLNA_MAX_IP];       /* HTTP listen address, default 0.0.0.0 */
    char advertise_ip[DLNA_MAX_IP];    /* LOCATION host, resolved at startup  */
    int  http_port;
    int  ssdp_port;
    char uuid[DLNA_MAX_UUID];          /* without the "uuid:" prefix         */
    char friendly_name[DLNA_MAX_FRIENDLY];
    int  cache_control;                /* max-age in seconds                 */
    int  announce_interval;            /* seconds between alive rounds       */
    int  backend_mock;                 /* 0 = stub, 1 = mock sonosd          */
    int  mock_deny;                    /* mock backend: audio path busy      */
    int  log_level;                    /* 0=error 1=warn 2=info 3=debug      */
    int  boot_id;
};

int  dlna_config_init(struct dlna_config *cfg);
int  dlna_config_parse(struct dlna_config *cfg, int argc, char **argv);
void dlna_config_usage(const char *argv0);

int  dlna_uuid_normalise(const char *in, char *out, size_t outsz);
int  dlna_uuid_from_mac(char *out, size_t outsz);
int  dlna_uuid_random(char *out, size_t outsz);
int  dlna_detect_ip(char *out, size_t outsz);

#endif /* MOGGIE_DLNA_H */
