# dlna

A minimal UPnP AV MediaRenderer for Moggie: SSDP discovery, an HTTP device
description and three SCPDs, SOAP control for AVTransport / RenderingControl /
ConnectionManager, and GENA eventing. C99 and POSIX sockets only — no libupnp,
no gupnp, no dynamic allocation after startup.

The target is the Play:5 Gen 1: 32 MiB RAM, 300 MHz MPC8315, Linux 2.6.35.

## Why this exists

Home Assistant 2026.10.4 has **no MQTT `media_player` platform**, but it does
ship `integrations/dlna_dmr` with SSDP autodiscovery for
`urn:schemas-upnp-org:device:MediaRenderer:1`. A standards-compliant renderer is
therefore the native way for Moggie to appear as `media_player.*` without a
universal-player shim. See [docs/DLNA.md](../../docs/DLNA.md).

## Build

```sh
make                    # host build       -> build/host/dlna-renderer
make test               # host build + protocol tests
make clean
```

Cross build (the same convention as `firmware/sonosd`: an ambient
`CROSS_COMPILE` is ignored, only the command-line value is honoured):

```sh
export PATH=/home/ben/Moggie/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin:$PATH
make CROSS_COMPILE=powerpc-buildroot-linux-gnu- OPT='-Os -g'
# -> build/cross/dlna-renderer
```

The build is warning-clean with `-Wall -Wextra -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wpointer-arith`.

## Run

```sh
./build/host/dlna-renderer \
  --http-port 49494 \
  --ip 192.168.1.50 \
  --uuid 12345678-1234-4234-8234-123456789abc \
  --friendly-name "Moggie Play:5"
```

`--ip` is the address that goes into `LOCATION`; without it the first
non-loopback IPv4 address is used. The UUID defaults to one derived from the
first non-loopback MAC address, which is stable on a single-NIC speaker but
should be set explicitly for a predictable Home Assistant entity.

| option | default | meaning |
|---|---|---|
| `--http-port N` | `49494` | HTTP server port |
| `--http-addr ADDR` | `0.0.0.0` | HTTP bind address |
| `--ssdp-port N` | `1900` | SSDP port (tests use a private port) |
| `--ip ADDR` | first non-loopback IPv4 | address advertised in LOCATION |
| `--uuid UUID` | MAC-derived | device UUID, with or without `uuid:` |
| `--friendly-name NAME` | `Moggie Play:5` | UPnP friendlyName |
| `--cache-control N` | `1800` | SSDP `max-age`; re-announce at N/2 |
| `--announce-interval N` | `cache/2` | seconds between ssdp:alive rounds |
| `--backend stub\|mock-sonosd` | `stub` | audio/transport backend |
| `--mock-deny` | off | mock backend reports the audio path busy |
| `--log-level 0..3` | `2` | error / warn / info / debug |

Stop it with `SIGINT` or `SIGTERM`; it sends `ssdp:byebye` before exiting.

## Protocol surface

| endpoint | purpose |
|---|---|
| `GET /description.xml` | device description, three services |
| `GET /AVTransport/scpd.xml` | AVTransport service description |
| `GET /RenderingControl/scpd.xml` | RenderingControl service description |
| `GET /ConnectionManager/scpd.xml` | ConnectionManager service description |
| `POST /AVTransport/control` | AVTransport SOAP actions |
| `POST /RenderingControl/control` | RenderingControl SOAP actions |
| `POST /ConnectionManager/control` | ConnectionManager SOAP actions |
| `SUBSCRIBE/UNSUBSCRIBE /<service>/event` | GENA subscriptions |

AVTransport: `SetAVTransportURI`, `Play`, `Pause`, `Stop`, `Seek` (`REL_TIME`),
`GetTransportInfo`, `GetPositionInfo`, `GetMediaInfo`, `GetTransportSettings`,
`GetCurrentTransportActions`, `GetDeviceCapabilities`.

RenderingControl: `GetVolume`, `SetVolume` (0–100), `GetMute`, `SetMute`,
`GetVolumeDB` (linear −40…0 dB approximation).

