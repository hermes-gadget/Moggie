/*
 * selftest.c -- `sonosd --selftest`.
 *
 * Hard rules: no /dev access, no sockets, no writes outside the caller's temp
 * directory. Both force_mock() hooks are used so a build linked against the real
 * backends still runs this safely.
 */
#define _POSIX_C_SOURCE 200809L

#include "selftest.h"

#include "arbiter.h"
#include "audioctl_abi.h"
#include "config.h"
#include "dsp.h"
#include "jsonlite.h"
#include "log.h"
#include "mockdev.h"
#include "mqtt.h"
#include "volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static int g_checks;
static int g_failures;

static void check(int ok, const char *what)
{
    g_checks++;
    if (ok) {
        printf("[ ok ] %s\n", what);
    } else {
        g_failures++;
        printf("[FAIL] %s\n", what);
    }
}

static void check_eq(const char *what, long want, long got)
{
    g_checks++;
    if (want == got) {
        printf("[ ok ] %s (= %ld)\n", what, got);
    } else {
        g_failures++;
        printf("[FAIL] %s: want %ld, got %ld\n", what, want, got);
    }
}

/* ---- command callback used by the MQTT mock --------------------------- */

static struct {
    int  calls;
    char last[MQTT_PAYLOAD_MAX];
} g_cmd;

static void on_cmd(const char *topic, const char *payload, void *user)
{
    (void)topic;
    (void)user;
    g_cmd.calls++;
    snprintf(g_cmd.last, sizeof(g_cmd.last), "%s", payload ? payload : "");
}

static void check_abi(void)
{
    check_eq("DSPDEV_MAJOR", 250, DSPDEV_MAJOR);
    check_eq("AUDIOCTL_MAJOR", 252, AUDIOCTL_MAJOR);
    check_eq("DSPDEV_GET_VOLUME ioctl number", DSP_SYS_GET_VOLUME, _IOC_NR(DSPDEV_GET_VOLUME));
    check_eq("DSPDEV_SET_VOLUME payload size", (long)sizeof(int), (long)_IOC_SIZE(DSPDEV_SET_VOLUME));
    check_eq("DSPDEV_SET_VOLUME_NO_RAMP payload size", (long)sizeof(int),
             (long)_IOC_SIZE(DSPDEV_SET_VOLUME_NO_RAMP));
    check_eq("DSPDEV_FILTER_LEVEL payload size", 12, (long)_IOC_SIZE(DSPDEV_FILTER_LEVEL));
    check_eq("DSPDEV_FILTER_ENABLE payload size", 8, (long)_IOC_SIZE(DSPDEV_FILTER_ENABLE));
    check_eq("struct dsp_filter_level size", 12, (long)sizeof(struct dsp_filter_level));
    check_eq("struct dsp_filter_enable size", 8, (long)sizeof(struct dsp_filter_enable));
    check(_IOC_SIZE(DSPDEV_FILTER_LEVEL) == sizeof(struct dsp_filter_level),
          "filter level struct matches ioctl size");
}

static void check_volume_sm(void)
{
    struct volume_state s;
    int changed = 0;

    volume_init(&s, 20);
    check_eq("volume_init level", 20, s.volume);
    check_eq("volume_init generation", 0, (long)s.generation);

    check_eq("volume_set(150) clamps", 100, volume_set(&s, 150, &changed));
    check(changed == 1, "volume_set(150) reports a change");
    check_eq("volume_set(-5) clamps", 0, volume_set(&s, -5, &changed));

    volume_set(&s, 40, NULL);
    changed = 1;
    check_eq("volume_set same value is a no-op", 40, volume_set(&s, 40, &changed));
    check(changed == 0, "no change reported for identical level");

    check_eq("volume_adjust(+5)", 45, volume_adjust(&s, 5, NULL));
    check_eq("volume_adjust past max", 100, volume_adjust(&s, 500, NULL));
    check_eq("volume_adjust past min", 0, volume_adjust(&s, -500, NULL));

    volume_mute(&s, 1, &changed);
    check(s.muted == 1 && changed == 1, "mute engages and reports");
    check_eq("mute leaves the level alone", 0, s.volume);
    volume_set(&s, 30, &changed);
    check(s.muted == 0, "raising the level unmutes");
    check_eq("unmuted level", 30, s.volume);

    check_eq("tone bass clamps high", 10, volume_set_tone(&s, 99, 0, 0, &changed));
    check_eq("tone bass clamps low", -10, volume_set_tone(&s, -99, 0, 0, &changed));
    volume_set_tone(&s, 0, 0, 5, &changed);
    check_eq("balance stored", 5, s.balance);
    check_eq("treble stored", 0, s.treble);

    volume_set_loudness(&s, 1, &changed);
    check(s.loudness == 1, "loudness set");
    volume_set_loudness(&s, 7, &changed);
    check(s.loudness == 1, "loudness coerced to 1");
    volume_set_night_mode(&s, 0, &changed);
    check(s.night_mode == 0, "night mode cleared");
    check(s.generation > 0, "generation advanced");
}

