/*
 * mqtt_mock.c -- in-RAM MQTT backend.
 *
 * Keeps the last payload for each topic so tests can assert on what the daemon
 * would have published, and can deliver an inbound command as if the broker had
 * done it.
 */
#define _POSIX_C_SOURCE 200809L

#include "mqtt_backend.h"

#include <stdio.h>
#include <string.h>

#define MOCK_TOPICS 8

struct mock_slot {
    char          topic[MQTT_TOPIC_MAX];
    char          payload[MQTT_PAYLOAD_MAX];
    int           retain;
    unsigned long count;
};

static struct mock_slot g_slots[MOCK_TOPICS];
static int g_connected;
static mqtt_cmd_fn g_cb;
static void *g_user;
static char g_cmd_topic[MQTT_TOPIC_MAX];
static unsigned long g_publishes;

static struct mock_slot *slot_for(const char *topic, int create)
{
    int i;
    int free_slot = -1;

    if (!topic || !*topic)
        return NULL;

    for (i = 0; i < MOCK_TOPICS; i++) {
        if (g_slots[i].topic[0] && strcmp(g_slots[i].topic, topic) == 0)
            return &g_slots[i];
        if (g_slots[i].topic[0] == '\0' && free_slot < 0)
            free_slot = i;
    }

    if (!create || free_slot < 0)
        return NULL;
    return &g_slots[free_slot];
}

static int m_start(const struct mqtt_config *cfg, mqtt_cmd_fn cb, void *user,
                   const char *cmd_topic, const char *avail_topic)
{
    (void)cfg;
    (void)avail_topic;

    memset(g_slots, 0, sizeof(g_slots));
    g_publishes = 0;
    g_cb = cb;
    g_user = user;
    memset(g_cmd_topic, 0, sizeof(g_cmd_topic));
    if (cmd_topic)
        snprintf(g_cmd_topic, sizeof(g_cmd_topic), "%s", cmd_topic);
    g_connected = 1;
    return 0;
}

static int m_poll(int timeout_ms)
{
    (void)timeout_ms;
    return g_connected ? 0 : -1;
}

static int m_publish(const char *topic, const char *payload, int retain)
{
    struct mock_slot *s;

    if (!g_connected)
        return -1;

    s = slot_for(topic, 1);
    if (!s)
        return -1;

    memset(s->topic, 0, sizeof(s->topic));
    snprintf(s->topic, sizeof(s->topic), "%s", topic);
    memset(s->payload, 0, sizeof(s->payload));
    snprintf(s->payload, sizeof(s->payload), "%s", payload ? payload : "");
    s->retain = retain ? 1 : 0;
    s->count++;
    g_publishes++;
    return 0;
}

static void m_stop(void)
{
    g_connected = 0;
    g_cb = NULL;
    g_user = NULL;
}

static int m_connected(void)
{
    return g_connected;
}

static const struct mqtt_backend g_backend = {
    "mock",
    m_start,
    m_poll,
    m_publish,
    m_stop,
    m_connected
};

const struct mqtt_backend *mqtt_mock_backend(void)
{
    return &g_backend;
}

const char *mqtt_mock_backend_last(const char *topic)
{
    struct mock_slot *s = slot_for(topic, 0);

    return s ? s->payload : "";
}

unsigned long mqtt_mock_backend_publishes(void)
{
    return g_publishes;
}

int mqtt_mock_backend_deliver(const char *topic, const char *payload)
{
    if (!g_cb)
        return 0;
    g_cb(topic, payload ? payload : "", g_user);
    return 1;
}
