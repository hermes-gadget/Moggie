# Stock Play:5 Gen 1 firmware update path — research note

**Status:** Read-only research for the hardware-free Moggie run. No speaker was contacted.

## Source

Artur / `r-u-t-r-A`, [How to update Sonos Zone player 5 (1st gen) to newest firmware available](https://gist.github.com/r-u-t-r-A/1a6dbf8a841162d150877b8f9d4fc24b), posted 2026-10-11. The procedure and success report below are the author's claims; we have not run them on hardware.

## Reported method

For a Play:5 Gen 1 / ZonePlayer S5 (`HardwareVersion` `1.16.4.1-2`, model suffix `1-16`) that is too old to parse the newer update package, the author reports this signed-stock sequence:

```text
16.7-48310 → 34.16-37101 → 57.23-74170 (display 11.16.1)
```

The proposed transport is the speaker's UPnP `BeginSoftwareUpdate` action on port 1400, with official signed `.upd` files served over local plain HTTP. The guide reports the old updater appends `default-1-16.upd` to the supplied URL, whereas 34.16 requests the supplied path directly; the web-server request log is therefore important when diagnosing a 404.

The author's explanation for the bridge hop is that 57.23's package has a `PATCHES` section that the 16.x updater cannot parse, while 34.16 accepts the older layout. Binary inspection below corroborates that the two packages have different metadata layouts, but does not independently prove the parser failure or the full flashing procedure.

## Integrity checks performed locally

Fetched the two files directly from Sonos's `update-firmware.sonos.com` host, into ignored `dl/stock-firmware/` for local inspection. The sizes and SHA-256 values **exactly match the gist**:

| File | Bytes | SHA-256 |
|---|---:|---|
| `34.16-37101-1-16.upd` | 4,727,312 | `84632e02bdce626bf10ae846839e90d66e510a627320fff8690c2bdb998733f2` |
| `57.23-74170-1-16.upd` | 5,606,749 | `e19a7bb28f2d940346c605ccfcff81f8d9a7dd1771ee991841ac1f9e07223ad4` |

Both begin with the same binary magic (`49 7f 16 35`). The 57.23 package contains a metadata record with the visible strings `2021_11_PATCHES`, `2023_01_PATCHES`, `2023_12_PATCHES` and `2024_12_PATCHES`; those markers are absent from the 34.16 file's leading metadata. This is consistent with a package-format change.

## What this means for Moggie

- **Useful bootstrap/recovery knowledge:** there is a reported non-invasive way to move old stock firmware forward using Sonos-signed releases, without an account or a shell exploit.
- **Not an open-firmware installer:** it only installs Sonos-signed stock images through Sonos's own updater. It does not provide root, an unsigned-image signing key, or a path to install Moggie.
- **OTA design remains separate:** Moggie's A/B updater writes its own signed images to the inactive slot and needs the rebuilt boot chooser. A stock `.upd` is not an input to that updater.
- **Potential hardware-day prerequisite:** if the test unit is on an old 16.x build, 34.16 may be a useful candidate bridge before checking whether 57.23 is needed. Confirm the unit's actual `SoftwareVersion`, `HardwareVersion`, and update state first; the gist's reported results are not a guarantee for a different unit.
- **Never try this during the current run:** sending `BeginSoftwareUpdate` is a real firmware-write operation. The active approved run explicitly forbids physical-device interaction.

## Source and caveat

The gist is third-party, with no comments and one fork at the time checked. File hashes are independently confirmed against Sonos's official server; hardware success, the URL rewrite behaviour, and the 16.x parser diagnosis remain author-reported pending a separately authorised hardware session.
