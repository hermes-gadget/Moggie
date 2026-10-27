/*
 * mqtt.h -- state publishing and command subscription.
 *
 * The daemon only ever needs four topics under sonos/<id>/:
 *   state         retained JSON, one document, rewritten on change
 *   cmd           inbound JSON commands
 *   availability  retained "online"/"offline" (last will on the broker side)
 * and the HA discovery config topic.
 *
 * Two backends: libmosquitto when the build has it, otherwise a mock that
 * keeps the last payload per topic in RAM. The mock is not a stub that throws
 * work away: it is what the tests and --selftest drive, and it records enough
 * to assert on. mqtt_is_mock() tells you which one you got.
 */
#ifndef SONOSD_MQTT_H
#define SONOSD_MQTT_H

#include <stddef.h>

#define MQTT_HOST_MAX     96
#define MQTT_BASE_MAX     96
#define MQTT_TOPIC_MAX    128
#define MQTT_PAYLOAD_MAX  768

struct mqtt_config {
    char host[MQTT_HOST_MAX];
    int  port;
    char client_id[48];
    char base_topic[MQTT_BASE_MAX];
    char username[32];
    char password[48];
    int  keepalive;
};

typedef void (*mqtt_cmd_fn)(const char *topic, const char *payload, void *user);

int  mqtt_init(const struct mqtt_config *cfg, mqtt_cmd_fn cb, void *user);

/* Use the in-RAM backend even in a build that has libmosquitto. Call before
 * mqtt_init(). --selftest relies on this so it never opens a socket. */
void mqtt_force_mock(void);

int  mqtt_start(void);

/* Pump the network. Returns 0, or a negative value if the connection dropped
 * (the caller decides whether to retry). */
int  mqtt_poll(int timeout_ms);
void mqtt_stop(void);
int  mqtt_connected(void);
int  mqtt_is_mock(void);
const char *mqtt_backend_name(void);

int  mqtt_publish_state(const char *json);
int  mqtt_publish_availability(int online);

/* One media_player entity plus its device block. Feature names are the
 * lowercase strings Home Assistant's MQTT media_player accepts. */
int  mqtt_publish_ha_media_player(const char *object_id, const char *name);

/* The rest of the control surface: four numbers, two switches and one
 * diagnostic sensor.
 *
 * Deliberately absent, because the daemon cannot back them yet:
 *   LED, temperature, buffer level, firmware update, reboot.
 * Publishing discovery for something that does not work puts a control in
 * front of a user that silently does nothing — worse than no entity. */
int  mqtt_publish_ha_entities(const char *object_id, const char *name);
int  mqtt_publish_uptime(unsigned long seconds);

/* Mock-backend helpers. All degrade safely when the real backend is in use:
 * inject returns 0, last returns "", count returns 0. */
int  mqtt_mock_inject(const char *payload);
const char *mqtt_mock_last(const char *suffix);
unsigned long mqtt_mock_publish_count(void);

#endif /* SONOSD_MQTT_H */
