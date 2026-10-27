/*
 * dsp_linux.c -- the real backend: ioctl() and write() on the device nodes.
 *
 * Nothing here runs in a mock build or in --selftest, and nothing here opens a
 * node until dsp_init() is called with a non-mock configuration.
 *
 * Device-node note (see audioctl_abi.h): DSPDEV_* is the DSP ABI. In the GPL
 * drop the /dev/dsp TDM driver (mpc8xxxtdm_new.c) implements only
 * TDM_SETMODE/GETRATE; the volume and filter handlers are in the closed
 * audiodev.ko, and which node exposes them is not stated anywhere in the
 * source. So the target node is a configuration knob (dsp_ioctl_node), default
 * /dev/audioctl as the brief specifies, switchable to /dev/dsp without a
 * rebuild.
 */
#define _POSIX_C_SOURCE 200809L

#include "dsp.h"
#include "audioctl_abi.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static int g_pcm_fd = -1;
static int g_ctl_fd = -1;
static int g_ioctl_on_pcm;

static int ioctl_fd(void)
{
    return g_ioctl_on_pcm ? g_pcm_fd : g_ctl_fd;
}

static int errno_to_status(int e)
{
    switch (e) {
    case EINVAL:
    case ENOTTY:
    case EOPNOTSUPP:
        return DSP_ERR_UNSUP;
    case EPERM:
    case EACCES:
        return DSP_ERR_PERM;
    case ERANGE:
        return DSP_ERR_RANGE;
    default:
        return DSP_ERR_IOCTL;
    }
}

static int ioctl_get_int(int fd, unsigned long req, int *out)
{
    int v = -1;
    int rc;

    if (fd < 0)
        return DSP_ERR_OPEN;

    rc = ioctl(fd, req, &v);
    if (rc < 0) {
        log_debug("ioctl(0x%lx) get failed: %s", req, strerror(errno));
        return errno_to_status(errno);
    }
    if (out)
        *out = v;
    return DSP_OK;
}

static int ioctl_set_int(int fd, unsigned long req, int value)
{
    int v = value;
    int rc;

    if (fd < 0)
        return DSP_ERR_OPEN;

    rc = ioctl(fd, req, &v);
    if (rc < 0) {
        log_debug("ioctl(0x%lx, %d) set failed: %s", req, value, strerror(errno));
        return errno_to_status(errno);
    }
    return DSP_OK;
}

/* ---- backend ---------------------------------------------------------- */

static int l_open(const struct dsp_config *cfg)
{
    if (!cfg)
        return DSP_ERR_INVAL;

    g_ioctl_on_pcm = cfg->ioctl_on_pcm ? 1 : 0;

    g_pcm_fd = open(cfg->pcm_path, O_WRONLY | O_CLOEXEC);
    if (g_pcm_fd < 0) {
        log_error("open %s: %s", cfg->pcm_path, strerror(errno));
        return DSP_ERR_OPEN;
    }

    g_ctl_fd = open(cfg->ctl_path, O_RDWR | O_CLOEXEC);
    if (g_ctl_fd < 0) {
        log_error("open %s: %s", cfg->ctl_path, strerror(errno));
        close(g_pcm_fd);
        g_pcm_fd = -1;
        return DSP_ERR_OPEN;
    }

    log_info("audio device open: pcm=%s ctl=%s ioctl_on_pcm=%d",
             cfg->pcm_path, cfg->ctl_path, g_ioctl_on_pcm);
    return DSP_OK;
}

static void l_close(void)
{
    if (g_ctl_fd >= 0) {
        close(g_ctl_fd);
        g_ctl_fd = -1;
    }
    if (g_pcm_fd >= 0) {
        close(g_pcm_fd);
        g_pcm_fd = -1;
    }
}

static int l_get_volume(int *pct)
{
    return ioctl_get_int(ioctl_fd(), DSPDEV_GET_VOLUME, pct);
}

static int l_set_volume(int pct, int ramp)
{
    /* SET_VOLUME ramps, SET_VOLUME_NO_RAMP jumps. A ramp is what you want for a
     * user turning the knob; a jump is right when restoring state at boot or
     * when a source takes over. */
    return ioctl_set_int(ioctl_fd(),
                         ramp ? DSPDEV_SET_VOLUME : DSPDEV_SET_VOLUME_NO_RAMP,
                         pct);
}

