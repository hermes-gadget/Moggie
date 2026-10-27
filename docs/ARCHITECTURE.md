# Architecture

## Constraints first

The MPC8247 has **32 MB of RAM** and a **300 MHz** core. Every decision below follows
from that, plus one more: there is no A/B layout in the stock flash, so we have to make
one out of the 16 MB rootfs partition.

## Boot chain

```
U-Boot (mtd0, never written)
  └─ kernel (mtd4)
       └─ initramfs (mtd2, ours)  ← boot chooser + recovery
            └─ squashfs rootfs_a or rootfs_b (mtd3, two 8 MB slots)
                 └─ switch_root
```

The boot decision lives in **userspace**, not in the U-Boot environment. That keeps the
bootloader — the only unbrick path — completely untouched.

The chooser:

1. reads `/jffs/bootslot` (`a` or `b`)
2. verifies the slot's squashfs header and stored SHA-256
3. mounts it and `switch_root`s
4. on failure, tries the other slot
5. if both fail, starts a recovery shell on UART and network

## Flash map

| mtd | name | size | use |
|---|---|---|---|
| 0 | u-boot | 1 MB | untouched |
| 1 | jffs2 | 7 MB | settings, state, boot flags |
| 2 | ramdisk | 5 MB | boot chooser + recovery initramfs |
| 3 | rootfs | 16 MB | **A/B: rootfs_a (8 MB) + rootfs_b (8 MB)** |
| 4 | kernel | 2 MB | rebuilt 2.6.35 |
| 5 | dtb | 1 MB | spare |

Rootfs budget: **≤ 7.5 MB per slot**. This is the binding constraint on features.

## Userspace

```
sonosd                     owns the hardware
 ├── /dev/dsp              PCM out (DSPDEV ioctls)
 ├── /dev/audioctl         volume, filters, mute
 ├── GPIO                  buttons, LED ring
 ├── arbiter               one audio source at a time
 └── MQTT                  HA discovery + state

shairport-sync + nqptp     AirPlay 2      ─┐
cspot                      Spotify Connect ├─ all feed the arbiter
dlna-renderer              DLNA           ─┘
snapclient (optional)      multi-room     ─┘
```

### Source arbitration

Every endpoint listens all the time, but only one owns the audio path. A source calls
`arb_request()`; `sonosd` parks the previous one, switches `/dev/dsp`, reapplies the saved
volume, and announces the change on MQTT. Idle daemons cost a socket and an mDNS
advertisement — that's how the whole stack fits in 32 MB.

Rough peak with one active decoder: ~21–23 MB.

## Kernel and driver

Rebuilt from Sonos's GPL drop, same version (2.6.35). Required config additions:

- `CONFIG_MTD_CMDLINE_PARTS` — for the A/B sub-partitions
- `CONFIG_SQUASHFS` + `CONFIG_SQUASHFS_XZ` (or zlib) — for the rootfs

`audiodev.ko` is built from `drivers/audiodev/` in the drop. It is GPL v2 and ships with
the firmware; we do not redistribute anything Sonos hasn't published.

## OTA

1. fetch `manifest.json` (version, model, size, sha256, RSA-2048/SHA-256 signature)
2. verify signature against a key compiled into the firmware
3. stream the image into the **inactive** slot with `nandwrite` — never buffer in RAM
4. `nanddump` it back and compare hashes
5. write `/jffs/bootslot`, `sync`, reboot
6. new slot boots, `sonosd` writes `/jffs/boot_ok` within 60 s
7. no `boot_ok` → chooser counts a failure; two failures → boot the other slot

Invariants: never write mtd0, never write the running slot, never apply an unverified
image, never mark a slot bootable without a read-back.

## Home Assistant

MQTT discovery, published retained to `homeassistant/<component>/moggie_<id>/config`.

`media_player`, bass/treble/balance as `number`, loudness/LED/night mode as `switch`,
temperature/uptime/buffer as diagnostic sensors, identify/reboot as `button`, and an
`update` entity that triggers the OTA path.

## Deliberately out of scope

- Modernising the kernel. It's an appliance on a LAN.
- On-device decoding of Spotify's native formats beyond what `cspot` handles.
- Anything requiring a Sonos account, cloud service, or vendor binary.
