# firmware/

On-device software.

| | |
|---|---|
| `updater/` | OTA client: verify a signed manifest, stream the image into the inactive slot, verify the read-back, flip `bootslot` — **`make` builds it, see its README** |
| `bootchooser/` | initramfs `/init`: picks, verifies and mounts a rootfs slot, falls back, recovers |
| `sonosd/` | hardware daemon: `/dev/dsp`, `/dev/audioctl`, GPIO, arbiter, MQTT |
| `dlna/` | UPnP AV MediaRenderer: SSDP, SOAP, GENA, test harness |

`sonosd`, `updater/`, `bootchooser/` and `dlna/` build and are tested. The DLNA
renderer speaks the protocol end to end but still uses its no-op/mock backend;
see `dlna/README.md` and `docs/DLNA.md`. `ROADMAP.md` tracks the rest.

The updater and the boot chooser are the two halves of the update path and are
tested together by `tests/ota/run.sh` — every case is a way an update can fail.
