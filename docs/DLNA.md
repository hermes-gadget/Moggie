# DLNA / UPnP MediaRenderer

`firmware/dlna/` is Moggie's native UPnP AV MediaRenderer. It is how the
speaker becomes a `media_player` in Home Assistant without a custom component or
a universal-player wrapper.

## Why not MQTT media_player

Home Assistant 2026.10.4's MQTT integration has no `media_player` platform:
`media_player` is absent from `SUPPORTED_COMPONENTS` in
`homeassistant/components/mqtt/const.py`, and a discovery document published to
`homeassistant/media_player/...` is ignored without an error. Moggie still
publishes the document as a contract for other consumers (see
[HA.md](HA.md#the-media_player-problem)), but it cannot create the entity.

The image does ship `integrations/dlna_dmr`, which autodiscovers
`urn:schemas-upnp-org:device:MediaRenderer:1` over SSDP and creates a native
`media_player` entity. A compliant renderer is therefore the shortest path with
no extra Home Assistant configuration.

This branch has **not** been run against a live Home Assistant instance. What is
verified here is the protocol: discovery, description, SCPDs, SOAP actions,
faults and GENA events (`tests/dlna/`, 294 checks).

## What a control point sees

1. `M-SEARCH` to `239.255.255.250:1900` with
   `ST: urn:schemas-upnp-org:device:MediaRenderer:1` (or `ssdp:all`).
2. A unicast `HTTP/1.1 200 OK` after a delay bounded by `MX`, carrying
   `LOCATION: http://<ip>:49494/description.xml`, `ST`, `USN`,
   `CACHE-CONTROL: max-age=1800`, `SERVER`, `BOOTID.UPNP.ORG` and `EXT`.
3. `GET /description.xml`: `MediaRenderer:1`, friendly name, UDN, and the three
   services with `SCPDURL`, `controlURL`, `eventSubURL`.
4. `GET /<service>/scpd.xml`: actions, arguments and state variables.
5. `POST /<service>/control` with `SOAPACTION:
   "urn:schemas-upnp-org:service:<Service>:1#<Action>"`.
6. `SUBSCRIBE /<service>/event` with `CALLBACK`, `NT: upnp:event` and
   `TIMEOUT: Second-N`; then `NOTIFY` with
   `NT: upnp:event`, `NTS: upnp:propchange`, `SID` and `SEQ`.

The renderer uses `DMR-1.50` in `X_DLNADOC` and advertises only the formats it
intends to accept. There are no icons, no presentation URL and no embedded web
UI.

## HTTP

- Default port `49494` (`--http-port`), bound to `--http-addr` (default
  `0.0.0.0`).
- One request per connection, `Connection: close`, `Content-Length` on every
  response, `EXT:` on every response.
- Request bodies up to 8 KiB; larger `Content-Length` is `413` before the body
  is read.
- Chunked request bodies are refused (`400`). Persistent connections and
  pipelining are not implemented.
- The pool is eight connections. A ninth is closed immediately; a client that
  stalls is dropped after five seconds.

## Services

### AVTransport

| action | behaviour |
|---|---|
| `SetAVTransportURI` | stores URI and DIDL metadata; stops current playback; parses the first `duration="H:MM:SS"` from the metadata |
| `Play` | requires a URI and `Speed` `1`; asks the arbiter for audio ownership, then plays |
| `Pause` | only from `PLAYING`; otherwise `701` |
| `Stop` | stops and releases audio ownership; the URI is retained |
| `Seek` | `REL_TIME` only; `H:MM:SS` target; refused while stopped |
| `GetTransportInfo` | `STOPPED` / `PLAYING` / `PAUSED_PLAYBACK`, status `OK`, speed `1` |
| `GetPositionInfo` | track, duration, metadata, URI, relative/absolute time; counters are `2147483647` (not implemented) |
| `GetMediaInfo` | track count, duration, current URI/metadata; next URI empty, medium `NETWORK` |
| `GetTransportSettings` | play mode `NORMAL`, record quality `NOT_IMPLEMENTED` |
| `GetCurrentTransportActions` | the transport buttons valid in the current state |
| `GetDeviceCapabilities` | play media `NETWORK`, record media/quality `NOT_IMPLEMENTED` |

### RenderingControl

`GetVolume`, `SetVolume` (0–100), `GetMute`, `SetMute`, `GetVolumeDB`
(linear −40…0 dB × 256, matching the SCPD range −10240…0). Channel `Master`
only. Volume and mute are pushed through the arbiter to sonosd; the backend
snapshot is what UPnP reports, so a hardware knob or MQTT change can be seen by
a control point.

### ConnectionManager

`GetProtocolInfo` advertises this sink list, and nothing else:

```
http-get:*:audio/mpeg:*
http-get:*:audio/flac:*
http-get:*:audio/wav:*
http-get:*:audio/x-wav:*
http-get:*:audio/aac:*
http-get:*:audio/ogg:*
```

`Source` is empty (a renderer has no source). `GetCurrentConnectionIDs` returns
`0`; `GetCurrentConnectionInfo(0)` reports RcsID 0, AVTransportID 0,
direction `Input`, status `OK`, peer `-1`. Any other connection id is `402`.

## SOAP faults

All faults are HTTP 500 with `s:Fault` / `UPnPError` in
`urn:schemas-upnp-org:control-1-0`:

| code | when |
|---|---|
| `401 Invalid Action` | unknown action, missing/mismatched `SOAPACTION`, service mismatch |
| `402 Invalid Args` | missing/oversized argument, `InstanceID != 0`, channel not `Master`, volume outside 0–100, malformed mute/seek/time |
| `701 Transition not available` | `Play` without URI or with audio busy, `Pause`/`Seek` while stopped, non-`REL_TIME` seek unit, `Speed != 1` |

`176`/`701` etc. beyond this set are not used; the SCPD only lists actions that
are implemented, so a control point should rarely see `401` other than after
sending a non-standard action.

## GENA eventing

- 12 subscriber slots total, 8 concurrent NOTIFY sockets.
- `SUBSCRIBE` requires `CALLBACK` (`<http://host:port/path>`) and
  `NT: upnp:event`; malformed or missing values are `412`. The response carries
  `SID` and `TIMEOUT`.
- A `SUBSCRIBE` with only `SID` renews the same subscription (same SID, new
  timeout); an unknown or expired SID is `412`.
- `TIMEOUT: Second-N` is clamped to 1…1800 seconds. `Second-infinite` maps to
  1800. The default is 1800.
- The initial event has `SEQ: 0`; every later NOTIFY increments `SEQ` for that
  SID.
- AVTransport and RenderingControl send a `LastChange` property; ConnectionManager
  sends `SourceProtocolInfo`, `SinkProtocolInfo` and `CurrentConnectionIDs`.
- NOTIFY is non-blocking: connect/send/read have a 2 second deadline. Three
  failed deliveries drop the subscriber. A slow or dead callback cannot stall
  SSDP or the control server.
- Position is not evented every second. State and volume changes produce
  `LastChange`; a control point polls `GetPositionInfo` for smooth progress.

## The sonosd seam

The renderer never opens `/dev/dsp` and does not link sonosd. It calls a fixed
vtable declared in `firmware/dlna/dlna_arbiter.h`:

```c
struct dlna_arbiter {
    const char *name;
    void       *ctx;
    int  (*request)(void *ctx);
    void (*release)(void *ctx);
    int  (*set_uri)(void *ctx, const char *uri, const char *metadata,
                    unsigned duration_s);
    int  (*play)(void *ctx);
    int  (*pause)(void *ctx);
    int  (*stop)(void *ctx);
    int  (*seek)(void *ctx, unsigned position_s);
    int  (*set_volume)(void *ctx, int volume, int mute);
    int  (*query)(void *ctx, struct dlna_playback *out);
};
```

Two implementations ship:

| backend | purpose |
|---|---|
| `dlna_arbiter_stub()` | production default until sonosd grows a DLNA backend: keeps transport state and a monotonic position, no audio, always grants ownership |
| `dlna_arbiter_mock_sonosd()` | test double: refuses commands before `request()`, can report the audio path busy (`--mock-deny`), advances a clock, owns volume/mute |

`--backend mock-sonosd` selects the second; `--mock-deny` makes `request()`
return `DLNA_ARB_BUSY`, which the renderer maps to `701`. This is exactly what a
real sonosd must do when AirPlay owns the audio path.

### What sonosd must implement

A real backend (proposed `firmware/dlna/backend_sonosd.c`, a thin IPC client)
must provide:

1. **`request`** — equivalent to `arb_request("dlna")`, but non-blocking. Return
   `DLNA_ARB_OK` when DLNA may drive `/dev/dsp`; `DLNA_ARB_BUSY` when another
   source holds it (AirPlay, Spotify, Snapcast, line-in). Do not block the
   renderer's event loop waiting for the arbiter; a later `Play` retries.
2. **`release`** — equivalent to `arb_release("dlna")`; idempotent. Called on
   `Stop`, on `SetAVTransportURI` while playing, and at shutdown.
3. **`set_uri`** — accept an `http-get` URI and DIDL metadata, open or queue the
   stream, and return `DLNA_ARB_ERR` for an unsupported scheme, MIME type or
   unreachable host. `duration_s` is parsed from the metadata and may be 0.
   This must not start playback.
4. **`play` / `pause` / `stop` / `seek`** — drive the decoder and `/dev/dsp`.
   `play` resumes from the paused position or starts at zero after a stop;
   `pause` must freeze the position; `seek` is relative-time seconds. Return
   `DLNA_ARB_DENIED` for a state transition the decoder cannot honour (the
   renderer also checks the obvious cases).
5. **`set_volume` / mute** — apply through sonosd's existing volume path so the
   hardware, MQTT state and UPnP agree. `0…100`, mute 0/1.
6. **`query`** — a non-blocking snapshot of state, position, duration, volume
   and mute. It is called on every `GetTransportInfo` / `GetPositionInfo` /
   `GetMediaInfo` and for every `LastChange`, so it must not block on the DSP,
   network or MQTT.

The IPC shape is deliberately not fixed here. A unix socket at
`/run/sonosd.sock` carrying fixed-size binary requests/replies (or a tiny text
protocol) is enough; the renderer only needs one outstanding call at a time
because it is single-threaded. sonosd should own reconnection: if the socket is
missing, return `DLNA_ARB_ERR`, log once, and let the renderer answer `701`
rather than crash.

On the sonosd side this means:

- add `"dlna"` as an arbiter source name (it already accepts any name under
  `ARB_SOURCE_MAX`);
- expose `arb_request`, `arb_release`, `arb_is_holder`, `arb_holder_copy` over
  the local socket;
- expose a transport snapshot the decoder updates (state, position, duration);
- route DLNA volume changes through the same `volume.c` / `/dev/audioctl` path
  as MQTT, and publish the resulting state on MQTT so both control planes agree.

**No changes to `firmware/sonosd` are made in this branch.** The seam is the
header and this contract; a later branch implements the client and the server
side.

## Memory and size

The daemon allocates nothing after startup. Static footprint is dominated by the
HTTP pool:

- 8 connections × (8 KiB request + 8 KiB response + 512 B header) ≈ 132 KiB
- 8 GENA NOTIFY buffers × 4 KiB ≈ 32 KiB
- 12 subscribers, SSDP reply queue, SCPDs, state: a few KiB

Host measurement while serving the full suite: **~2.1 MiB RSS** (`VmRSS
2176 kB`, peak 2176 kB on x86_64). That is the process as a whole, including
libc; the device figure will differ but the static buffers are the same.

The cross build is a 32-bit MSB PowerPC ELF (`file`: `ELF 32-bit MSB pie
executable, PowerPC`); **296,392 B unstripped, 132,668 B stripped** (`-Os -g`).
The binary is dynamically linked against the pinned glibc toolchain.

## Limitations

- No real audio backend yet; see the seam above.
- IPv4 only.
- One request per HTTP connection, no keep-alive or chunking.
- Metadata 1024 B, URI 512 B; longer is `402`.
- No per-second position events.
- No authentication: UPnP is a LAN protocol. Do not expose the HTTP port to the
  internet.
- SSDP multicast join failure is non-fatal (unicast M-SEARCH still works), but
  Home Assistant discovery then depends on a control point that already knows
  the address.
- Cross-toolchain glibc ABI note says GNU/Linux 3.2.0; the Play:5 runs 2.6.35.
  Runtime compatibility is unverified.
