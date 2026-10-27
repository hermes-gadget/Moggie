#define _POSIX_C_SOURCE 200809L

#include "renderer.h"
#include "log.h"
#include "util.h"

#include <string.h>

void renderer_init(struct dlna_renderer *r, const struct dlna_config *cfg,
                   const struct dlna_arbiter *arb)
{
    memset(r, 0, sizeof(*r));
    r->cfg = cfg;
    r->arb = arb;
    r->volume = 30;
    r->mute = 0;
    if (r->arb->set_volume)
        (void)r->arb->set_volume(r->arb->ctx, r->volume, r->mute);
}

static void renderer_mark(struct dlna_renderer *r, int svc)
{
    r->dirty |= 1u << (unsigned)svc;
}

/* Re-arm a service whose LastChange could not be built, so the next tick
 * retries it instead of the transition being lost forever. */
void renderer_mark_dirty(struct dlna_renderer *r, int svc)
{
    renderer_mark(r, svc);
}

void renderer_shutdown(struct dlna_renderer *r)
{
    if (!r)
        return;
    if (r->arb && r->arb->stop)
        (void)r->arb->stop(r->arb->ctx);
    if (r->arb && r->owned && r->arb->release)
        r->arb->release(r->arb->ctx);
    r->owned = 0;
}

/* Worst-case length of s in the LastChange document.
 *
 * The value is escaped *twice* on its way out: once when the AVTransport
 * property set is built, and again when that document is embedded in the GENA
 * body. So a single '&' reaches the wire as "&amp;amp;" -- nine bytes, not
 * five. Estimating five is what let a 500-byte URI of '&' pass this check and
 * then fail to build, which is the bug this guard exists to prevent. */
static size_t escaped_len_double(const char *s)
{
    size_t n = 0;

    if (!s)
        return 0;
    for (; *s; s++) {
        switch (*s) {
        case '&': case '<': case '>': case '"': case '\'':
            n += 9;         /* first escape -> 5 bytes, one '&' escapes again */
            break;
        default:
            n += 1;
        }
    }
    return n;
}

int renderer_set_uri(struct dlna_renderer *r, const char *uri,
                     const char *metadata)
{
    unsigned duration;
    int rc;
    size_t n;

    if (!uri || !*uri)
        return RENDERER_ERR_ARG;
    n = strlen(uri);
    if (n >= sizeof(r->uri))
        return RENDERER_ERR_ARG;
    if (metadata && strlen(metadata) >= sizeof(r->metadata))
        return RENDERER_ERR_ARG;

    /* The URI and metadata are published XML-escaped inside a fixed 4 KiB
     * LastChange document. Both are under their own caps and still do not fit
     * together once escaped (a 511-byte URI of '&' alone expands to 2555).
     * Accepting it would answer 200 and then fail to build the event, so the
     * control point would never be told what it just set. Refuse it here.
     * The 512-byte reserve covers the rest of the property set. */
    if (escaped_len_double(uri) + escaped_len_double(metadata) + 512
        >= DLNA_GENA_OUT_BUF)
        return RENDERER_ERR_ARG;

    /* SetAVTransportURI replaces whatever is playing. */
    if (r->arb->query) {
        struct dlna_playback pb;

        if (r->arb->query(r->arb->ctx, &pb) == DLNA_ARB_OK &&
            (pb.state == DLNA_PLAYING || pb.state == DLNA_PAUSED))
            (void)renderer_stop(r);
    }

    duration = parse_didl_duration(metadata);
    rc = r->arb->set_uri(r->arb->ctx, uri, metadata ? metadata : "", duration);
    if (rc != DLNA_ARB_OK) {
        log_warn("renderer: backend refused URI: %d", rc);
        return RENDERER_ERR_BACKEND;
    }

    memcpy(r->uri, uri, n + 1);
    if (metadata) {
        size_t m = strlen(metadata);

        memcpy(r->metadata, metadata, m + 1);
    } else {
        r->metadata[0] = '\0';
    }
    r->has_uri = 1;
    renderer_mark(r, DLNA_SVC_AVT);
    log_info("renderer: URI set (%u bytes%s)", (unsigned)n,
             duration ? ", duration known" : "");
    return RENDERER_OK;
}

