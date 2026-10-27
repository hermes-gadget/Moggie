#define _POSIX_C_SOURCE 200809L

#include "mockdev.h"

#include <string.h>

static struct mockdev_state g;

void mockdev_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.volume = 20;
    g.rvolume = 100;
    g.muted = 0;
    g.filters.bass = 0;
    g.filters.treble = 0;
    g.filters.balance = 0;
    g.filters.loudness = 0;
    g.filters.night_mode = 0;
}

struct mockdev_state *mockdev_state(void)
{
    return &g;
}

void mockdev_fail_next(int n)
{
    g.fail_countdown = n;
}

void mockdev_short_write(int bytes)
{
    g.short_write = bytes;
}

void mockdev_fail_mute(int on)
{
    g.fail_mute = on ? 1 : 0;
}

/* Returns 1 if this call should fail. */
static int injected(void)
{
    if (g.fail_countdown > 0) {
        g.fail_countdown--;
        return 1;
    }
    return 0;
}

static int m_open(const struct dsp_config *cfg)
{
    (void)cfg;
    if (g.open)
        return DSP_OK;
    g.open = 1;
    return DSP_OK;
}

static void m_close(void)
{
    g.open = 0;
}

static int m_get_volume(int *pct)
{
    if (!pct)
        return DSP_ERR_INVAL;
    if (injected())
        return DSP_ERR_IOCTL;
    *pct = g.volume;
    return DSP_OK;
}

static int m_set_volume(int pct, int ramp)
{
    if (pct < VOLUME_MIN || pct > VOLUME_MAX)
        return DSP_ERR_RANGE;
    if (injected())
        return DSP_ERR_IOCTL;
    g.volume = pct;
    g.last_ramp = ramp ? 1 : 0;
    return DSP_OK;
}

static int m_get_rvolume(int *pct)
{
    if (!pct)
        return DSP_ERR_INVAL;
    *pct = g.rvolume;
    return DSP_OK;
}

static int m_set_rvolume(int pct)
{
    if (pct < VOLUME_MIN || pct > VOLUME_MAX)
        return DSP_ERR_RANGE;
    if (injected())
        return DSP_ERR_IOCTL;
    g.rvolume = pct;
    return DSP_OK;
}

static int m_get_mute(int *on)
{
    if (!on)
        return DSP_ERR_INVAL;
    if (g.fail_mute)
        return DSP_ERR_UNSUP;
    *on = g.muted;
    return DSP_OK;
}

static int m_set_mute(int on)
{
    if (g.fail_mute)
        return DSP_ERR_UNSUP;
    if (injected())
        return DSP_ERR_IOCTL;
    g.muted = on ? 1 : 0;
    return DSP_OK;
}

static int m_get_filters(struct dsp_filters *f)
{
    if (!f)
        return DSP_ERR_INVAL;
    *f = g.filters;
    return DSP_OK;
}

static int m_set_filters(const struct dsp_filters *f)
{
    if (!f)
        return DSP_ERR_INVAL;
    if (injected())
        return DSP_ERR_IOCTL;
    g.filters = *f;
    return DSP_OK;
}

static int m_write_pcm(const void *buf, size_t len)
{
    if (!buf || len == 0)
        return DSP_ERR_INVAL;
    if (injected())
        return DSP_ERR_IO;
    if (g.short_write > 0) {
        int n = g.short_write;
        g.short_write = 0;
        g.writes++;
        g.bytes += (unsigned long)n;
        return n;
    }
    g.writes++;
    g.bytes += (unsigned long)len;
    return (int)len;
}

static int m_flush(void)
{
    g.flushes++;
    return DSP_OK;
}

static const struct dsp_backend g_backend = {
    "mock",
    m_open,
    m_close,
    m_get_volume,
    m_set_volume,
    m_get_rvolume,
    m_set_rvolume,
    m_get_mute,
    m_set_mute,
    m_get_filters,
    m_set_filters,
    m_write_pcm,
    m_flush
};

const struct dsp_backend *mockdev_backend(void)
{
    return &g_backend;
}
