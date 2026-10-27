# sonosd

The hardware daemon for Moggie. Owns `/dev/dsp` (PCM out), `/dev/audioctl`
(volume, mute, filters), arbitrates between audio sources, and publishes state to
MQTT for Home Assistant.

Target: PowerPC MPC8247, 32 MB RAM, Linux 2.6.35. C99, no dynamic allocation
anywhere in the daemon, everything sized at compile time.

## Build

```sh
make            # host build, real device backends          -> ./sonosd
make mock       # SONOSD_MOCK=1, no device node is opened   -> ./sonosd_mock
make test       # unit tests + libmosquitto backend syntax check
make clean
```

Cross build (the only way `CROSS_COMPILE` is honoured — an ambient value in the
environment is ignored deliberately):

```sh
make CROSS_COMPILE=powerpc-e300c3-linux-gnu- CFLAGS="-Os -g"
```

`libmosquitto` is optional. The Makefile probes with `pkg-config`; when it is
missing the daemon is linked with the in-RAM mock MQTT backend and says so at
startup (`mqtt backend: mock`).

## Run

```sh
./sonosd_mock --selftest     # in-process checks, never touches /dev or a socket
./sonosd_mock --once --mock  # start up, publish discovery + state, exit
./sonosd_mock --mock         # full loop (Ctrl-C to stop)
./sonosd --config /etc/sonosd.conf
```

`--selftest` forces the mock audio backend and the mock MQTT backend even in a
binary that was built with the real ones.

## Layout

| file | purpose |
|---|---|
| `main.c` | event loop, MQTT command dispatch, shutdown |
| `config.c/h` | flat `key = value` config, fixed buffers, strict unknown-key errors |
| `log.c/h` | level-filtered logging to stderr and optionally syslog |
| `volume.c/h` | volume/tone state machine (pure logic, unit tested) |
| `dsp.c/h` | backend vtable, single owner of the volume state |
| `dsp_linux.c` | `ioctl()`/`write()` on `/dev/dsp` and `/dev/audioctl` |
| `mockdev.c/h` | in-memory device, fault injection |
| `arbiter.c/h` | one audio source at a time: condvar FIFO + `fcntl` lock file |
| `mqtt.c/h` | topics, state, HA discovery |
| `mqtt_mock.c` | in-RAM broker (default when libmosquitto is absent) |
| `mqtt_mosquitto.c` | real backend, compiled only with the library |
| `jsonlite.c/h` | flat `{"key":value}` reader for command payloads |
| `selftest.c` | `--selftest` checks |
| `audioctl_abi.h` | mirror of the GPL `dspdev.h` / `audioctl.h` ioctl ABI |
| `tests/` | plain-C unit tests (arbiter, volume, jsonlite, config) |

## MQTT

Base topic `sonos/<device_id>`:

| topic | direction | payload |
|---|---|---|
| `state` | out, retained | `{"state","source","volume","mute","bass","treble","balance","loudness","night_mode"}` |
| `cmd` | in | `{"volume":42}`, `{"volume_delta":-2}`, `{"mute":true}`, `{"bass":3}`, `{"treble":-1}`, `{"balance":0}`, `{"loudness":true}`, `{"night_mode":false}`, `{"cmd":"volume_up"\|"volume_down"\|"mute_toggle"\|"status"}` |
| `availability` | out, retained | `online` / `offline` (last will in the real backend) |

Discovery config goes to `homeassistant/media_player/<device_id>/config`.

## Config keys

```
device_id, device_name
mqtt_host, mqtt_port, mqtt_user, mqtt_pass, mqtt_base, mqtt_keepalive
arbiter_lock, initial_volume, volume_step
log_level (0..3), use_syslog, led_enabled
dsp_path, audioctl_path, dsp_ioctl_node (ctl|pcm), mock
```

## Known gaps

- GPIO buttons and the LED ring are not implemented yet.
- Tone/filter values are write-only on the DSP; the daemon's state is the only
  readable copy, so it resets to flat on restart.
- `DSPDEV_*` volume/filter handlers are not in the GPL drop; see
  `audioctl_abi.h` for what is assumed and `dsp_ioctl_node` for the workaround.