int renderer_play(struct dlna_renderer *r)
{
    int acquired = 0;
    int rc;

    if (!r->has_uri)
        return RENDERER_ERR_STATE;

    if (!r->owned) {
        rc = r->arb->request(r->arb->ctx);
        if (rc == DLNA_ARB_BUSY)
            return RENDERER_ERR_BUSY;
        if (rc != DLNA_ARB_OK)
            return RENDERER_ERR_BACKEND;
        r->owned = 1;
        acquired = 1;
    }

    rc = r->arb->play(r->arb->ctx);
    if (rc != DLNA_ARB_OK) {
        if (acquired) {
            r->arb->release(r->arb->ctx);
            r->owned = 0;
        }
        return (rc == DLNA_ARB_DENIED) ? RENDERER_ERR_STATE
                                       : RENDERER_ERR_BACKEND;
    }

    renderer_mark(r, DLNA_SVC_AVT);
    log_info("renderer: playing");
    return RENDERER_OK;
}

int renderer_pause(struct dlna_renderer *r)
{
    int rc = r->arb->pause(r->arb->ctx);

    if (rc == DLNA_ARB_DENIED)
        return RENDERER_ERR_STATE;
    if (rc != DLNA_ARB_OK)
        return RENDERER_ERR_BACKEND;
    renderer_mark(r, DLNA_SVC_AVT);
    log_info("renderer: paused");
    return RENDERER_OK;
}

int renderer_stop(struct dlna_renderer *r)
{
    int rc = r->arb->stop(r->arb->ctx);

    if (rc != DLNA_ARB_OK && rc != DLNA_ARB_DENIED)
        return RENDERER_ERR_BACKEND;
    if (r->owned) {
        r->arb->release(r->arb->ctx);
        r->owned = 0;
    }
    renderer_mark(r, DLNA_SVC_AVT);
    log_info("renderer: stopped");
    return RENDERER_OK;
}

int renderer_seek(struct dlna_renderer *r, unsigned position_s)
{
    int rc = r->arb->seek(r->arb->ctx, position_s);

    if (rc == DLNA_ARB_DENIED)
        return RENDERER_ERR_STATE;
    if (rc != DLNA_ARB_OK)
        return RENDERER_ERR_BACKEND;
    renderer_mark(r, DLNA_SVC_AVT);
    return RENDERER_OK;
}

int renderer_set_volume(struct dlna_renderer *r, int volume)
{
    int rc;

    if (volume < 0 || volume > 100)
        return RENDERER_ERR_ARG;
    rc = r->arb->set_volume(r->arb->ctx, volume, r->mute);
    if (rc != DLNA_ARB_OK)
        return RENDERER_ERR_BACKEND;
    r->volume = volume;
    renderer_mark(r, DLNA_SVC_RCS);
    return RENDERER_OK;
}

int renderer_set_mute(struct dlna_renderer *r, int mute)
{
    int rc;

    if (mute != 0 && mute != 1)
        return RENDERER_ERR_ARG;
    rc = r->arb->set_volume(r->arb->ctx, r->volume, mute);
    if (rc != DLNA_ARB_OK)
        return RENDERER_ERR_BACKEND;
    r->mute = mute;
    renderer_mark(r, DLNA_SVC_RCS);
    return RENDERER_OK;
}

void renderer_snapshot(const struct dlna_renderer *r, struct dlna_playback *out)
{
    memset(out, 0, sizeof(*out));
    out->volume = r->volume;
    out->mute = r->mute;
    if (r->arb->query && r->arb->query(r->arb->ctx, out) == DLNA_ARB_OK) {
        /* sonosd owns the hardware volume; use its snapshot when it has one. */
    } else {
        out->volume = r->volume;
        out->mute = r->mute;
    }
}

const char *renderer_uri(const struct dlna_renderer *r)
{
    return r->uri;
}

const char *renderer_metadata(const struct dlna_renderer *r)
{
    return r->metadata;
}

int renderer_has_uri(const struct dlna_renderer *r)
{
    return r->has_uri;
}

unsigned renderer_take_dirty(struct dlna_renderer *r)
{
    unsigned mask = r->dirty;

    r->dirty = 0;
    return mask;
}
