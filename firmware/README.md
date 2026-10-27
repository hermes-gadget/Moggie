# firmware/

On-device software.

- `sonosd/` — hardware daemon: `/dev/dsp`, `/dev/audioctl`, GPIO, arbiter, MQTT
- `updater/` — OTA client: verify, stream to the inactive slot, flip, verify
- `bootchooser/` — initramfs that picks, verifies and mounts a rootfs slot
- `dlna/` — minimal UPnP renderer

Nothing here ships yet. `sonosd/` builds and is unit-tested; `updater/`,
`bootchooser/` and `dlna/` are not written. See `ROADMAP.md`.
