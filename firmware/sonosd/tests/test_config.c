/*
 * test_config.c -- defaults, key validation, and the file parser.
 *
 * The parser test writes its fixture into build/test, never outside the tree.
 */
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "tests.h"

#include "config.h"

#include <stdio.h>
#include <string.h>

#define CONF_PATH "build/test/sonosd-test.conf"

static int write_file(const char *path, const char *body)
{
    FILE *fp = fopen(path, "w");

    if (!fp)
        return -1;
    fputs(body, fp);
    fclose(fp);
    return 0;
}

static void case_defaults(void)
{
    struct config c;

    t_suite("config: defaults");

    config_defaults(&c);

    T_STREQ("moggie", c.device_id);
    T_STREQ("Moggie", c.device_name);
    T_STREQ("127.0.0.1", c.mqtt_host);
    T_EQ(1883, c.mqtt_port);
    T_EQ(30, c.mqtt_keepalive);
    T_STREQ("/dev/dsp", c.dsp.pcm_path);
    T_STREQ("/dev/audioctl", c.dsp.ctl_path);
    T_EQ(0, c.dsp.force_mock);
    T_EQ(0, c.dsp.ioctl_on_pcm);
    T_EQ(20, c.initial_volume);
    T_EQ(2, c.volume_step);
    T_EQ(0, c.mqtt_base[0]);

    config_apply_derived(&c);
    T_STREQ("sonos/moggie", c.mqtt_base);

    /* Idempotent: a second call must not double the prefix. */
    config_apply_derived(&c);
    T_STREQ("sonos/moggie", c.mqtt_base);

    /* Changing the id re-derives the base... */
    config_set(&c, "device_id", "study");
    config_apply_derived(&c);
    T_STREQ("sonos/study", c.mqtt_base);

    /* ...unless the operator pinned it explicitly. */
    T_EQ(0, config_set(&c, "mqtt_base", "house/kitchen"));
    T_EQ(0, config_set(&c, "device_id", "utility"));
    config_apply_derived(&c);
    T_STREQ("house/kitchen", c.mqtt_base);
}

static void case_set(void)
{
    struct config c;

    t_suite("config: key validation");

    config_defaults(&c);

    T_EQ(0, config_set(&c, "device_id", "kitchen"));
    T_STREQ("kitchen", c.device_id);

    T_EQ(0, config_set(&c, "mqtt_port", "8883"));
    T_EQ(8883, c.mqtt_port);
    T_EQ(-2, config_set(&c, "mqtt_port", "0"));
    T_EQ(-2, config_set(&c, "mqtt_port", "70000"));
    T_EQ(-2, config_set(&c, "mqtt_port", "abc"));

    T_EQ(0, config_set(&c, "initial_volume", "0"));
    T_EQ(0, c.initial_volume);
    T_EQ(0, config_set(&c, "initial_volume", "100"));
    T_EQ(100, c.initial_volume);
    T_EQ(-2, config_set(&c, "initial_volume", "-1"));
    T_EQ(-2, config_set(&c, "initial_volume", "101"));

    T_EQ(0, config_set(&c, "volume_step", "5"));
    T_EQ(5, c.volume_step);
    T_EQ(-2, config_set(&c, "volume_step", "0"));

    T_EQ(0, config_set(&c, "use_syslog", "true"));
    T_EQ(1, c.use_syslog);
    T_EQ(0, config_set(&c, "use_syslog", "off"));
    T_EQ(0, c.use_syslog);
    T_EQ(0, config_set(&c, "led_enabled", "0"));
    T_EQ(0, c.led_enabled);
    T_EQ(-2, config_set(&c, "led_enabled", "maybe"));

    T_EQ(0, config_set(&c, "log_level", "3"));
    T_EQ(3, c.log_level);
    T_EQ(-2, config_set(&c, "log_level", "4"));

    T_EQ(0, config_set(&c, "mock", "1"));
    T_EQ(1, c.dsp.force_mock);

    T_EQ(0, config_set(&c, "dsp_ioctl_node", "pcm"));
    T_EQ(1, c.dsp.ioctl_on_pcm);
    T_EQ(0, config_set(&c, "dsp_ioctl_node", "ctl"));
    T_EQ(0, c.dsp.ioctl_on_pcm);
    T_EQ(-2, config_set(&c, "dsp_ioctl_node", "both"));

    T_EQ(-1, config_set(&c, "not_a_key", "1"));

    /* Too long for the buffer must be rejected, not truncated. */
    T_EQ(-2, config_set(&c, "device_id",
                        "0123456789012345678901234567890123456789012345678901234567890123"));
}

static void case_file(void)
{
    struct config c;

    t_suite("config: file parsing");

    T_EQ(0, write_file(CONF_PATH,
                       "# a comment\n"
                       "\n"
                       "  device_id   =  study  \n"
                       "device_name = \"Study Speaker\"\n"
                       "mqtt_host = 10.0.0.5\n"
                       "mqtt_port = 1884\n"
                       "mock = true\n"
                       "led_enabled = off\n"
                       "dsp_ioctl_node = pcm\n"));

    config_defaults(&c);
    T_EQ(0, config_load(&c, CONF_PATH));
    T_STREQ("study", c.device_id);
    T_STREQ("Study Speaker", c.device_name);
    T_STREQ("10.0.0.5", c.mqtt_host);
    T_EQ(1884, c.mqtt_port);
    T_EQ(1, c.dsp.force_mock);
    T_EQ(0, c.led_enabled);
    T_EQ(1, c.dsp.ioctl_on_pcm);
    T_STREQ("sonos/study", c.mqtt_base);

    /* Unknown key: error with file:line, not a silent ignore. */
    T_EQ(0, write_file(CONF_PATH, "device_id = x\nbogus = 1\n"));
    config_defaults(&c);
    T_EQ(-2, config_load(&c, CONF_PATH));
    T_ASSERT(strstr(config_error(), CONF_PATH) != NULL);
    T_ASSERT(strstr(config_error(), "bogus") != NULL);
    T_ASSERT(strstr(config_error(), ":2:") != NULL);

    /* Bad value. */
    T_EQ(0, write_file(CONF_PATH, "mqtt_port = 99999\n"));
    config_defaults(&c);
    T_EQ(-2, config_load(&c, CONF_PATH));
    T_ASSERT(strstr(config_error(), "mqtt_port") != NULL);

    /* Line with no '='. */
    T_EQ(0, write_file(CONF_PATH, "device_id study\n"));
    config_defaults(&c);
    T_EQ(-2, config_load(&c, CONF_PATH));

    /* Missing file. */
    config_defaults(&c);
    T_EQ(-1, config_load(&c, "build/test/does-not-exist.conf"));
    T_ASSERT(strstr(config_error(), "does-not-exist") != NULL);
}

void suite_config(void)
{
    case_defaults();
    case_set();
    case_file();
}
