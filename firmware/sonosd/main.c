/*
 * main.c -- sonosd.
 *
 * Owns the audio hardware, arbitrates between sources, and speaks just enough
 * MQTT for Home Assistant. Single-threaded event loop: the decoders are separate
 * processes and talk to us through the arbiter's lock file, so there is nothing
 * to gain from threads inside the daemon.
 *
 * Build with SONOSD_MOCK=1 (`make mock`) to run the whole thing on a workstation
 * with no device nodes: the audio backend and the MQTT client are both faked.
 */
#define _POSIX_C_SOURCE 200809L

#include "arbiter.h"
#include "config.h"
#include "dsp.h"
#include "jsonlite.h"
#include "log.h"
#include "mqtt.h"
#include "selftest.h"
#include "volume.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SONOSD_VERSION   "0.1.0"
#define DEFAULT_CONFIG   "/etc/sonosd.conf"
#define DEFAULT_LOCK     "/run/sonosd.audio.lock"
#define STATE_JSON_MAX   512
#define STATE_HEARTBEAT  60   /* seconds between unconditional republishes */
#define MQTT_RETRY_SECONDS 30 /* backoff after a broker connection failure  */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "usage: %s [options]\n"
            "\n"
            "  --config PATH   read settings from PATH (default %s if it exists)\n"
            "  --mock          use the fake audio device and fake MQTT broker\n"
            "  --once          start up, publish once, exit\n"
            "  --selftest      run in-process checks (never touches /dev)\n"
            "  --verbose       debug logging\n"
            "  --version       print version and backend, exit\n"
            "  --help          this text\n",
            prog, DEFAULT_CONFIG);
}

static void install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* deliberately no SA_RESTART: the loop must see EINTR */
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
}

/* In mock mode there is no /run to write to and no root to write it with. */
static void adjust_lock_path(struct config *cfg)
{
    const char *tmp;

    if (!dsp_is_mock())
        return;
    if (strcmp(cfg->arbiter_lock, DEFAULT_LOCK) != 0)
        return;

    tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(cfg->arbiter_lock, sizeof(cfg->arbiter_lock), "%s/sonosd.audio.lock", tmp);
}

static int build_state_json(char *buf, size_t bufsz, const char *source)
{
    const struct volume_state *v;
    int n;

    v = dsp_volume_state();
    if (!v || !buf || bufsz == 0)
        return -1;

    n = snprintf(buf, bufsz,
                 "{\"state\":\"%s\",\"source\":\"%s\",\"volume\":%d,\"mute\":%s,"
                 "\"bass\":%d,\"treble\":%d,\"balance\":%d,\"loudness\":%s,"
                 "\"night_mode\":%s}",
                 (source && source[0]) ? "playing" : "idle",
                 (source && source[0]) ? source : "none",
                 v->volume,
                 v->muted ? "true" : "false",
                 v->bass, v->treble, v->balance,
                 v->loudness ? "true" : "false",
                 v->night_mode ? "true" : "false");

    if (n < 0 || (size_t)n >= bufsz)
        return -1;
    return n;
}

static void publish_state(void)
{
    char json[STATE_JSON_MAX];
    char holder[ARB_SOURCE_MAX];
    int n;

    holder[0] = '\0';
    (void)arb_holder_copy(holder, sizeof(holder));

    n = build_state_json(json, sizeof(json), holder);
    if (n < 0) {
        log_error("state JSON does not fit in %d bytes", (int)sizeof(json));
        return;
    }

    if (mqtt_publish_state(json) != 0)
        log_warn("could not publish state");
    else
        log_debug("state: %s", json);
}

