/*
 * dsp.c -- the daemon-facing audio API.
 *
 * Owns one volume_state and the chosen backend. Everything that reaches the
 * hardware goes through here, so there is exactly one place that knows the
 * order of operations (state first, then device, then report the failure).
 */
#define _POSIX_C_SOURCE 200809L

#include "dsp.h"
#include "arbiter.h"
#include "log.h"
#include "mockdev.h"

#include <string.h>

#ifndef SONOSD_MOCK
#define SONOSD_MOCK 0
#endif

static const struct dsp_backend *g_be;
static struct volume_state g_vol;
static int g_inited;
static int g_force_mock;
static int g_mute_via_volume;   /* device had no mute control */
static int g_saved_volume;      /* level to restore when unmuting that way */

void dsp_force_mock(void)
{
    g_force_mock = 1;
}

int dsp_is_mock(void)
{
    return g_be ? (g_be == mockdev_backend()) : (g_force_mock || SONOSD_MOCK);
}

const char *dsp_backend_name(void)
{
    return g_be ? g_be->name : (g_force_mock || SONOSD_MOCK ? "mock" : "linux");
}

static int choose_mock(void)
{
    if (g_force_mock)
        return 1;
    if (SONOSD_MOCK)
        return 1;
    return 0;
}

int dsp_init(const struct config *cfg)
{
    const struct dsp_config *dc;
    int rc;
    int v = 0;

    if (!cfg)
        return DSP_ERR_INVAL;
    if (g_inited)
        return DSP_OK;

    dc = &cfg->dsp;

    if (choose_mock()) {
        mockdev_reset();
        g_be = mockdev_backend();
        log_info("audio backend: mock (no device node will be opened)");
    } else {
        g_be = dsp_linux_backend();
    }

    rc = g_be->open(dc);
    if (rc != DSP_OK) {
        g_be = NULL;
        return rc;
    }

    g_mute_via_volume = 0;
    g_saved_volume = 0;

    if (g_be->get_volume(&v) == DSP_OK) {
        volume_init(&g_vol, v);
    } else {
        /* A DSP that cannot report its level is not unusual after a cold boot;
         * start from the configured default rather than guessing. */
        log_warn("device would not report volume, using initial_volume=%d",
                 cfg->initial_volume);
        volume_init(&g_vol, cfg->initial_volume);
        (void)g_be->set_volume(g_vol.volume, 0);
    }

    {
        int muted = 0;
        if (g_be->get_mute(&muted) == DSP_OK && muted)
            g_vol.muted = 1;
    }

    g_saved_volume = g_vol.volume;
    g_inited = 1;

    log_info("audio ready: backend=%s volume=%d muted=%d",
             g_be->name, g_vol.volume, g_vol.muted);
    return DSP_OK;
}

void dsp_shutdown(void)
{
    if (!g_inited)
        return;
    if (g_be && g_be->close)
        g_be->close();
    g_inited = 0;
    g_mute_via_volume = 0;
    g_saved_volume = 0;
    memset(&g_vol, 0, sizeof(g_vol));
    g_be = NULL;
}

const struct volume_state *dsp_volume_state(void)
{
    return &g_vol;
}

unsigned long dsp_volume_generation(void)
{
    return g_vol.generation;
}

int dsp_get_volume(int *pct)
{
    int v = 0;
    int rc;

    if (!pct)
        return DSP_ERR_INVAL;
    if (!g_inited)
        return DSP_ERR_OPEN;

    rc = g_be->get_volume(&v);
    if (rc != DSP_OK) {
        *pct = g_vol.volume;
        return rc;
    }

    /* Resync the state machine, but do NOT go through volume_set(): that would
     * treat "level went up" as a user action and clear the mute flag. Reading
     * the control must never unmute. */
    v = volume_clamp(v);
    if (v != g_vol.volume) {
        log_debug("volume read back as %d (state had %d), resynced", v, g_vol.volume);
        g_vol.volume = v;
        g_vol.generation++;
    }
    *pct = g_vol.volume;
    return DSP_OK;
}

int dsp_set_volume(int pct, int ramp)
{
    int changed = 0;
    int applied;
    int rc;

    if (!g_inited)
        return DSP_ERR_OPEN;

    applied = volume_set(&g_vol, pct, &changed);
    if (!changed)
        return DSP_OK;

    rc = g_be->set_volume(applied, ramp);
    if (rc != DSP_OK) {
        log_error("set_volume(%d) failed: %d", applied, rc);
        return rc;
    }

    if (!g_vol.muted)
        g_saved_volume = applied;

    log_debug("volume=%d ramp=%d", applied, ramp ? 1 : 0);
    return DSP_OK;
}

