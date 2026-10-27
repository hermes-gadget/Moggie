/*
 * mqtt_mosquitto.c -- real backend, libmosquitto.
 *
 * Only compiled when the build has libmosquitto (SONOSD_HAVE_MOSQUITTO=1);
 * see the Makefile probe. This file is syntax-checked against the stub header
 * in tests/stubs/ by `make check-mosquitto` so it cannot rot when the library is
 * absent from the build host.
 *
 * Threading: mosquitto_loop() is called from the daemon's single event loop, so
 * no callbacks fire on another thread and no locking is needed here.
 */
#define _POSIX_C_SOURCE 200809L

#include "mqtt_backend.h"
#include "log.h"

#include <mosquitto.h>
#include <string.h>

static struct mosquitto *g_mosq;
static mqtt_cmd_fn g_cb;
static void *g_user;
static int g_connected;

static void on_message(struct mosquitto *m, void *obj, const struct mosquitto_message *msg)
{
    (void)m;
    (void)obj;

    if (!g_cb || !msg || !msg->topic)
        return;
    g_cb(msg->topic, msg->payload ? (const char *)msg->payload : "", g_user);
}

static void teardown(void)
{
    if (g_mosq) {
        mosquitto_disconnect(g_mosq);
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
    }
    mosquitto_lib_cleanup();
    g_connected = 0;
}

static int s_start(const struct mqtt_config *cfg, mqtt_cmd_fn cb, void *user,
                   const char *cmd_topic, const char *avail_topic)
{
    int rc;

    if (g_mosq)
        teardown();

    g_cb = cb;
    g_user = user;

    mosquitto_lib_init();

    g_mosq = mosquitto_new(cfg->client_id, 1, NULL);
    if (!g_mosq) {
        log_error("mosquitto_new failed (out of memory?)");
        mosquitto_lib_cleanup();
        return -1;
    }

    if (cfg->username[0] != '\0')
        (void)mosquitto_username_pw_set(g_mosq, cfg->username,
                                        cfg->password[0] ? cfg->password : NULL);

    /* Last will: if the daemon dies, the broker tells HA we are offline. */
    (void)mosquitto_will_set(g_mosq, avail_topic, (int)strlen("offline"), "offline", 1, 1);
    mosquitto_message_callback_set(g_mosq, on_message);

    rc = mosquitto_connect(g_mosq, cfg->host, cfg->port, cfg->keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_error("mqtt connect %s:%d failed: %s", cfg->host, cfg->port,
                  mosquitto_strerror(rc));
        teardown();
        return -1;
    }

    rc = mosquitto_subscribe(g_mosq, NULL, cmd_topic, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_error("mqtt subscribe %s failed: %s", cmd_topic, mosquitto_strerror(rc));
        teardown();
        return -1;
    }

    g_connected = 1;
    log_info("mqtt connected to %s:%d as %s", cfg->host, cfg->port, cfg->client_id);
    return 0;
}

static int s_poll(int timeout_ms)
{
    int rc;

    if (!g_mosq)
        return -1;

    rc = mosquitto_loop(g_mosq, timeout_ms, 1);
    if (rc == MOSQ_ERR_SUCCESS)
        return 0;

    if (rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST) {
        g_connected = 0;
        log_warn("mqtt connection lost (%s), reconnecting", mosquitto_strerror(rc));
        if (mosquitto_reconnect(g_mosq) == MOSQ_ERR_SUCCESS) {
            g_connected = 1;
            return 0;
        }
        return -1;
    }

    log_warn("mqtt loop: %s", mosquitto_strerror(rc));
    return -1;
}

static int s_publish(const char *topic, const char *payload, int retain)
{
    int rc;

    if (!g_mosq || !g_connected)
        return -1;

    rc = mosquitto_publish(g_mosq, NULL, topic, (int)strlen(payload ? payload : ""),
                           payload ? payload : "", 1, retain ? 1 : 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_warn("mqtt publish %s: %s", topic, mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

static void s_stop(void)
{
    teardown();
}

static int s_connected(void)
{
    return g_connected;
}

static const struct mqtt_backend g_backend = {
    "mosquitto",
    s_start,
    s_poll,
    s_publish,
    s_stop,
    s_connected
};

const struct mqtt_backend *mqtt_mosquitto_backend(void)
{
    return &g_backend;
}
