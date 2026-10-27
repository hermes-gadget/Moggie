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
| 3 — `sonosd` | **partial** | real MQTT backend verified against a broker; no audio path yet |
| 4 — Endpoints | **in progress** | DLNA renderer builds, 296 protocol checks; no audio backend |
| 5 — Home Assistant | **partial** | 7 entities verified on a real broker; `media_player` needs `dlna_dmr` (#13) |
| 6 — OTA | **done** | 23/23 negative tests pass |
| 7 — Hardware | **blocked** | needs UART and the speaker |
| 8 — Installer | **not started** | depends on a root path |
| 9 — Other models | **not started** | |

## Verified

```
kernel         vmlinux.bin   4,204,896 B
               gzip payload  1,703,280 B  = 81% of the 2 MB kernel partition
audiodev.ko                     120,720 B  modinfo matches stock
sonosd         272 unit checks + 108 selftest checks, all pass
               cross-compiles: ELF 32-bit MSB PowerPC, links libmosquitto.so.1
MQTT           20/20 integration checks against a real Mosquitto broker
               (availability, retained discovery, multi-key commands, clamping)
OTA            23/23 negative cases pass, no MTD device opened
boot chooser   17/17 slot-selection checks, under dash and busybox sh
compat probe   15/15 on the target ABI, static and dynamic
emulation      6/6; `sonosd --selftest` 108 checks under qemu-ppc
HA             7 entities published and verified; 16 was the design target
dlna           296/296 protocol checks; ELF 32-bit MSB PowerPC, 132,668 B stripped
rootfs         3,069,851 B raw / 3,145,728 B padded = 3.00 MiB; 4,718,592 B spare
               reproducible: two clean builds, byte-identical; every ELF tagged ABI 2.6.35
audio          stock DSP graph mapped; capture tool self-tests 4/4
```

## Known gaps

- **No real audio has been played.** `/dev/dsp` is never opened on a build host.
- **Buildroot is not wired up.** The rootfs figure above is a hand-staged set, not a
  reproducible Buildroot output.
- **NAND behaviour is simulated.** `nandwrite`/`nanddump` sit behind the slotdev
  interface and have not run on real flash.
- **The boot chooser has never booted.** It is tested against files, under `sh`,
  `dash` and busybox `ash` (17 checks), but not by a real kernel. `sonosd` now
  writes `boot_ok`, so the commit half of the handshake exists; the handshake
  itself has still never run on a device.
- **Kernel builds are not byte-reproducible.** No `KBUILD_BUILD_TIMESTAMP`; two
  builds hash differently.
- **`sonosd` MQTT is now real**, but no reconnect/backoff test exists.
- **The stock DSP graph is mapped but not implemented.** `sonosd` has no biquad
  engine and its volume is linear, while the original is dB with a clamp.
- **The DLNA renderer has no audio backend.** Its SSDP/HTTP/SOAP/GENA protocol
  is tested end to end, but playback goes through the stub or mock arbiter;
  `sonosd` does not implement the seam yet.
- **No AirPlay or Spotify endpoint daemons exist yet.**

## Findings that changed the design

1. **The stock kernel has no initramfs support.** `CONFIG_BLK_DEV_INITRD` is unset,
   so the boot chooser only works after our rebuild enables it.
2. **The 7.5 MB budget is the compressed image size**, not the uncompressed rootfs.
   5.87 MiB uncompressed compresses to roughly 2 MiB, leaving far more room for
   endpoint daemons than originally assumed.
3. **HA has no MQTT `media_player`.** Verified against the shipped 2026.10.4 image.
   The player entity has to come from `dlna_dmr` over SSDP instead (#13).
4. **The stock DSP is a configuration graph, not fixed coefficients.** `anacapad`
   runs an IIR block engine and the firmware serves that graph at `GET /getDSP`,
   so hardware day can capture the real crossover/EQ instead of guessing (#17).
5. **Our toolchain's libc needs a newer kernel than the speaker runs.** Stock
   firmware ships glibc 2.23; the pinned toolchain ships glibc 2.44 with
   `Minimum supported kernel: 3.2.0`, against a 2.6.35 device kernel. The kernel
   build is unaffected (no libc), but every userspace binary is suspect. Measured:
   `sonosd` imports nothing newer than 2.6.35; `libmosquitto` adds `getrandom`
   (3.17+) with a documented `/dev/urandom` fallback. Narrowed, not cleared —
   see `docs/TOOLCHAIN.md` and #22.