static void check_dsp(void)
{
    struct config c;
    struct mockdev_state *ms;
    int v = 0;
    int rc;

    config_defaults(&c);
    /* Point the paths somewhere that does not exist: if the mock were not
     * selected, dsp_init() would fail loudly rather than open a real node. */
    snprintf(c.dsp.pcm_path, sizeof(c.dsp.pcm_path), "/nonexistent/sonosd-selftest");
    snprintf(c.dsp.ctl_path, sizeof(c.dsp.ctl_path), "/nonexistent/sonosd-selftest");
    c.dsp.force_mock = 1;
    c.initial_volume = 20;

    dsp_force_mock();
    rc = dsp_init(&c);
    check_eq("dsp_init(mock)", DSP_OK, rc);
    check(dsp_is_mock(), "backend is the mock, not /dev");
    check(strcmp(dsp_backend_name(), "mock") == 0, "backend name is 'mock'");

    ms = mockdev_state();

    check_eq("device volume after init", 20, ms->volume);
    check_eq("dsp_set_volume(55)", DSP_OK, dsp_set_volume(55, 1));
    check_eq("device saw 55", 55, ms->volume);
    check_eq("ramp flag reached the device", 1, ms->last_ramp);
    check_eq("dsp volume state follows", 55, dsp_volume_state()->volume);

    check_eq("dsp_set_volume(150) clamps", DSP_OK, dsp_set_volume(150, 0));
    check_eq("device clamped to 100", 100, ms->volume);

    check_eq("dsp_set_mute(1)", DSP_OK, dsp_set_mute(1));
    check_eq("device muted", 1, ms->muted);
    check_eq("state muted", 1, dsp_volume_state()->muted);
    check_eq("dsp_toggle_mute()", DSP_OK, dsp_toggle_mute());
    check_eq("device unmuted", 0, ms->muted);

    check_eq("dsp_set_tone", DSP_OK, dsp_set_tone(3, -2, 1));
    check_eq("bass reached device", 3, ms->filters.bass);
    check_eq("treble reached device", -2, ms->filters.treble);
    check_eq("balance reached device", 1, ms->filters.balance);

    check_eq("dsp_set_loudness(1)", DSP_OK, dsp_set_loudness(1));
    check_eq("loudness reached device", 1, ms->filters.loudness);
    check_eq("dsp_set_night_mode(1)", DSP_OK, dsp_set_night_mode(1));
    check_eq("night mode reached device", 1, ms->filters.night_mode);

    check_eq("dsp_set_rvolume(80)", DSP_OK, dsp_set_rvolume(80));
    check_eq("dsp_get_rvolume", DSP_OK, dsp_get_rvolume(&v));
    check_eq("rvolume value", 80, v);
    check_eq("dsp_set_rvolume(101) rejected", DSP_ERR_RANGE, dsp_set_rvolume(101));

    /* Reading the control back must not be mistaken for a user volume change:
     * a read-back while muted must not unmute. */
    dsp_set_volume(60, 0);
    dsp_set_mute(1);
    v = 0;
    check_eq("dsp_get_volume while muted", DSP_OK, dsp_get_volume(&v));
    check_eq("read-back value", 60, v);
    check_eq("read-back did not unmute", 1, dsp_volume_state()->muted);
    dsp_set_mute(0);

    /* Fault injection: the state machine must not swallow a device error. */
    printf("(fault injection: one error line follows)\n");
    mockdev_fail_next(1);
    check(dsp_set_volume(10, 0) != DSP_OK, "device error propagates");

    dsp_shutdown();
}

static void check_arbiter(const char *lock_path)
{
    const char *src = "selftest";
    struct config c;
    unsigned char pcm[16];
    int rc;

    memset(pcm, 0, sizeof(pcm));

    /* dsp_write_pcm() needs an initialised device layer; the mock one keeps
     * this test free of /dev. */
    config_defaults(&c);
    snprintf(c.dsp.pcm_path, sizeof(c.dsp.pcm_path), "/nonexistent/sonosd-selftest");
    snprintf(c.dsp.ctl_path, sizeof(c.dsp.ctl_path), "/nonexistent/sonosd-selftest");
    c.dsp.force_mock = 1;
    dsp_force_mock();
    check_eq("dsp_init(mock) for arbiter test", DSP_OK, dsp_init(&c));

    check_eq("arb_init", ARB_OK, arb_init(lock_path));

    check_eq("write before holding is refused", DSP_ERR_PERM,
             dsp_write_pcm(src, pcm, sizeof(pcm)));

    rc = arb_request(src);
    check_eq("arb_request", ARB_OK, rc);
    check(arb_is_holder(src), "source is the holder");
    check(dsp_write_pcm(src, pcm, sizeof(pcm)) == (int)sizeof(pcm),
          "holder may write PCM");

    check_eq("second source is refused while held", ARB_ERR_BUSY,
             arb_try_request("other"));

    arb_release(src);
    check(!arb_is_holder(src), "release drops ownership");
    check_eq("write after release is refused", DSP_ERR_PERM,
             dsp_write_pcm(src, pcm, sizeof(pcm)));

    /* Double release must be a no-op, not a corruption. It logs a warning on
     * purpose: a source releasing audio it does not hold is a bug worth
     * seeing in the journal. */
    printf("(double release: one warning line follows)\n");
    arb_release(src);
    check_eq("double release is harmless", ARB_OK, arb_try_request("selftest2"));
    arb_release("selftest2");

    arb_shutdown();
    dsp_shutdown();
}

