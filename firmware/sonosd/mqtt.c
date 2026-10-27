/*
 * mqtt.c -- topic/payload layer. Knows the four topics and the HA discovery
 * document; does not know how bytes reach the broker.
 */
#define _POSIX_C_SOURCE 200809L

#include "mqtt.h"
#include "mqtt_backend.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static struct mqtt_config g_cfg;
static mqtt_cmd_fn g_cb;
static void *g_user;
static const struct mqtt_backend *g_be;
static int g_started;
static int g_force_mock;

static char g_state_topic[MQTT_TOPIC_MAX];
static char g_cmd_topic[MQTT_TOPIC_MAX];
static char g_avail_topic[MQTT_TOPIC_MAX];

void mqtt_force_mock(void)
{
    g_force_mock = 1;
}

int mqtt_is_mock(void)
{
    return g_be == mqtt_mock_backend();
}

const char *mqtt_backend_name(void)
{
    return g_be ? g_be->name : "none";
}

static void build_topic(const char *suffix, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/%s", g_cfg.base_topic, suffix);
}

int mqtt_init(const struct mqtt_config *cfg, mqtt_cmd_fn cb, void *user)
{
    if (!cfg)
        return -1;
    if (cfg->host[0] == '\0' || cfg->base_topic[0] == '\0')
        return -1;

    g_cfg = *cfg;
    g_cb = cb;
    g_user = user;

#if SONOSD_HAVE_MOSQUITTO
    g_be = g_force_mock ? mqtt_mock_backend() : mqtt_mosquitto_backend();
#else
    (void)g_force_mock;
    g_be = mqtt_mock_backend();
#endif

    build_topic("state", g_state_topic, sizeof(g_state_topic));
    build_topic("cmd", g_cmd_topic, sizeof(g_cmd_topic));
    build_topic("availability", g_avail_topic, sizeof(g_avail_topic));

    log_info("mqtt backend: %s, base topic %s", g_be->name, g_cfg.base_topic);
    return 0;
}

int mqtt_start(void)
{
    int rc;

    if (!g_be)
        return -1;

    rc = g_be->start(&g_cfg, g_cb, g_user, g_cmd_topic, g_avail_topic);
    if (rc != 0) {
        log_error("mqtt backend '%s' failed to start: %d", g_be->name, rc);
        return rc;
    }

    g_started = 1;
    log_info("mqtt: subscribed to %s", g_cmd_topic);
    return 0;
}

int mqtt_poll(int timeout_ms)
{
    if (!g_be || !g_started)
        return -1;
    return g_be->poll(timeout_ms);
}

void mqtt_stop(void)
{
    if (g_be && g_started)
        g_be->stop();
    g_started = 0;
}

int mqtt_connected(void)
{
    return (g_be && g_started) ? g_be->connected() : 0;
}

int mqtt_publish_state(const char *json)
{
    if (!g_be || !g_started || !json)
        return -1;
    return g_be->publish(g_state_topic, json, 1);
}

int mqtt_publish_availability(int online)
{
    if (!g_be || !g_started)
        return -1;
    return g_be->publish(g_avail_topic, online ? "online" : "offline", 1);
}

/* Names come from a config file a human edits; strip the two characters that
 * would break the JSON rather than pretending to escape a full document. */
static void sanitize(char *dst, size_t dstsz, const char *src)
{
    size_t n = 0;

    while (src && *src && n + 1 < dstsz) {
        if (*src != '"' && *src != '\\')
            dst[n++] = *src;
        src++;
    }
    dst[n] = '\0';
}

int mqtt_publish_ha_media_player(const char *object_id, const char *name)
{
    char topic[MQTT_TOPIC_MAX];
    char json[MQTT_PAYLOAD_MAX];
    char clean_name[48];
    char clean_id[48];
    char clean_uid[64];
    int n;

    if (!g_be || !g_started || !object_id || !*object_id)
        return -1;

    sanitize(clean_name, sizeof(clean_name), name ? name : object_id);
    sanitize(clean_id, sizeof(clean_id), object_id);
    snprintf(clean_uid, sizeof(clean_uid), "moggie_%s", clean_id);

    /* Compact, one line: HA does not care about pretty printing and the
     * appliance has no room for a JSON writer. supported_features is the list
     * of lowercase feature names HA's MQTT media_player understands. */
    n = snprintf(json, sizeof(json),
                 "{\"name\":\"%s\",\"unique_id\":\"%s\","
                 "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
                 "\"availability_topic\":\"%s\",\"payload_available\":\"online\","
                 "\"payload_not_available\":\"offline\","
                 "\"supported_features\":[\"turn_on\",\"turn_off\",\"volume_set\",\"volume_mute\"],"
                 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
                 "\"manufacturer\":\"Sonos\",\"model\":\"Play:5 Gen 1\","
                 "\"sw_version\":\"moggie\"}}",
                 clean_name, clean_uid, g_state_topic, g_cmd_topic, g_avail_topic,
                 clean_uid, clean_name);

    if (n < 0 || (size_t)n >= sizeof(json)) {
        log_error("discovery payload truncated for '%s'", clean_id);
        return -1;
    }

    snprintf(topic, sizeof(topic), "homeassistant/media_player/%s/config", clean_id);
    log_info("mqtt: publishing discovery to %s", topic);
    return g_be->publish(topic, json, 1);
}

/* ---- mock helpers ----------------------------------------------------- */

int mqtt_mock_inject(const char *payload)
{
    if (!mqtt_is_mock() || !g_started)
        return 0;
    return mqtt_mock_backend_deliver(g_cmd_topic, payload);
}

const char *mqtt_mock_last(const char *suffix)
{
    char topic[MQTT_TOPIC_MAX];

    if (!mqtt_is_mock() || !suffix)
        return "";
    build_topic(suffix, topic, sizeof(topic));
    return mqtt_mock_backend_last(topic);
}

unsigned long mqtt_mock_publish_count(void)
{
    if (!mqtt_is_mock())
        return 0;
    return mqtt_mock_backend_publishes();
}
