# Status

_Last updated: 2026-10-27 (hardware-free run)._

Nothing here has touched a speaker. Everything below was built and tested against
the vendor firmware dump and Sonos's GPL source drop.

## Phases

| Phase | State | Evidence |
|---|---|---|
| 0 — Repo | **done** | builds from clean, pinned toolchain by hash |
| 1 — Kernel + `audiodev` | **done** | `make kernel` → vmlinux + module, size-gated |
| 2 — Rootfs | **partial** | minimal rootfs measured at 5.87 MiB; Buildroot not wired up |
| 3 — `sonosd` | **partial** | daemon + arbiter + mock backend; no real audio path yet |
| 4 — Endpoints | **not started** | shairport-sync / cspot / DLNA |
| 5 — Home Assistant | **done** | 16 entities, live HA rig verified |
| 6 — OTA | **done** | 23/23 negative tests pass |
| 7 — Hardware | **blocked** | needs UART and the speaker |
| 8 — Installer | **not started** | depends on a root path |
| 9 — Other models | **not started** | |

## Verified

```
kernel         vmlinux.bin   4,204,896 B
               gzip payload  1,703,280 B  = 81% of the 2 MB kernel partition
audiodev.ko                     120,720 B  modinfo matches stock
sonosd         263 unit checks + 108 selftest checks, all pass
               cross-compiles: ELF 32-bit MSB PowerPC, 81,656 B
OTA            23/23 negative cases pass, no MTD device opened
HA             16 entities, 28 retained topics verified on a live rig
rootfs         minimal set 6,152,200 B = 5.87 MiB uncompressed
```

## Known gaps

- **No real audio has been played.** `/dev/dsp` is never opened on a build host.
- **Buildroot is not wired up.** The rootfs figure above is a hand-staged set, not a
  reproducible Buildroot output.
- **NAND behaviour is simulated.** `nandwrite`/`nanddump` sit behind the slotdev
  interface and have not run on real flash.
- **The boot chooser has never booted.** It is tested against files, under `sh`,
  `dash` and busybox `ash`, but not by a real kernel.
- **Kernel builds are not byte-reproducible.** No `KBUILD_BUILD_TIMESTAMP`; two
  builds hash differently.
- **`sonosd` MQTT is the mock backend.** The libmosquitto backend compiles but has
  not been run against a broker.
- **No endpoint daemons exist yet** — no AirPlay, Spotify or DLNA.

## Two findings that changed the design

1. **The stock kernel has no initramfs support.** `CONFIG_BLK_DEV_INITRD` is unset,
   so the boot chooser only works after our rebuild enables it.
2. **The 7.5 MB budget is the compressed image size**, not the uncompressed rootfs.
   5.87 MiB uncompressed compresses to roughly 2 MiB, leaving far more room for
   endpoint daemons than originally assumed.
