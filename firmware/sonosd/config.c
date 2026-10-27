#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_error[160] = "no error";

static void set_error(const char *path, int line, const char *msg)
{
    if (line > 0)
        snprintf(g_error, sizeof(g_error), "%s:%d: %s", path ? path : "<config>", line, msg);
    else
        snprintf(g_error, sizeof(g_error), "%s: %s", path ? path : "<config>", msg);
}

const char *config_error(void)
{
    return g_error;
}

static void copy_str(char *dst, size_t dstsz, const char *src)
{
    size_t n;

    if (dstsz == 0)
        return;
    n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void config_defaults(struct config *c)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));

    copy_str(c->device_id, sizeof(c->device_id), "moggie");
    copy_str(c->device_name, sizeof(c->device_name), "Moggie");

    copy_str(c->mqtt_host, sizeof(c->mqtt_host), "127.0.0.1");
    c->mqtt_port = 1883;
    c->mqtt_user[0] = '\0';
    c->mqtt_pass[0] = '\0';
    c->mqtt_base[0] = '\0';
    c->mqtt_keepalive = 30;

    /* In mock mode the lock lives next to the other build artefacts; on the
     * device it is /run. main() overrides this for SONOSD_MOCK builds. */
    copy_str(c->arbiter_lock, sizeof(c->arbiter_lock), "/run/sonosd.audio.lock");
    c->initial_volume = 20;
    c->volume_step = 2;

    /* The chooser reads boot_ok from here. An empty value disables the commit,
     * which is what a dev build wants. */
    copy_str(c->state_dir, sizeof(c->state_dir), "/jffs");

    c->log_level = 2;
    c->use_syslog = 0;
    c->led_enabled = 1;

    copy_str(c->dsp.pcm_path, sizeof(c->dsp.pcm_path), "/dev/dsp");
    copy_str(c->dsp.ctl_path, sizeof(c->dsp.ctl_path), "/dev/audioctl");
    c->dsp.ioctl_on_pcm = 0;
    c->dsp.force_mock = 0;
}