ConnectionManager: `GetProtocolInfo` (sink:
`audio/mpeg`, `audio/flac`, `audio/wav`, `audio/x-wav`, `audio/aac`,
`audio/ogg` over `http-get`), `GetCurrentConnectionIDs`,
`GetCurrentConnectionInfo`.

Errors use the UPnP control namespace with HTTP 500:
`401 Invalid Action`, `402 Invalid Args`, `701 Transition not available`.

SSDP answers `M-SEARCH` for the device type, `upnp:rootdevice`, the UUID,
`ssdp:all`, and each service type, with an MX-delayed unicast reply. It sends
`ssdp:alive` for the same six targets at startup and every
`--announce-interval`, and `ssdp:byebye` on shutdown.

GENA uses a fixed table of 12 subscribers and 8 non-blocking NOTIFY sockets.
Notifications are `LastChange` property sets for AVTransport/RenderingControl,
and the three ConnectionManager variables. A subscriber that misses three
deliveries, or whose SID expires, is dropped.

## Layout

| file | purpose |
|---|---|
| `main.c` | CLI, signal pipe, one `poll()` loop |
| `config.c/h` | defaults, argument parsing, MAC/IP discovery |
| `log.c/h` | level-filtered stderr logging |
| `util.c/h` | bounded string builder, XML scraping, time parsing |
| `backend.c` | `dlna_arbiter` stub and mock-sonosd implementations |
| `renderer.c/h` | URI/metadata/volume/transport state, dirty flags |
| `scpd.c/h` | the three SCPD documents |
| `description.c/h` | `/description.xml` |
| `soap.c/h` | SOAP parsing, action dispatch, UPnP faults |
| `http.c/h` | HTTP/1.1 server, routing, fixed connection pool |
| `event.c/h` | GENA subscribers, LastChange bodies, NOTIFY outbox |
| `ssdp.c/h` | multicast discovery, M-SEARCH replies, alive/byebye |
| `dlna_arbiter.h` | the seam sonosd will implement |

There is no audio path here. `backend.c` contains a no-audio stub and a mock
sonosd; a real `backend_sonosd.c` is the integration point and is documented in
[docs/DLNA.md](../../docs/DLNA.md). `firmware/sonosd` is not modified by this
component.

## Tests

`tests/dlna/test_renderer.py` is a Python 3 stdlib harness. It starts the real
binary, performs a multicast SSDP `M-SEARCH` (with a reported unicast fallback),
fetches the description and SCPDs, drives every SOAP action, checks the fault
codes, subscribes to each service from a real callback server and asserts the
NOTIFY bodies/SEQ numbers, verifies SID expiry and renewal, checks that a dead
callback is dropped after three failed deliveries while the server stays
responsive, measures RSS, and checks `ssdp:byebye`.

```sh
make test
# or
python3 tests/dlna/test_renderer.py --binary firmware/dlna/build/host/dlna-renderer
```

The suite prints `N checks: N passed, N skipped, N failed` and exits non-zero on
any failure. It never opens `/dev`, never changes network configuration, and
binds only loopback plus the SSDP port for discovery.

## Known limitations

- No decoder or `/dev/dsp` output. The stub reports transport state and advances
  a clock; real playback requires the sonosd backend.
- IPv4 only. No IPv6 SSDP/HTTP.
- HTTP/1.1 with `Connection: close`; no keep-alive, no chunked request bodies.
- The SCPD is the honest subset above. Play modes, next-track queueing, record
  media and `SetNextAVTransportURI` are absent and return `401`.
- Position is not streamed as a per-second event; `LastChange` is emitted on
  state and volume changes, and `GetPositionInfo` reads the live position.
- Metadata is capped at 1024 bytes and URIs at 512 bytes; longer values are
  `402 Invalid Args`, not silently truncated.
- `GetVolumeDB` is a linear approximation of the volume scale, not a calibrated
  dB measurement.
- The cross binary is dynamically linked against the pinned glibc toolchain.
  Its ELF ABI note says GNU/Linux 3.2.0 while the speaker kernel is 2.6.35;
  runtime compatibility on the device has **not** been verified in this branch.
- RSS was measured on the x86_64 host, not on a Play:5.
