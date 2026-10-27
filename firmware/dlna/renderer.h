/*
 * renderer.h -- UPnP transport/volume state on top of the arbiter seam.
 *
 * The backend is the authority for transport state and position; the renderer
 * owns the URI, metadata and the volume it reports, and marks services dirty so
 * GENA can publish LastChange after a command.
 */
#ifndef MOGGIE_DLNA_RENDERER_H
#define MOGGIE_DLNA_RENDERER_H

#include "dlna.h"
#include "dlna_arbiter.h"

enum renderer_result {
    RENDERER_OK      =  0,
    RENDERER_ERR_ARG = -1,
    RENDERER_ERR_STATE = -2,
    RENDERER_ERR_BUSY  = -3,
    RENDERER_ERR_BACKEND = -4
};

struct dlna_renderer {
    const struct dlna_config  *cfg;
    const struct dlna_arbiter *arb;

    char uri[DLNA_MAX_URI];
    char metadata[DLNA_MAX_METADATA];
    int  has_uri;
    int  owned;
    int  volume;
    int  mute;

    unsigned dirty;   /* bit per enum dlna_service */
};

void renderer_init(struct dlna_renderer *r, const struct dlna_config *cfg,
                   const struct dlna_arbiter *arb);
void renderer_shutdown(struct dlna_renderer *r);

int renderer_set_uri(struct dlna_renderer *r, const char *uri,
                     const char *metadata);
int renderer_play(struct dlna_renderer *r);
int renderer_pause(struct dlna_renderer *r);
int renderer_stop(struct dlna_renderer *r);
int renderer_seek(struct dlna_renderer *r, unsigned position_s);
int renderer_set_volume(struct dlna_renderer *r, int volume);
int renderer_set_mute(struct dlna_renderer *r, int mute);

void renderer_snapshot(const struct dlna_renderer *r,
                       struct dlna_playback *out);
const char *renderer_uri(const struct dlna_renderer *r);
const char *renderer_metadata(const struct dlna_renderer *r);
int  renderer_has_uri(const struct dlna_renderer *r);
unsigned renderer_take_dirty(struct dlna_renderer *r);

/* Re-arm a service so its LastChange is retried on the next tick. */
void renderer_mark_dirty(struct dlna_renderer *r, int svc);

#endif /* MOGGIE_DLNA_RENDERER_H */