int dsp_adjust_volume(int delta, int ramp)
{
    int changed = 0;
    int want;
    int applied;
    int rc;

    if (!g_inited)
        return DSP_ERR_OPEN;
    if (delta == 0)
        return DSP_OK;

    want = g_vol.volume + delta;
    applied = volume_set(&g_vol, want, &changed);
    if (!changed)
        return DSP_OK;

    rc = g_be->set_volume(applied, ramp);
    if (rc != DSP_OK) {
        log_error("set_volume(%d) failed: %d", applied, rc);
        return rc;
    }
    if (!g_vol.muted)
        g_saved_volume = applied;
    return DSP_OK;
}

/* Mute by pulling the level to zero, for devices with no mute control. */
static int apply_soft_mute(int on)
{
    int rc;

    if (on)
        rc = g_be->set_volume(0, 0);
    else
        rc = g_be->set_volume(g_saved_volume, 0);

    if (rc != DSP_OK)
        log_error("soft mute(%d) failed: %d", on, rc);
    return rc;
}

int dsp_set_mute(int on)
{
    int want = on ? 1 : 0;
    int changed = 0;
    int rc;

    if (!g_inited)
        return DSP_ERR_OPEN;

    if (g_mute_via_volume) {
        rc = apply_soft_mute(want);
        if (rc != DSP_OK)
            return rc;
        (void)volume_mute(&g_vol, want, NULL);
        return DSP_OK;
    }

    (void)volume_mute(&g_vol, want, &changed);
    if (!changed)
        return DSP_OK;

    rc = g_be->set_mute(want);
    if (rc == DSP_ERR_UNSUP || rc == DSP_ERR_IOCTL) {
        /* Fall back once, then stay in that mode: retrying a missing ioctl on
         * every volume key press is noise in the log and latency on the path. */
        log_warn("no usable mute control, falling back to volume 0");
        g_mute_via_volume = 1;
        if (want)
            g_saved_volume = g_vol.volume;
        rc = apply_soft_mute(want);
        if (rc != DSP_OK)
            return rc;
        return DSP_OK;
    }

    if (rc != DSP_OK) {
        log_error("set_mute(%d) failed: %d", want, rc);
        return rc;
    }
    return DSP_OK;
}

int dsp_toggle_mute(void)
{
    return dsp_set_mute(g_vol.muted ? 0 : 1);
}

static int push_filters(void)
{
    struct dsp_filters f;
    int rc;

    f.bass = g_vol.bass;
    f.treble = g_vol.treble;
    f.balance = g_vol.balance;
    f.loudness = g_vol.loudness;
    f.night_mode = g_vol.night_mode;

    rc = g_be->set_filters(&f);
    if (rc != DSP_OK)
        log_error("set_filters failed: %d", rc);
    return rc;
}

int dsp_set_tone(int bass, int treble, int balance)
{
    int changed = 0;

    if (!g_inited)
        return DSP_ERR_OPEN;

    (void)volume_set_tone(&g_vol, bass, treble, balance, &changed);
    if (!changed)
        return DSP_OK;
    return push_filters();
}

int dsp_set_loudness(int on)
{
    int changed = 0;

    if (!g_inited)
        return DSP_ERR_OPEN;

    (void)volume_set_loudness(&g_vol, on, &changed);
    if (!changed)
        return DSP_OK;
    return push_filters();
}

int dsp_set_night_mode(int on)
{
    int changed = 0;

    if (!g_inited)
        return DSP_ERR_OPEN;

    (void)volume_set_night_mode(&g_vol, on, &changed);
    if (!changed)
        return DSP_OK;
    return push_filters();
}

int dsp_set_rvolume(int pct)
{
    if (!g_inited)
        return DSP_ERR_OPEN;
    if (pct < VOLUME_MIN || pct > VOLUME_MAX)
        return DSP_ERR_RANGE;
    return g_be->set_rvolume(pct);
}

int dsp_get_rvolume(int *pct)
{
    if (!g_inited)
        return DSP_ERR_OPEN;
    if (!pct)
        return DSP_ERR_INVAL;
    return g_be->get_rvolume(pct);
}

int dsp_write_pcm(const char *source, const void *buf, size_t len)
{
    if (!g_inited)
        return DSP_ERR_OPEN;
    if (!source || !buf || len == 0)
        return DSP_ERR_INVAL;

    /* The whole point of the arbiter: a parked source must not be able to push
     * samples into the DAC. */
    if (!arb_is_holder(source)) {
        log_debug("write from '%s' refused: not the arbiter holder", source);
        return DSP_ERR_PERM;
    }

    return g_be->write_pcm(buf, len);
}

int dsp_flush(void)
{
    if (!g_inited)
        return DSP_ERR_OPEN;
    if (!g_be->flush)
        return DSP_ERR_UNSUP;
    return g_be->flush();
}