static int l_get_rvolume(int *pct)
{
    return ioctl_get_int(ioctl_fd(), DSPDEV_GET_RVOLUME, pct);
}

static int l_set_rvolume(int pct)
{
    return ioctl_set_int(ioctl_fd(), DSPDEV_SET_RVOLUME, pct);
}

static int l_set_mute(int on)
{
    int rc;

    if (g_ctl_fd < 0)
        return DSP_ERR_OPEN;

    /* audiodev exposes three mute paths depending on board revision. Try the
     * amp first, then the codec, then the line mute. */
    rc = ioctl_set_int(g_ctl_fd, AUDIOCTL_AMP_MUTE, on);
    if (rc == DSP_OK)
        return DSP_OK;
    rc = ioctl_set_int(g_ctl_fd, AUDIOCTL_CS42416_MUTE, on);
    if (rc == DSP_OK)
        return DSP_OK;
    rc = ioctl_set_int(g_ctl_fd, AUDIOCTL_LINE_MUTE, on);
    if (rc == DSP_OK)
        return DSP_OK;

    log_warn("no working mute ioctl on %s", "audioctl");
    return DSP_ERR_UNSUP;
}

static int l_get_mute(int *on)
{
    int status = 0;
    int rc;

    if (g_ctl_fd < 0)
        return DSP_ERR_OPEN;

    rc = ioctl_get_int(g_ctl_fd, AUDIOCTL_SYSTEM_STATUS, &status);
    if (rc != DSP_OK)
        return rc;

    if (on)
        *on = (status & SYSTAT_AMP_MUTE_STATUS) ? 1 : 0;
    return DSP_OK;
}

static int l_get_filters(struct dsp_filters *f)
{
    (void)f;

    /* dspdev.h has no GET for the filter registers: FILTER_LEVEL and
     * FILTER_ENABLE are write-only. The daemon's volume_state is therefore the
     * only readable copy of tone settings, and it is republished to MQTT from
     * there. Report unsupported rather than inventing values. */
    return DSP_ERR_UNSUP;
}

static int l_set_filters(const struct dsp_filters *f)
{
    struct dsp_filter_level lvl;
    struct dsp_filter_enable en;
    int rc;

    if (!f)
        return DSP_ERR_INVAL;

    lvl.bass = f->bass;
    lvl.treble = f->treble;
    lvl.balance = f->balance;

    en.loudness = f->loudness ? 1 : 0;
    en.night_mode = f->night_mode ? 1 : 0;

    /* ASSUMED payload layout, see audioctl_abi.h. */
    rc = ioctl(ioctl_fd(), DSPDEV_FILTER_LEVEL, &lvl);
    if (rc < 0) {
        log_debug("DSPDEV_FILTER_LEVEL failed: %s", strerror(errno));
        return errno_to_status(errno);
    }

    rc = ioctl(ioctl_fd(), DSPDEV_FILTER_ENABLE, &en);
    if (rc < 0) {
        log_debug("DSPDEV_FILTER_ENABLE failed: %s", strerror(errno));
        return errno_to_status(errno);
    }

    return DSP_OK;
}

static int l_write_pcm(const void *buf, size_t len)
{
    ssize_t n;

    if (g_pcm_fd < 0)
        return DSP_ERR_OPEN;

    n = write(g_pcm_fd, buf, len);
    if (n < 0) {
        log_debug("write(%s): %s", "pcm", strerror(errno));
        return DSP_ERR_IO;
    }
    return (int)n;
}

static int l_flush(void)
{
    int rc;

    if (g_pcm_fd < 0)
        return DSP_ERR_OPEN;

    rc = ioctl(g_pcm_fd, DSPDEV_FLUSH, 0);
    if (rc < 0)
        return errno_to_status(errno);
    return DSP_OK;
}

static const struct dsp_backend g_backend = {
    "linux",
    l_open,
    l_close,
    l_get_volume,
    l_set_volume,
    l_get_rvolume,
    l_set_rvolume,
    l_get_mute,
    l_set_mute,
    l_get_filters,
    l_set_filters,
    l_write_pcm,
    l_flush
};

const struct dsp_backend *dsp_linux_backend(void)
{
    return &g_backend;
}
