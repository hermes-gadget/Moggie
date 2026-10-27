/*
 * dsp.h -- the audio device, behind a backend vtable.
 *
 * Two backends exist, both always compiled:
 *   linux  -- ioctl()/write() on /dev/dsp and /dev/audioctl
 *   mock   -- in-memory simulation, no device node is ever opened
 *
 * A SONOSD_MOCK=1 build defaults to the mock backend; a normal build defaults
 * to linux. dsp_force_mock() overrides either way and is what --selftest uses,
 * so the selftest can never touch /dev.
 *
 * dsp.c owns the single volume_state and is the only place that pushes it to
 * the hardware. Callers use dsp_* and read dsp_volume_state() for reporting.
 */
#ifndef SONOSD_DSP_H
#define SONOSD_DSP_H

#include <stddef.h>

#include "config.h"
#include "volume.h"

enum dsp_status {
    DSP_OK        = 0,
    DSP_ERR_OPEN  = -1,
    DSP_ERR_IOCTL = -2,
    DSP_ERR_RANGE = -3,
    DSP_ERR_PERM  = -4,   /* writer is not the arbiter holder            */
    DSP_ERR_UNSUP = -5,   /* device does not implement this control      */
    DSP_ERR_INVAL = -6,
    DSP_ERR_IO    = -7
};

struct dsp_filters {
    int bass;         /* -10..+10 */
    int treble;       /* -10..+10 */
    int balance;      /* -10..+10 */
    int loudness;     /* 0/1      */
    int night_mode;   /* 0/1      */
};

struct dsp_backend {
    const char *name;
    int  (*open)(const struct dsp_config *cfg);
    void (*close)(void);
    int  (*get_volume)(int *pct);
    int  (*set_volume)(int pct, int ramp);
    int  (*get_rvolume)(int *pct);
    int  (*set_rvolume)(int pct);
    int  (*get_mute)(int *on);
    int  (*set_mute)(int on);
    int  (*get_filters)(struct dsp_filters *f);
    int  (*set_filters)(const struct dsp_filters *f);
    int  (*write_pcm)(const void *buf, size_t len);
    int  (*flush)(void);
};

/* Backends. */
const struct dsp_backend *dsp_linux_backend(void);
const struct dsp_backend *mockdev_backend(void);

/* Daemon-side API. Takes the whole config: the audio layer needs the device
 * paths and the boot volume, which live in different structures. */
int  dsp_init(const struct config *cfg);
void dsp_shutdown(void);
void dsp_force_mock(void);              /* call before dsp_init() */
int  dsp_is_mock(void);
const char *dsp_backend_name(void);

int  dsp_get_volume(int *pct);          /* read back from the device */
int  dsp_set_volume(int pct, int ramp);
int  dsp_adjust_volume(int delta, int ramp);
int  dsp_set_mute(int on);
int  dsp_toggle_mute(void);
int  dsp_set_tone(int bass, int treble, int balance);
int  dsp_set_loudness(int on);
int  dsp_set_night_mode(int on);
int  dsp_set_rvolume(int pct);
int  dsp_get_rvolume(int *pct);

/* Only the arbiter holder may call this; returns DSP_ERR_PERM otherwise. */
int  dsp_write_pcm(const char *source, const void *buf, size_t len);

/* Drop whatever is queued -- used when a source is parked. */
int  dsp_flush(void);

const struct volume_state *dsp_volume_state(void);
unsigned long dsp_volume_generation(void);

#endif /* SONOSD_DSP_H */