static void check_mqtt(void)
{
    struct mqtt_config mc;
    const char *last;

    memset(&mc, 0, sizeof(mc));
    snprintf(mc.host, sizeof(mc.host), "127.0.0.1");
    mc.port = 1883;
    snprintf(mc.client_id, sizeof(mc.client_id), "sonosd-selftest");
    snprintf(mc.base_topic, sizeof(mc.base_topic), "sonos/selftest");
    mc.keepalive = 30;

    mqtt_force_mock();
    check_eq("mqtt_init", 0, mqtt_init(&mc, on_cmd, NULL));
    check(mqtt_is_mock(), "mqtt backend is the mock");
    check_eq("mqtt_start", 0, mqtt_start());
    check(mqtt_connected(), "mock reports connected");

    check_eq("publish availability", 0, mqtt_publish_availability(1));
    last = mqtt_mock_last("availability");
    check(strcmp(last, "online") == 0, "availability payload retained");

    check_eq("publish state", 0, mqtt_publish_state("{\"volume\":55}"));
    last = mqtt_mock_last("state");
    check(strstr(last, "\"volume\":55") != NULL, "state payload stored under state topic");

    check_eq("publish discovery", 0, mqtt_publish_ha_media_player("selftest", "Test"));
    last = mqtt_mock_last("state");
    check(strstr(last, "\"volume\"") != NULL, "state topic survived discovery publish");

    g_cmd.calls = 0;
    check_eq("inject command", 1, mqtt_mock_inject("{\"volume\":42}"));
    check_eq("callback fired once", 1, g_cmd.calls);
    check(strstr(g_cmd.last, "42") != NULL, "callback got the payload");

    mqtt_stop();
    check(!mqtt_connected(), "stop drops the connection");
}

static void check_json(void)
{
    const char *doc = "{\"volume\":42,\"mute\":true,\"cmd\":\"volume_up\",\"bass\":-3}";
    int v = 0;
    int b = 0;
    char s[32];

    check_eq("json int", 1, json_get_int(doc, "volume", &v));
    check_eq("json int value", 42, v);
    check_eq("json negative int", 1, json_get_int(doc, "bass", &b));
    check_eq("json negative int value", -3, b);
    check_eq("json bool", 1, json_get_bool(doc, "mute", &b));
    check_eq("json bool value", 1, b);
    check_eq("json string", 1, json_get_str(doc, "cmd", s, sizeof(s)));
    check(strcmp(s, "volume_up") == 0, "json string value");
    check_eq("json missing key", 0, json_get_int(doc, "treble", &v));
    check_eq("json malformed int", -1, json_get_int("{\"volume\":\"loud\"}", "volume", &v));
    check_eq("json unterminated string", -1, json_get_str("{\"cmd\":\"x}", "cmd", s, sizeof(s)));
}

static void check_config(void)
{
    struct config c;
    int rc;

    config_defaults(&c);
    check(strcmp(c.dsp.pcm_path, "/dev/dsp") == 0, "default pcm path is /dev/dsp (never opened here)");
    check_eq("default mqtt port", 1883, c.mqtt_port);

    rc = config_set(&c, "device_id", "kitchen");
    check_eq("config_set device_id", 0, rc);
    config_apply_derived(&c);
    check(strcmp(c.mqtt_base, "sonos/kitchen") == 0, "derived mqtt base topic");

    check_eq("config_set unknown key", -1, config_set(&c, "nonsense", "1"));
    check_eq("config_set bad volume", -2, config_set(&c, "initial_volume", "101"));
    check_eq("config_set dsp_ioctl_node", 0, config_set(&c, "dsp_ioctl_node", "pcm"));
    check_eq("dsp ioctl target switched", 1, c.dsp.ioctl_on_pcm);
}

static void check_log(void)
{
    log_init(LOG_LEVEL_ERROR, 0, "sonosd-selftest");
    check_eq("log level", LOG_LEVEL_ERROR, log_get_level());
    log_set_level(LOG_LEVEL_WARN);
    check_eq("log level after set", LOG_LEVEL_WARN, log_get_level());
}

int selftest_run(void)
{
    char lock_path[160];
    const char *tmp = getenv("TMPDIR");

    /* Line-buffer stdout even when piped, so the injected-fault log lines from
     * stderr land in the right place in a combined capture. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("sonosd selftest -- mock devices only, no /dev access\n\n");

    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(lock_path, sizeof(lock_path), "%s/sonosd-selftest.lock", tmp);

    check_log();
    check_abi();
    check_volume_sm();
    check_dsp();
    check_arbiter(lock_path);
    check_mqtt();
    check_json();
    check_config();

    printf("\nselftest: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        printf("selftest: PASS\n");
    else
        printf("selftest: FAIL\n");

    return g_failures == 0 ? 0 : 1;
}