static void handle_command(const char *topic, const char *payload, void *user)
{
    const struct volume_state *v;
    struct config *cfg = user;
    int val = 0;
    char cmd[24];

    (void)topic;

    if (!payload || !*payload)
        return;

    log_info("mqtt cmd: %s", payload);
    v = dsp_volume_state();

    /* Every key in the payload is applied, not just the first. A payload such
     * as {"mute":true,"volume":30} used to apply mute and silently drop the
     * rest, which is worse than refusing it outright. */
    int applied = 0;
    int tone_changed = 0;
    int bass = v ? v->bass : 0;
    int treble = v ? v->treble : 0;
    int balance = v ? v->balance : 0;

    if (json_get_int(payload, "volume", &val) == 1) {
        (void)dsp_set_volume(val, 1);
        applied = 1;
    }
    if (json_get_int(payload, "volume_delta", &val) == 1) {
        (void)dsp_adjust_volume(val, 1);
        applied = 1;
    }
    if (json_get_int(payload, "bass", &val) == 1) {
        bass = val;
        tone_changed = 1;
        applied = 1;
    }
    if (json_get_int(payload, "treble", &val) == 1) {
        treble = val;
        tone_changed = 1;
        applied = 1;
    }
    if (json_get_int(payload, "balance", &val) == 1) {
        balance = val;
        tone_changed = 1;
        applied = 1;
    }
    /* Applied as one update: three separate calls would have the later keys
     * write pre-change values back over the earlier ones. */
    if (tone_changed)
        (void)dsp_set_tone(bass, treble, balance);
    if (json_get_bool(payload, "mute", &val) == 1) {
        (void)dsp_set_mute(val);
        applied = 1;
    }
    if (json_get_bool(payload, "loudness", &val) == 1) {
        (void)dsp_set_loudness(val);
        applied = 1;
    }
    if (json_get_bool(payload, "night_mode", &val) == 1) {
        (void)dsp_set_night_mode(val);
        applied = 1;
    }
    if (json_get_str(payload, "cmd", cmd, sizeof(cmd)) == 1) {
        applied = 1;
        if (strcmp(cmd, "volume_up") == 0)
            (void)dsp_adjust_volume(cfg ? cfg->volume_step : 2, 1);
        else if (strcmp(cmd, "volume_down") == 0)
            (void)dsp_adjust_volume(-(cfg ? cfg->volume_step : 2), 1);
        else if (strcmp(cmd, "mute_toggle") == 0)
            (void)dsp_toggle_mute();
        else if (strcmp(cmd, "status") == 0)
            ;   /* publish_state() below is the answer */
        else
            log_warn("unknown cmd '%s'", cmd);
    }
    if (!applied)
        log_warn("unparseable command payload: %s", payload);

    publish_state();
}

/* Bring the broker connection up and publish the retained baseline: online,
 * discovery, state. Returns 1 on success. */
static time_t g_started_at;

static unsigned long uptime_seconds(void)
{
    if (g_started_at == 0)
        g_started_at = time(NULL);
    return (unsigned long)(time(NULL) - g_started_at);
}

