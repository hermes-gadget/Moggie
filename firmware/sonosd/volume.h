/*
 * volume.h -- volume / tone state machine.
 *
 * Pure logic: no ioctls, no I/O, no allocation. dsp.c owns an instance and
 * pushes changes to the device; the unit tests exercise this file alone.
 *
 * Rules (these are asserted by tests/test_volume.c):
 *   - level is 0..100 and is clamped, never rejected
 *   - mute is independent of level
 *   - setting a non-zero level, or nudging the level at all, unmutes; that is
 *     what a Sonos does when you touch the volume rocker while muted
 *   - bass/treble/balance are -10..+10 and are clamped
 *   - loudness/night mode are 0/1 and are coerced
 *   - generation increments only when something actually changed, so callers
 *     can use it to decide whether a state publish is needed
 */
#ifndef SONOSD_VOLUME_H
#define SONOSD_VOLUME_H

#define VOLUME_MIN 0
#define VOLUME_MAX 100
#define TONE_MIN   (-10)
#define TONE_MAX   10

struct volume_state {
    int volume;        /* 0..100            */
    int muted;         /* 0/1               */
    int bass;          /* -10..+10          */
    int treble;        /* -10..+10          */
    int balance;       /* -10..+10          */
    int loudness;      /* 0/1               */
    int night_mode;    /* 0/1               */
    unsigned long generation;
};

void volume_init(struct volume_state *s, int initial);

int  volume_clamp(int pct);

/* All setters return the value actually applied. *changed (may be NULL) is set
 * to 1 when the device needs an update. */
int volume_set(struct volume_state *s, int pct, int *changed);
int volume_adjust(struct volume_state *s, int delta, int *changed);
int volume_mute(struct volume_state *s, int on, int *changed);
int volume_toggle_mute(struct volume_state *s, int *changed);
int volume_set_tone(struct volume_state *s, int bass, int treble, int balance, int *changed);
int volume_set_loudness(struct volume_state *s, int on, int *changed);
int volume_set_night_mode(struct volume_state *s, int on, int *changed);

#endif /* SONOSD_VOLUME_H */
