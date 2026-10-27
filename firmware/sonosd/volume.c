#define _POSIX_C_SOURCE 200809L

#include "volume.h"

#include <string.h>

int volume_clamp(int pct)
{
    if (pct < VOLUME_MIN)
        return VOLUME_MIN;
    if (pct > VOLUME_MAX)
        return VOLUME_MAX;
    return pct;
}

static int tone_clamp(int v)
{
    if (v < TONE_MIN)
        return TONE_MIN;
    if (v > TONE_MAX)
        return TONE_MAX;
    return v;
}

static int flag(int v)
{
    return v ? 1 : 0;
}

void volume_init(struct volume_state *s, int initial)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->volume = volume_clamp(initial);
    s->muted = 0;
    s->bass = 0;
    s->treble = 0;
    s->balance = 0;
    s->loudness = 0;
    s->night_mode = 0;
    s->generation = 0;
}

int volume_set(struct volume_state *s, int pct, int *changed)
{
    int want;
    int was_muted;
    int diff;

    if (!s) {
        if (changed)
            *changed = 0;
        return 0;
    }

    want = volume_clamp(pct);
    was_muted = s->muted;

    /* Sonos behaviour: touching the level while muted brings the sound back,
     * except when the requested level is 0 (that is still "silence"). */
    if (want > 0 && s->muted)
        s->muted = 0;

    diff = (want != s->volume) || (s->muted != was_muted);
    s->volume = want;
    if (diff)
        s->generation++;
    if (changed)
        *changed = diff;
    return s->volume;
}

int volume_adjust(struct volume_state *s, int delta, int *changed)
{
    if (!s) {
        if (changed)
            *changed = 0;
        return 0;
    }
    if (delta == 0) {
        if (changed)
            *changed = 0;
        return s->volume;
    }
    return volume_set(s, s->volume + delta, changed);
}

int volume_mute(struct volume_state *s, int on, int *changed)
{
    int want;

    if (!s) {
        if (changed)
            *changed = 0;
        return 0;
    }

    want = flag(on);
    if (s->muted != want) {
        s->muted = want;
        s->generation++;
        if (changed)
            *changed = 1;
    } else if (changed) {
        *changed = 0;
    }
    return s->muted;
}

int volume_toggle_mute(struct volume_state *s, int *changed)
{
    if (!s) {
        if (changed)
            *changed = 0;
        return 0;
    }
    return volume_mute(s, !s->muted, changed);
}

int volume_set_tone(struct volume_state *s, int bass, int treble, int balance, int *changed)
{
    int b;
    int t;
    int bal;

    if (!s) {
        if (changed)
            *changed = 0;
        return 0;
    }

    b = tone_clamp(bass);
    t = tone_clamp(treble);
    bal = tone_clamp(balance);

    if (b != s->bass || t != s->treble || bal != s->balance) {
        s->bass = b;
        s->treble = t;
        s->balance = bal;
        s->generation++;
        if (changed)
            *changed = 1;
    } else if (changed) {
        *changed = 0;
    }
    return s->bass;
}

int volume_set_loudness(struct volume_state *s, int on, int *changed)
{
    int want;

    if (!s) {
        if (changed)
            *changed = 0;
        return 0;
    }
    want = flag(on);
    if (s->loudness != want) {
        s->loudness = want;
        s->generation++;
        if (changed)
            *changed = 1;
    } else if (changed) {
        *changed = 0;
    }
    return s->loudness;
}

int volume_set_night_mode(struct volume_state *s, int on, int *changed)
{
    int want;

    if (!s) {
        if (changed)
            *changed = 0;
        return 0;
    }
    want = flag(on);
    if (s->night_mode != want) {
        s->night_mode = want;
        s->generation++;
        if (changed)
            *changed = 1;
    } else if (changed) {
        *changed = 0;
    }
    return s->night_mode;
}
