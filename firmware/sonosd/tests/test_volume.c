/*
 * test_volume.c -- the volume/tone state machine, no device involved.
 */
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "tests.h"

#include "volume.h"

#include <string.h>

static void case_init_and_clamp(void)
{
    struct volume_state s;

    t_suite("volume: init and clamping");

    volume_init(&s, 20);
    T_EQ(20, s.volume);
    T_EQ(0, s.muted);
    T_EQ(0, s.bass);
    T_EQ(0, s.treble);
    T_EQ(0, s.balance);
    T_EQ(0, s.loudness);
    T_EQ(0, s.night_mode);
    T_EQ(0, s.generation);

    volume_init(&s, 999);
    T_EQ(100, s.volume);
    volume_init(&s, -7);
    T_EQ(0, s.volume);

    T_EQ(0, volume_clamp(-1));
    T_EQ(0, volume_clamp(-100000));
    T_EQ(50, volume_clamp(50));
    T_EQ(100, volume_clamp(101));
    T_EQ(100, volume_clamp(1000000));
}

static void case_set_and_adjust(void)
{
    struct volume_state s;
    int changed = 0;

    t_suite("volume: set and adjust");

    volume_init(&s, 20);

    T_EQ(35, volume_set(&s, 35, &changed));
    T_EQ(1, changed);
    T_EQ(35, s.volume);

    changed = 1;
    T_EQ(35, volume_set(&s, 35, &changed));
    T_EQ(0, changed);                     /* idempotent: nothing to write */
    T_EQ(1, s.generation);

    T_EQ(40, volume_adjust(&s, 5, &changed));
    T_EQ(1, changed);
    T_EQ(100, volume_adjust(&s, 1000, &changed));   /* clamps at the top */
    T_EQ(0, volume_adjust(&s, -1000, &changed));    /* and at the bottom */
    T_EQ(0, volume_adjust(&s, 0, &changed));        /* no-op */
    T_EQ(0, changed);

    /* Clamping to the current value is not a change. */
    changed = 1;
    volume_set(&s, 0, &changed);
    T_EQ(0, changed);
}

static void case_mute(void)
{
    struct volume_state s;
    int changed = 0;

    t_suite("volume: mute");

    volume_init(&s, 42);

    volume_mute(&s, 1, &changed);
    T_EQ(1, s.muted);
    T_EQ(1, changed);
    T_EQ(42, s.volume);                  /* the level is preserved */

    changed = 1;
    volume_mute(&s, 1, &changed);
    T_EQ(0, changed);                    /* already muted */

    volume_mute(&s, 0, &changed);
    T_EQ(0, s.muted);
    T_EQ(1, changed);
    T_EQ(42, s.volume);

    volume_toggle_mute(&s, &changed);
    T_EQ(1, s.muted);
    volume_toggle_mute(&s, &changed);
    T_EQ(0, s.muted);

    /* Sonos behaviour: touching the level while muted brings sound back. */
    volume_mute(&s, 1, &changed);
    T_EQ(55, volume_set(&s, 55, &changed));
    T_EQ(0, s.muted);
    T_EQ(1, changed);
    T_EQ(55, s.volume);

    /* Requesting 0 while muted stays muted. */
    volume_mute(&s, 1, &changed);
    T_EQ(0, volume_set(&s, 0, &changed));
    T_EQ(1, s.muted);

    /* A nudge in either direction also unmutes. */
    volume_set(&s, 30, NULL);
    volume_mute(&s, 1, NULL);
    T_EQ(28, volume_adjust(&s, -2, &changed));
    T_EQ(0, s.muted);
}

static void case_tone(void)
{
    struct volume_state s;
    int changed = 0;

    t_suite("volume: tone");

    volume_init(&s, 30);

    volume_set_tone(&s, 4, -3, 2, &changed);
    T_EQ(4, s.bass);
    T_EQ(-3, s.treble);
    T_EQ(2, s.balance);
    T_EQ(1, changed);

    changed = 1;
    volume_set_tone(&s, 4, -3, 2, &changed);
    T_EQ(0, changed);

    volume_set_tone(&s, 100, -100, 0, &changed);
    T_EQ(10, s.bass);
    T_EQ(-10, s.treble);
    T_EQ(0, s.balance);

    T_EQ(10, volume_set_tone(&s, 10, 10, 0, &changed));
    T_EQ(10, s.treble);

    /* Tone changes must not touch the level or mute. */
    T_EQ(30, s.volume);
    T_EQ(0, s.muted);
}

static void case_flags(void)
{
    struct volume_state s;
    int changed = 0;

    t_suite("volume: loudness and night mode");

    volume_init(&s, 10);

    volume_set_loudness(&s, 1, &changed);
    T_EQ(1, s.loudness);
    T_EQ(1, changed);

    volume_set_loudness(&s, 7, &changed);
    T_EQ(1, s.loudness);        /* coerced, not stored as 7 */
    T_EQ(0, changed);

    volume_set_night_mode(&s, 1, &changed);
    T_EQ(1, s.night_mode);
    volume_set_night_mode(&s, -4, &changed);
    T_EQ(1, s.night_mode);
    volume_set_night_mode(&s, 0, &changed);
    T_EQ(0, s.night_mode);
}

static void case_generation(void)
{
    struct volume_state s;
    unsigned long g0;

    t_suite("volume: generation counter");

    volume_init(&s, 10);
    g0 = s.generation;

    volume_set(&s, 11, NULL);
    T_ASSERT(s.generation > g0);

    g0 = s.generation;
    volume_set(&s, 11, NULL);           /* same value */
    volume_mute(&s, 0, NULL);           /* already unmuted */
    volume_set_tone(&s, 0, 0, 0, NULL); /* already flat */
    volume_set_loudness(&s, 0, NULL);   /* already off */
    T_EQ(g0, s.generation);             /* no spurious republish */

    volume_mute(&s, 1, NULL);
    T_ASSERT(s.generation > g0);
}

static void case_null_safety(void)
{
    struct volume_state s;
    int changed = 1;

    t_suite("volume: NULL state is a no-op");

    T_EQ(0, volume_set(NULL, 50, &changed));
    T_EQ(0, changed);
    T_EQ(0, volume_adjust(NULL, 5, &changed));
    T_EQ(0, volume_mute(NULL, 1, &changed));
    T_EQ(0, volume_toggle_mute(NULL, &changed));
    T_EQ(0, volume_set_tone(NULL, 1, 1, 1, &changed));
    T_EQ(0, volume_set_loudness(NULL, 1, &changed));
    T_EQ(0, volume_set_night_mode(NULL, 1, &changed));

    /* A NULL changed-pointer is allowed everywhere; so is a NULL out-param on
     * the getters. Nothing here should touch memory it does not own. */
    volume_init(&s, 0);
    volume_set(&s, 17, NULL);
    T_EQ(17, s.volume);
}

void suite_volume(void)
{
    case_init_and_clamp();
    case_set_and_adjust();
    case_mute();
    case_tone();
    case_flags();
    case_generation();
    case_null_safety();
}
