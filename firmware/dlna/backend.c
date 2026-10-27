/*
 * backend.c -- the two dlna_arbiter implementations that ship today.
 *
 * Neither touches hardware. The stub is the production default until sonosd
 * grows a DLNA backend; the mock is what tests/dlna exercises, including the
 * "audio path is busy" path a real sonosd will produce when AirPlay owns it.
 */
#define _POSIX_C_SOURCE 200809L

#include "dlna_arbiter.h"
#include "log.h"
#include "util.h"

#include <string.h>

struct sim {
    int      state;
    unsigned pos_base;
    int64_t  anchor_ms;
    unsigned duration_s;
    int      volume;
    int      mute;
    int      owned;
    int      deny;
    int      mock;
};

static struct sim g_stub = {
    .state = DLNA_STOPPED,
    .volume = 30,
    .mute = 0,
    .mock = 0
};

static struct sim g_mock = {
    .state = DLNA_STOPPED,
    .volume = 30,
    .mute = 0,
    .mock = 1
};

const char *dlna_transport_name(int state)
{
    switch (state) {
    case DLNA_PLAYING:       return "PLAYING";
    case DLNA_PAUSED:        return "PAUSED_PLAYBACK";
    case DLNA_TRANSITIONING: return "TRANSITIONING";
    case DLNA_STOPPED:
    default:                 return "STOPPED";
    }
}

static unsigned sim_position(struct sim *s)
{
    unsigned pos = s->pos_base;
    int64_t now = now_ms();

    if (s->state == DLNA_PLAYING && now > s->anchor_ms) {
        pos += (unsigned)((now - s->anchor_ms) / 1000);
        if (s->duration_s > 0 && pos >= s->duration_s) {
            pos = s->duration_s;
            s->state = DLNA_STOPPED;
            s->pos_base = pos;
            s->anchor_ms = now;
        }
    }
    if (s->duration_s > 0 && pos > s->duration_s)
        pos = s->duration_s;
    return pos;
}

static int sim_request(void *ctx)
{
    struct sim *s = ctx;

    if (s->mock && s->deny) {
        log_info("backend: audio path busy, DLNA waits");
        return DLNA_ARB_BUSY;
    }
    s->owned = 1;
    return DLNA_ARB_OK;
}

static void sim_release(void *ctx)
{
    struct sim *s = ctx;

    s->owned = 0;
}

static int sim_set_uri(void *ctx, const char *uri, const char *metadata,
                       unsigned duration_s)
{
    struct sim *s = ctx;

    (void)metadata;
    if (!uri || !*uri)
        return DLNA_ARB_ERR;
    s->duration_s = duration_s;
    s->pos_base = 0;
    s->anchor_ms = now_ms();
    s->state = DLNA_STOPPED;
    log_debug("backend: set_uri duration=%us", duration_s);
    return DLNA_ARB_OK;
}

static int sim_play(void *ctx)
{
    struct sim *s = ctx;

    if (s->mock && !s->owned)
        return DLNA_ARB_DENIED;
    if (s->state == DLNA_PLAYING)
        return DLNA_ARB_OK;
    if (s->state == DLNA_STOPPED)
        s->pos_base = 0;
    s->anchor_ms = now_ms();
    s->state = DLNA_PLAYING;
    return DLNA_ARB_OK;
}

static int sim_pause(void *ctx)
{
    struct sim *s = ctx;

    if (s->state != DLNA_PLAYING)
        return DLNA_ARB_DENIED;
    s->pos_base = sim_position(s);
    s->anchor_ms = now_ms();
    s->state = DLNA_PAUSED;
    return DLNA_ARB_OK;
}

static int sim_stop(void *ctx)
{
    struct sim *s = ctx;

    s->state = DLNA_STOPPED;
    s->pos_base = 0;
    s->anchor_ms = now_ms();
    return DLNA_ARB_OK;
}

static int sim_seek(void *ctx, unsigned position_s)
{
    struct sim *s = ctx;

    if (s->state == DLNA_STOPPED)
        return DLNA_ARB_DENIED;
    if (s->duration_s > 0 && position_s > s->duration_s)
        return DLNA_ARB_ERR;
    s->pos_base = position_s;
    s->anchor_ms = now_ms();
    return DLNA_ARB_OK;
}

static int sim_set_volume(void *ctx, int volume, int mute)
{
    struct sim *s = ctx;

    if (volume < 0 || volume > 100 || (mute != 0 && mute != 1))
        return DLNA_ARB_ERR;
    s->volume = volume;
    s->mute = mute;
    return DLNA_ARB_OK;
}

static int sim_query(void *ctx, struct dlna_playback *out)
{
    struct sim *s = ctx;

    if (!out)
        return DLNA_ARB_ERR;
    out->state = s->state;
    out->position_s = sim_position(s);
    out->duration_s = s->duration_s;
    out->volume = s->volume;
    out->mute = s->mute;
    return DLNA_ARB_OK;
}

static const struct dlna_arbiter g_stub_ops = {
    .name = "stub",
    .ctx = &g_stub,
    .request = sim_request,
    .release = sim_release,
    .set_uri = sim_set_uri,
    .play = sim_play,
    .pause = sim_pause,
    .stop = sim_stop,
    .seek = sim_seek,
    .set_volume = sim_set_volume,
    .query = sim_query
};

static const struct dlna_arbiter g_mock_ops = {
    .name = "mock-sonosd",
    .ctx = &g_mock,
    .request = sim_request,
    .release = sim_release,
    .set_uri = sim_set_uri,
    .play = sim_play,
    .pause = sim_pause,
    .stop = sim_stop,
    .seek = sim_seek,
    .set_volume = sim_set_volume,
    .query = sim_query
};

const struct dlna_arbiter *dlna_arbiter_stub(void)
{
    return &g_stub_ops;
}

const struct dlna_arbiter *dlna_arbiter_mock_sonosd(void)
{
    return &g_mock_ops;
}

void dlna_arbiter_mock_configure(int deny)
{
    g_mock.deny = deny ? 1 : 0;
}