static int mqtt_bring_up(const struct config *cfg)
{
    if (mqtt_start() != 0)
        return 0;

    (void)mqtt_publish_availability(1);
    (void)mqtt_publish_ha_media_player(cfg->device_id, cfg->device_name);
    (void)mqtt_publish_ha_entities(cfg->device_id, cfg->device_name);
    (void)mqtt_publish_uptime(uptime_seconds());
    publish_state();
    return 1;
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

int main(int argc, char **argv)
{
    struct config cfg;
    struct mqtt_config mc;
    const char *config_path = NULL;
    int force_mock = 0;
    int once = 0;
    int verbose = 0;
    int mqtt_ok = 0;
    int rc;
    int i;
    unsigned long last_gen;
    time_t last_publish;
    time_t last_mqtt_try;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) {
            return selftest_run();
        } else if (strcmp(argv[i], "--config") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--config needs a path\n");
                return 2;
            }
            config_path = argv[i];
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--mock") == 0) {
            force_mock = 1;
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("sonosd %s (audio backend: %s)\n", SONOSD_VERSION, dsp_backend_name());
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    config_defaults(&cfg);
    if (config_path) {
        if (config_load(&cfg, config_path) != 0) {
            fprintf(stderr, "config: %s\n", config_error());
            return 2;
        }
    } else if (access(DEFAULT_CONFIG, R_OK) == 0) {
        if (config_load(&cfg, DEFAULT_CONFIG) != 0) {
            fprintf(stderr, "config: %s\n", config_error());
            return 2;
        }
    }

    if (force_mock)
        cfg.dsp.force_mock = 1;

    /* config_load() derives these too, but the defaults path has to as well:
     * without it mqtt_base is empty and mqtt_init() rejects the config. */
    config_apply_derived(&cfg);

    log_init(verbose ? LOG_LEVEL_DEBUG : cfg.log_level, cfg.use_syslog, "sonosd");

    if (cfg.dsp.force_mock)
        dsp_force_mock();

    rc = dsp_init(&cfg);
    if (rc != DSP_OK) {
        log_error("audio init failed (%d); is audiodev.ko loaded?", rc);
        return 3;
    }

    adjust_lock_path(&cfg);

    if (arb_init(cfg.arbiter_lock) != ARB_OK) {
        log_error("arbiter init failed (%s)", cfg.arbiter_lock);
        dsp_shutdown();
        return 4;
    }

    memset(&mc, 0, sizeof(mc));
    snprintf(mc.host, sizeof(mc.host), "%s", cfg.mqtt_host);
    mc.port = cfg.mqtt_port;
    snprintf(mc.client_id, sizeof(mc.client_id), "sonosd-%s", cfg.device_id);
    snprintf(mc.base_topic, sizeof(mc.base_topic), "%s", cfg.mqtt_base);
    snprintf(mc.username, sizeof(mc.username), "%s", cfg.mqtt_user);
    snprintf(mc.password, sizeof(mc.password), "%s", cfg.mqtt_pass);
    mc.keepalive = cfg.mqtt_keepalive;

    if (mqtt_init(&mc, handle_command, &cfg) != 0) {
        log_error("mqtt config rejected");
        arb_shutdown();
        dsp_shutdown();
        return 5;
    }

    if (mqtt_bring_up(&cfg)) {
        mqtt_ok = 1;
    } else {
        /* Local control still works without a broker: buttons and volume do not
         * depend on the network. The loop retries every 30 s. */
        log_warn("running without MQTT, will retry");
    }

    install_signals();

    log_info("sonosd %s ready: id=%s backend=%s mock_mqtt=%d",
             SONOSD_VERSION, cfg.device_id, dsp_backend_name(), mqtt_is_mock());

    if (once)
        goto done;

    last_gen = dsp_volume_generation();
    last_publish = time(NULL);
    last_mqtt_try = time(NULL);

    while (!g_stop) {
        if (mqtt_ok) {
            rc = mqtt_poll(200);
            if (rc < 0) {
                log_warn("mqtt poll failed");
                mqtt_ok = 0;
                last_mqtt_try = time(NULL);
            }
        }

        if (!mqtt_ok) {
            if (time(NULL) - last_mqtt_try >= MQTT_RETRY_SECONDS) {
                last_mqtt_try = time(NULL);
                log_info("mqtt: retrying");
                if (mqtt_bring_up(&cfg)) {
                    mqtt_ok = 1;
                    last_gen = dsp_volume_generation();
                    last_publish = time(NULL);
                }
            }
            sleep_ms(1000);   /* no broker: 1 Hz housekeeping only */
            continue;
        }

        if (dsp_volume_generation() != last_gen) {
            last_gen = dsp_volume_generation();
            publish_state();
            last_publish = time(NULL);
        } else if (time(NULL) - last_publish >= STATE_HEARTBEAT) {
            publish_state();
            (void)mqtt_publish_uptime(uptime_seconds());
            last_publish = time(NULL);
        }
    }

done:
    log_info("stopping");
    if (mqtt_ok) {
        publish_state();
        (void)mqtt_publish_availability(0);
    }
    mqtt_stop();
    arb_shutdown();
    dsp_shutdown();
    return 0;
}
