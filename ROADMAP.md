# Roadmap

Status legend: `done` · `in progress` · `blocked on hardware` · `planned`

## Phase 0 — Repo and reproducibility — `in progress`
- [x] Repo scaffold, licences, architecture
- [ ] Pinned toolchain with verified hashes
- [ ] `make` targets that build from a clean checkout
- [ ] Artifact hashes recorded per build

## Phase 1 — Build the vendor software — `planned`
- [ ] Extract the stock kernel config
- [ ] Rebuild `vmlinux` with the pinned toolchain; diff against stock
- [ ] Rebuild `audiodev.ko`; compare `modinfo` and symbols
- [ ] Confirm `CONFIG_MTD_CMDLINE_PARTS` and `CONFIG_SQUASHFS`

## Phase 2 — Root filesystem — `planned`
- [ ] Buildroot `e300c3` config, squashfs output
- [ ] Hit the ≤ 7.5 MB budget
- [ ] `rcS` brings up `/jffs`, `sonosd`, endpoints
- [ ] Boot test in emulation

## Phase 3 — `sonosd` — `planned`
- [ ] PCM path via `/dev/dsp`
- [ ] Volume/filters via `/dev/audioctl`
- [ ] Arbiter with tests
- [ ] MQTT state and commands
- [ ] GPIO: buttons, LED ring, thermal, accelerometer

## Phase 4 — Native endpoints — `planned`
- [ ] `shairport-sync` + `nqptp` cross-build
- [ ] `cspot` cross-build
- [ ] DLNA renderer
- [ ] Optional `snapclient`
- [ ] RSS measurement on hardware ← **blocked on hardware**

## Phase 5 — Home Assistant — `planned`
- [ ] Discovery payload generator
- [ ] Docker test rig (mosquitto + HA)
- [ ] Announcements and ducking

## Phase 6 — OTA — `planned`
- [ ] Manifest format + signer
- [ ] Updater with streaming NAND writes
- [ ] Boot chooser initramfs
- [ ] Negative tests: corrupt, truncated, bad signature, power cut

## Phase 7 — Hardware session — `blocked on hardware`
- [ ] UART capture and U-Boot capability probe
- [ ] Root
- [ ] Full mtd dumps, verified and stored off-device
- [ ] First flash, read-back verification
- [ ] Audio verification

## Phase 8 — Installer — `planned`
- [ ] Discovery
- [ ] Network install without opening the case
- [ ] Rollback to stock
- [ ] Packaging for linux/macos

## Phase 9 — Other Fenway models — `planned`
- Play:1, Play:3, Sub Gen 1 share the platform. Model 8 is first.