void config_apply_derived(struct config *c)
{
    if (!c)
        return;
    if (!c->mqtt_base_explicit)
        snprintf(c->mqtt_base, sizeof(c->mqtt_base), "sonos/%s", c->device_id);
    if (c->device_name[0] == '\0')
        copy_str(c->device_name, sizeof(c->device_name), c->device_id);
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s)
        return -1;
    while (end && *end && isspace((unsigned char)*end))
        end++;
    if (end && *end != '\0')
        return -1;
    if (v < INT_MIN || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_bool(const char *s, int *out)
{
    if (!s)
        return -1;
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
        strcmp(s, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
        strcmp(s, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

int config_set(struct config *c, const char *key, const char *value)
{
    int v;

    if (!c || !key || !value)
        return -1;

    if (strcmp(key, "device_id") == 0) {
        if (value[0] == '\0' || strlen(value) >= sizeof(c->device_id))
            return -2;
        copy_str(c->device_id, sizeof(c->device_id), value);
    } else if (strcmp(key, "device_name") == 0) {
        if (strlen(value) >= sizeof(c->device_name))
            return -2;
        copy_str(c->device_name, sizeof(c->device_name), value);
    } else if (strcmp(key, "mqtt_host") == 0) {
        if (value[0] == '\0' || strlen(value) >= sizeof(c->mqtt_host))
            return -2;
        copy_str(c->mqtt_host, sizeof(c->mqtt_host), value);
    } else if (strcmp(key, "mqtt_port") == 0) {
        if (parse_int(value, &v) != 0 || v < 1 || v > 65535)
            return -2;
        c->mqtt_port = v;
    } else if (strcmp(key, "mqtt_user") == 0) {
        if (strlen(value) >= sizeof(c->mqtt_user))
            return -2;
        copy_str(c->mqtt_user, sizeof(c->mqtt_user), value);
    } else if (strcmp(key, "mqtt_pass") == 0) {
        if (strlen(value) >= sizeof(c->mqtt_pass))
            return -2;
        copy_str(c->mqtt_pass, sizeof(c->mqtt_pass), value);
    } else if (strcmp(key, "mqtt_base") == 0) {
        if (strlen(value) >= sizeof(c->mqtt_base))
            return -2;
        copy_str(c->mqtt_base, sizeof(c->mqtt_base), value);
        c->mqtt_base_explicit = 1;
    } else if (strcmp(key, "mqtt_keepalive") == 0) {
        if (parse_int(value, &v) != 0 || v < 5 || v > 600)
            return -2;
        c->mqtt_keepalive = v;
    } else if (strcmp(key, "arbiter_lock") == 0) {
        if (value[0] == '\0' || strlen(value) >= sizeof(c->arbiter_lock))
            return -2;
        copy_str(c->arbiter_lock, sizeof(c->arbiter_lock), value);
    } else if (strcmp(key, "state_dir") == 0) {
        /* Empty is legal: it disables the boot_ok commit for dev builds. */
        if (strlen(value) >= sizeof(c->state_dir))
            return -2;
        copy_str(c->state_dir, sizeof(c->state_dir), value);
    } else if (strcmp(key, "initial_volume") == 0) {
        if (parse_int(value, &v) != 0 || v < 0 || v > 100)
            return -2;
        c->initial_volume = v;
    } else if (strcmp(key, "volume_step") == 0) {
        if (parse_int(value, &v) != 0 || v < 1 || v > 20)
            return -2;
        c->volume_step = v;
    } else if (strcmp(key, "log_level") == 0) {
        if (parse_int(value, &v) != 0 || v < 0 || v > 3)
            return -2;
        c->log_level = v;
    } else if (strcmp(key, "use_syslog") == 0) {
        if (parse_bool(value, &v) != 0)
            return -2;
        c->use_syslog = v;
    } else if (strcmp(key, "led_enabled") == 0) {
        if (parse_bool(value, &v) != 0)
            return -2;
        c->led_enabled = v;
    } else if (strcmp(key, "dsp_path") == 0) {
        if (value[0] == '\0' || strlen(value) >= sizeof(c->dsp.pcm_path))
            return -2;
        copy_str(c->dsp.pcm_path, sizeof(c->dsp.pcm_path), value);
    } else if (strcmp(key, "audioctl_path") == 0) {
        if (value[0] == '\0' || strlen(value) >= sizeof(c->dsp.ctl_path))
            return -2;
        copy_str(c->dsp.ctl_path, sizeof(c->dsp.ctl_path), value);
    } else if (strcmp(key, "dsp_ioctl_node") == 0) {
        /* The GPL drop's /dev/dsp (TDM) implements only TDM_SETMODE/GETRATE;
         * the DSPDEV_* handlers live in the closed audiodev.ko. Which node
         * answers them is a hardware-session question, so make it a knob. */
        if (strcmp(value, "pcm") == 0)
            c->dsp.ioctl_on_pcm = 1;
        else if (strcmp(value, "ctl") == 0)
            c->dsp.ioctl_on_pcm = 0;
        else
            return -2;
    } else if (strcmp(key, "mock") == 0) {
        if (parse_bool(value, &v) != 0)
            return -2;
        c->dsp.force_mock = v;
    } else {
        return -1;
    }
    return 0;
}

static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return s;
}

/* Strip one layer of matching single or double quotes. */
static char *unquote(char *s)
{
    size_t n = strlen(s);

    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        s++;
    }
    return s;
}

int config_load(struct config *c, const char *path)
{
    FILE *fp;
    char line[256];
    int lineno = 0;

    if (!c || !path)
        return -1;

    fp = fopen(path, "r");
    if (!fp) {
        set_error(path, 0, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p;
        char *eq;
        char *key;
        char *value;
        int rc;

        lineno++;
        p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;

        /* Trailing comment after the value, only if it is separated by space
         * (so values containing '#' survive). */
        eq = strchr(p, '=');
        if (!eq) {
            set_error(path, lineno, "expected key = value");
            fclose(fp);
            return -2;
        }
        *eq = '\0';
        key = trim(p);
        value = trim(eq + 1);

        /* Allow "key: value" too, a common typo, but only if there is no '='.
         * Handled above: a line without '=' is an error, deliberately strict. */
        value = unquote(value);

        rc = config_set(c, key, value);
        if (rc == -1) {
            char msg[96];
            snprintf(msg, sizeof(msg), "unknown key '%s'", key);
            set_error(path, lineno, msg);
            fclose(fp);
            return -2;
        }
        if (rc == -2) {
            char msg[128];
            snprintf(msg, sizeof(msg), "bad value for '%s'", key);
            set_error(path, lineno, msg);
            fclose(fp);
            return -2;
        }
    }

    if (ferror(fp)) {
        set_error(path, lineno, "read error");
        fclose(fp);
        return -2;
    }

    fclose(fp);
    config_apply_derived(c);
    return 0;
}
