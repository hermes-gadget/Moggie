/*
 * mockdev.h -- in-memory stand-in for /dev/dsp + /dev/audioctl.
 *
 * Used by SONOSD_MOCK=1 builds and by --selftest. It models what the hardware
 * actually does (clamping, mute flag, relative volume, filters) and adds fault
 * injection so error paths can be tested without a device.
 */
#ifndef SONOSD_MOCKDEV_H
#define SONOSD_MOCKDEV_H

#include <stddef.h>

#include "dsp.h"

struct mockdev_state {
    int  open;
    int  volume;        /* 0..100 */
    int  rvolume;       /* 0..100 */
    int  muted;
    int  last_ramp;     /* ramp flag of the last set_volume */
    struct dsp_filters filters;
    unsigned long writes;       /* successful write_pcm calls  */
    unsigned long bytes;        /* total bytes written         */
    unsigned long flushes;
    int  fail_countdown;        /* next N calls fail with DSP_ERR_IO */
    int  short_write;           /* next write returns this many bytes, 0 = off */
    int  fail_mute;             /* simulate "device has no mute control" */
};

const struct dsp_backend *mockdev_backend(void);
struct mockdev_state *mockdev_state(void);
void mockdev_reset(void);
void mockdev_fail_next(int n);
void mockdev_short_write(int bytes);
void mockdev_fail_mute(int on);

#endif /* SONOSD_MOCKDEV_H */
