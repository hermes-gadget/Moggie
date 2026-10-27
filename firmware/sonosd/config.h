/*
 * config.h -- flat "key = value" config, parsed into fixed buffers.
 *
 * Deliberately dumb: no sections, no includes, no allocation. The appliance has
 * 32 MB of RAM and the config is written by a human or by the installer.
 */
#ifndef SONOSD_CONFIG_H
#define SONOSD_CONFIG_H

#include <stddef.h>

#define CFG_PATH_MAX  96
#define CFG_NAME_MAX  48
#define CFG_ID_MAX    32

struct dsp_config {
    char pcm_path[CFG_PATH_MAX];      /* /dev/dsp     */
    char ctl_path[CFG_PATH_MAX];      /* /dev/audioctl*/
    int  ioctl_on_pcm;                /* 1 = send DSPDEV_* to /dev/dsp, 0 = /dev/audioctl */
    int  force_mock;                  /* 1 = never open a device node      */
};

struct config {
    /* identity */
    char device_id[CFG_ID_MAX];       /* stable slug, used in MQTT topics  */
    char device_name[CFG_NAME_MAX];   /* friendly name, HA display name    */

    /* mqtt */
    char mqtt_host[CFG_PATH_MAX];
    int  mqtt_port;
    char mqtt_user[CFG_ID_MAX];
    char mqtt_pass[CFG_NAME_MAX];
    char mqtt_base[CFG_PATH_MAX];     /* empty => sonos/<device_id>        */
    int  mqtt_base_explicit;          /* set when mqtt_base came from a file */
    int  mqtt_keepalive;

    /* audio */
    char arbiter_lock[CFG_PATH_MAX];  /* lock file path                    */
    int  initial_volume;              /* 0..100, applied at boot           */
    int  volume_step;                 /* for volume_up/volume_down         */

    /* misc */
    int  log_level;                   /* 0..3                              */
    int  use_syslog;
    int  led_enabled;

    struct dsp_config dsp;
};

void config_defaults(struct config *c);

/* Fill in values derived from others (mqtt_base, dsp paths). Idempotent. */
void config_apply_derived(struct config *c);

/* Apply one key/value pair. Returns 0 on success, -1 on unknown key, -2 on a
 * bad or out-of-range value. Works on the same keys the file parser accepts,
 * so tests can drive it directly. */
int config_set(struct config *c, const char *key, const char *value);

/* Load a file over the top of c. Returns 0 on success, -1 if the file cannot be
 * opened, -2 on a parse error (see config_error()). Unknown keys are errors:
 * a typo in a 20-line config should not be silently ignored. */
int config_load(struct config *c, const char *path);

/* Last parse error, "file:line: message". Never NULL. */
const char *config_error(void);

#endif /* SONOSD_CONFIG_H */
