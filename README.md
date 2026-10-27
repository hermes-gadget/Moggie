# Moggie

> **Designed in California. Perfected in Teesside.**

Open firmware for the Sonos Play:5 (Gen 1) — and, in time, the rest of the Fenway family.

Moggie takes a speaker that Sonos has finished with and gives it a second life as a
first-class network audio endpoint: **AirPlay 2**, **DLNA** and **Spotify Connect running
on the device itself**, Home Assistant discovery over MQTT, and signed over-the-air
updates with A/B rollback.

A *moggie* is a British mixed-breed cat. That's the idea: a pedigree speaker running
someone else's software, and none the worse for it.

**Status: pre-hardware.** The build system, OTA toolchain and control plane are being
developed against the vendor firmware dump. Nothing here has been flashed to a speaker
yet. The loader that gets you *onto* the device lives outside this repo for now.

---

## Why

Sonos stopped adding features to the Play:5 Gen 1 years ago, but the hardware is fine:
a 300 MHz PowerPC, 32 MB of RAM, 32 MB of NAND, a Cirrus CS44800 digital amplifier
controller and a proper three-way speaker. It is a good speaker with software that has
outlived its usefulness.

The parts Sonos wrote that talk to the hardware are **GPL**. The parts that don't —
the streaming stack, the cloud dependency, the app — are not. Moggie replaces the second
group and keeps the first.

## What it does

| | |
|---|---|
| **AirPlay 2** | `shairport-sync` 4.x + `nqptp`, on-device |
| **DLNA / UPnP** | minimal renderer, on-device |
| **Spotify Connect** | `cspot` (C++, built for ESP32) |
| **Home Assistant** | MQTT discovery, no YAML |
| **Multi-room** | Snapcast client (optional), or AirPlay 2 grouping |
| **Updates** | signed manifest, A/B slots, automatic rollback |
| **Recovery** | initramfs boot chooser + UART shell |

No cloud. No account. No server required.

## Hardware

| | |
|---|---|
| SoC | MPC8247, 300 MHz, PowerPC big-endian |
| RAM | 32 MB |
| NAND | 32 MB (u-boot 1 MB, jffs2 7 MB, ramdisk 5 MB, rootfs 16 MB, kernel 2 MB, dtb 1 MB) |
| Audio | TDM → Cirrus CS44800 → STA335 amplifiers |
| Console | J25004, 38400 8N1, 3 wires (TX/RX/GND) |

## Architecture in one paragraph

Keep U-Boot. Rebuild the kernel and `audiodev` from Sonos's GPL drop. Replace `anacapad`
with `sonosd`, which owns `/dev/dsp`, `/dev/audioctl` and the GPIO, arbitrates between
audio sources, and speaks MQTT. Split the 16 MB rootfs partition into two 8 MB A/B slots
and boot through a small initramfs chooser that verifies the slot before mounting it.
Every endpoint daemon runs on the device; a server is a bonus, never a dependency.

Full detail: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Build

```bash
make check        # toolchain and dependency check
make toolchain    # fetch and verify the Bootlin e300c3 toolchain
make sources      # fetch the Sonos GPL drop
make kernel       # build the kernel and audiodev.ko
make rootfs       # build the squashfs rootfs (≤ 7.5 MB)
make ota-test     # run the OTA negative tests
```

Everything lands in `out/`, with SHA-256s in `out/artifacts.sha256`.

## Licence

Firmware: **GPL-2.0** (it links against Sonos's GPL `audiodev` module, and we'd rather
be honest about it). Tools and the installer: **MIT**.

See [`LICENSE`](LICENSE) and [`LICENSE-MIT`](LICENSE-MIT).
