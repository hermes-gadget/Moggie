/*
 * mqtt_backend.h -- internal vtable shared by the mosquitto and mock backends.
 * Nothing outside mqtt*.c includes this.
 */
#ifndef SONOSD_MQTT_BACKEND_H
#define SONOSD_MQTT_BACKEND_H

#include "mqtt.h"

struct mqtt_backend {
    const char *name;
    int  (*start)(const struct mqtt_config *cfg, mqtt_cmd_fn cb, void *user,
                  const char *cmd_topic, const char *avail_topic);
    int  (*poll)(int timeout_ms);
    int  (*publish)(const char *topic, const char *payload, int retain);
    void (*stop)(void);
    int  (*connected)(void);
};

const struct mqtt_backend *mqtt_mock_backend(void);

#if SONOSD_HAVE_MOSQUITTO
const struct mqtt_backend *mqtt_mosquitto_backend(void);
#endif

/* Mock storage, used by mqtt.c to answer mqtt_mock_last()/count(). */
const char *mqtt_mock_backend_last(const char *topic);
unsigned long mqtt_mock_backend_publishes(void);
int mqtt_mock_backend_deliver(const char *topic, const char *payload);

#endif /* SONOSD_MQTT_BACKEND_H */
