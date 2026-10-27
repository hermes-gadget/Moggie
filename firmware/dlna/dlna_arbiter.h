/*
 * dlna_arbiter.h -- the seam between the UPnP renderer and sonosd.
 *
 * The renderer does not touch /dev/dsp. It asks sonosd for the audio path,
 * hands it a URI, and asks it what is happening. Everything in this header is
 * deliberately small: a fixed vtable, no allocation, no threads, no library.
 *
 * Two implementations ship in firmware/dlna:
 *
 *   dlna_arbiter_stub()        no audio. Keeps the transport state and a
 *                              monotonic position so the renderer is usable and
 *                              testable without sonosd. Always grants audio.
 *
 *   dlna_arbiter_mock_sonosd() a simulated sonosd for the protocol tests. It
 *                              owns a transport, advances a wall-clock position,
 *                              refuses SetURI/Play before request(), and can be
 *                              configured to report the audio path busy (the
 *                              real sonosd does this when AirPlay holds it).
 *
 * What sonosd will need to implement (see docs/DLNA.md):
 *
 *   request()   -> equivalent of arb_request("dlna"), non-blocking, returns
 *                  DLNA_ARB_BUSY when another source holds the path.
 *   release()   -> arb_release("dlna").
 *   set_uri()   -> open/queue the HTTP stream for the URI. May return
 *                  DLNA_ARB_ERR for an unsupported scheme or MIME type.
 *   play/pause/stop/seek -> drive the decoder and /dev/dsp.
 *   set_volume()/--       -> the same volume path sonosd already owns.
 *   query()     -> a snapshot that is safe to call from the renderer's event
 *                  loop; it must not block on the DSP or the network.
 *
 * A future sonosd backend is expected to be a thin IPC client (a unix socket
 * to sonosd) implementing this vtable in firmware/dlna/backend_sonosd.c. The
 * renderer never links sonosd itself, so the two daemons stay independent
 * processes and the 32 MiB device does not get a second copy of the audio path.
 */
#ifndef MOGGIE_DLNA_ARBITER_H
#define MOGGIE_DLNA_ARBITER_H

#include <stddef.h>

#define DLNA_ARB_SOURCE "dlna"

/* Transport states, in the vocabulary GetTransportInfo uses. */
enum dlna_transport_state {
    DLNA_STOPPED       = 0,
    DLNA_PLAYING       = 1,
    DLNA_PAUSED        = 2,   /* reported as PAUSED_PLAYBACK */
    DLNA_TRANSITIONING = 3
};

enum dlna_arb_result {
    DLNA_ARB_OK      =  0,
    DLNA_ARB_BUSY    = -1,    /* another source owns the audio path        */
    DLNA_ARB_DENIED  = -2,    /* command not legal in the current state    */
    DLNA_ARB_ERR     = -3     /* backend failure (I/O, bad URI, no decoder) */
};

/* A snapshot. position_s is the position at the moment of the call. */
struct dlna_playback {
    int      state;         /* enum dlna_transport_state */
    unsigned position_s;
    unsigned duration_s;    /* 0 when unknown */
    int      volume;        /* 0..100 */
    int      mute;          /* 0 or 1 */
};

struct dlna_arbiter {
    const char *name;
    void       *ctx;

    /* Acquire the audio path. Non-blocking: DLNA_ARB_BUSY means "not now". */
    int  (*request)(void *ctx);

    /* Drop the audio path. Safe to call when not held. */
    void (*release)(void *ctx);

    /* Prepare a URI. duration_s may be 0 when the metadata carried none. */
    int  (*set_uri)(void *ctx, const char *uri, const char *metadata,
                    unsigned duration_s);

    int  (*play)(void *ctx);
    int  (*pause)(void *ctx);
    int  (*stop)(void *ctx);
    int  (*seek)(void *ctx, unsigned position_s);

    /* volume 0..100; mute 0/1. */
    int  (*set_volume)(void *ctx, int volume, int mute);

    /* Fill *out. Must not block. Returns DLNA_ARB_OK or a negative value. */
    int  (*query)(void *ctx, struct dlna_playback *out);
};

/* Default, audio-free backend. */
const struct dlna_arbiter *dlna_arbiter_stub(void);

/* Simulated sonosd backend used by tests. */
const struct dlna_arbiter *dlna_arbiter_mock_sonosd(void);

/* Test hook: when deny is non-zero request() reports the audio path busy. */
void dlna_arbiter_mock_configure(int deny);

/* "STOPPED" / "PLAYING" / "PAUSED_PLAYBACK" / "TRANSITIONING". */
const char *dlna_transport_name(int state);

#endif /* MOGGIE_DLNA_ARBITER_H */
