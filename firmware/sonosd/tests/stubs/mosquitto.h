/*
 * tests/stubs/mosquitto.h -- minimal stand-in for libmosquitto's public header.
 *
 * Purpose: syntax-check mqtt_mosquitto.c on a build host that has no
 * libmosquitto (`make check-mosquitto`), so the real backend cannot silently
 * rot. It is NOT a reimplementation and must never be linked into a binary.
 * Signatures mirror libmosquitto 1.6 / 2.x for the calls we make.
 */
#ifndef SONOSD_STUB_MOSQUITTO_H
#define SONOSD_STUB_MOSQUITTO_H

#define MOSQ_ERR_SUCCESS    0
#define MOSQ_ERR_NO_CONN    4
#define MOSQ_ERR_CONN_LOST  7

struct mosquitto;

struct mosquitto_message {
    int   mid;
    char *topic;
    void *payload;
    int   payloadlen;
    int   qos;
    int   retain;
};

typedef void (*mosquitto_message_callback)(struct mosquitto *mosq, void *obj,
                                           const struct mosquitto_message *msg);

int  mosquitto_lib_init(void);
int  mosquitto_lib_cleanup(void);

struct mosquitto *mosquitto_new(const char *id, int clean_session, void *obj);
void mosquitto_destroy(struct mosquitto *mosq);

int  mosquitto_username_pw_set(struct mosquitto *mosq, const char *username,
                               const char *password);
int  mosquitto_will_set(struct mosquitto *mosq, const char *topic, int payloadlen,
                        const void *payload, int qos, int retain);
void mosquitto_message_callback_set(struct mosquitto *mosq,
                                    mosquitto_message_callback on_message);

int  mosquitto_connect(struct mosquitto *mosq, const char *host, int port, int keepalive);
int  mosquitto_reconnect(struct mosquitto *mosq);
int  mosquitto_disconnect(struct mosquitto *mosq);
int  mosquitto_subscribe(struct mosquitto *mosq, int *mid, const char *sub, int qos);
int  mosquitto_publish(struct mosquitto *mosq, int *mid, const char *topic, int payloadlen,
                       const void *payload, int qos, int retain);
int  mosquitto_loop(struct mosquitto *mosq, int timeout, int max_packets);

const char *mosquitto_strerror(int mosq_errno);

#endif /* SONOSD_STUB_MOSQUITTO_H */
