#ifndef MOGGIE_DLNA_SSDP_H
#define MOGGIE_DLNA_SSDP_H

#include "dlna.h"

#include <netinet/in.h>

#define SSDP_TARGETS 6

struct ssdp_reply {
    int      used;
    int64_t  due_ms;
    struct sockaddr_in to;
    int      target;
};

struct ssdp {
    int      fd;
    const struct dlna_config *cfg;
    char     nt[SSDP_TARGETS][96];
    char     usn[SSDP_TARGETS][160];
    int      count;
    int64_t  next_announce_ms;
    struct ssdp_reply replies[DLNA_SSDP_MAX_REPLIES];
};

int  ssdp_init(struct ssdp *s, const struct dlna_config *cfg);
void ssdp_shutdown(struct ssdp *s);

void ssdp_announce(struct ssdp *s, int64_t now);
void ssdp_byebye(struct ssdp *s);
void ssdp_read(struct ssdp *s, int64_t now);
void ssdp_tick(struct ssdp *s, int64_t now);
int  ssdp_next_timeout_ms(const struct ssdp *s, int64_t now);

#endif /* MOGGIE_DLNA_SSDP_H */
