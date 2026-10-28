# ZP5S execution surface — offline evidence boundary

**Status: BLOCKED. The stock boot/init and updater reconstruction is incomplete.**
No stock owner-code execution route is proved by the evidence inspected here. This
is an evidence limit, not proof that no such route exists. The requested stock
captures have not been read because the brief names directories outside the
worktree but limits file analysis to the worktree; see the access gate below.

Analysis date: 2026-10-28. Repository source baseline:
`3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db` (`origin/main`). All source line
references below refer to that commit, not to the original `feat/ota-toolchain`
checkout. The branch was created as
`burn/moggie-execution-surface-20261028` from that default-branch baseline.

## Access gate and evidence inventory

The governing brief is
`/home/ben/.review-notes/2026-10-28-sol-burn/L2-moggie-execution-surface.md`.
Its lines 8–9 identify these inputs, which remain **uninspected**:

| Input directory | Named evidence needed |
|---|---|
| `/home/ben/.hermes/plans/2026-10-28-moggie-recon-output/` | `BOOT-AND-MTD-MAP.md`, `ROOTFS-UPDATER-ANALYSIS.md`, `HARDWARE-STORAGE-MAP.md`, `SAFE-HARDWARE-NEXT-STEPS.md`, `SOFTWARE-ARCHITECTURE.md` |
| `/home/ben/sonos-bench-analysis/` | Captured kernel, vmlinux, boot logs; rootfs location/provenance to be established |
| `/home/ben/sonos-bench/logs/` | Captured stock observations and command responses |

The same brief permits “read/analyse files inside YOUR cwd” (line 30) and requires
“STOP and return BLOCKED” for a needed action outside its envelope (line 33).
**Exact unblock request:** permit read-only analysis of the three named
directories, with any derived files created only in this worktree. This would
not authorize hardware access, writes to NAND/NOR, flashing, bootloader changes,
power cycling, service changes, or reading unrelated live configuration/secrets.
No response to that request is assumed by this document.

The available repository contains replacement-firmware source. Its architecture
explicitly calls the initramfs “ours” and proposes splitting the rootfs into two
slots (`docs/ARCHITECTURE.md:5–20`). It cannot establish stock behavior.
The brief's statements about the three U-Boot commands, the `0x00400000` handoff,
and signed official updates are **brief-supplied premises only** (brief line 10);
they have not been independently checked against a capture in this analysis.

A local filename/size inventory, without reading file contents, found these
image/log candidates on 2026-10-28:

```text
build/tmp/asan-suite.log                 810 bytes
build/tmp/smoke/mtd3.img             16777216 bytes
build/tmp/smoke/rootfs.squashfs          4096 bytes
build/tmp/smoke/ch/slots/slot_a.img      4096 bytes
build/tmp/smoke/ch/slots/slot_b.img      4096 bytes
```

No `kernel.raw`, `vmlinux.raw`, `.upd`, or named recon report was found in the
worktree. These build-directory files have no established stock-capture
provenance and were not substituted for the requested stock images. Inventory
method: Python `os.walk('.', followlinks=False)`, excluding `.git`, `.tokensave`,
`node_modules`, and `.pio`; suffixes `.raw`, `.img`, `.upd`, `.squashfs`, `.sqsh`,
`.ubifs`, `.jffs2`, `.log`. `git ls-tree -r --name-only origin/main` also contains
none of the named recon reports. No claimed absence extends outside the
inspected worktree or beyond those inventory criteria.

## Init, supervision, mounts and persistence

**Stock sequence, watchdog, mount table and writable-media mapping: UNKNOWN.**
They require the captured rootfs and boot/mount observations (brief tasks 1–2,
lines 13–14). The table below is a source reconstruction of **Moggie only**,
included to prevent its design from being mistaken for a stock observation.

| Stage or path | What Moggie source establishes | Source |
|---|---|---|
| Chooser entry | Mount pseudo filesystems and state, read `bootslot`, try the preferred slot followed by the other | `firmware/bootchooser/init:169–186,287–316` |
| Root handoff | Mount chosen squashfs read-only, require an init executable, move `/proc`, `/sys`, `/dev`, then `switch_root` | `firmware/bootchooser/init:234–251` |
| PID 1 and services | BusyBox inittab runs `rcS`; respawns console getty, `sonosd`, and `dlna-renderer` | `buildroot/board/moggie/rootfs-overlay/etc/inittab:3–15` |
| `/proc`, `/sys` | `rcS` mounts proc and sysfs | `buildroot/board/moggie/rootfs-overlay/etc/init.d/rcS:17–19` |
| `/tmp`, `/run`, `/var/log` | Requested tmpfs mounts; volatile when those mounts succeed | `buildroot/board/moggie/rootfs-overlay/etc/init.d/rcS:21–26` |
| `/dev` | Attempts devtmpfs if advertised, otherwise tmpfs plus mdev | `buildroot/board/moggie/rootfs-overlay/etc/init.d/rcS:28–46` |
| `/jffs` | Attempts JFFS2 on `/dev/mtdblock1`; fstab requests `rw,noatime`; the architecture assigns mtd1 to settings/state | `buildroot/board/moggie/rootfs-overlay/etc/init.d/rcS:54–65`; `buildroot/board/moggie/rootfs-overlay/etc/fstab:13`; `docs/ARCHITECTURE.md:32–39` |
| Network and SSH | `rcS` attempts DHCP/link-local setup and optional key-only Dropbear gated by files in `/jffs/ssh` | `buildroot/board/moggie/rootfs-overlay/etc/init.d/rcS:67–109` |
| Recovery | Chooser can invoke a configured command, attempt telnetd, or exec a shell after both candidates fail | `firmware/bootchooser/init:257–282,316–347` |

These are instructions in source, not a captured successful mount table.
Moggie's persistent-state writes include `bootslot`, `running_slot`, failure
counters, slot status, and consumption of `boot_ok`
(`firmware/bootchooser/init:48–53,192–203,214–223,298–339`). Thus even its
read-only squashfs handoff is **not** an end-to-end zero-persistent-write boot.
Stock watchdog ownership, feeding interval, reset causes, startup hooks, actual
mount success and underlying NAND/NOR mapping remain unknown.

## Updater and boot-selection trust boundaries

**Every stock verification point, stock primitive/key location, physical slot
layout, boot flags, `boot_ok` semantics and rollback policy: UNKNOWN.** The
following chain describes the repository's custom updater and chooser only.

1. **Manifest authentication.** `ota_manifest_load` reconstructs the unsigned
   body, parses it, checks canonical form, hashes those bytes with SHA-256,
   base64-decodes a 256-byte signature and calls `rsa_verify_sha256`
   (`firmware/updater/manifest.c:360–401`). The verifier computes the RSA public
   operation and compares the full PKCS#1 v1.5 encoded SHA-256 DigestInfo block
   (`firmware/updater/rsa.c:131–183`). This is RSA-2048 / PKCS#1 v1.5 / SHA-256
   for **Moggie**, not a finding about Sonos's stock verifier.
2. **Trust anchor.** The normal build takes the modulus and exponent from the
   compiled header (`firmware/updater/updater.c:59–65,183–186`). The checked-in
   `pubkey_dev.h:1–13` identifies a placeholder 2048-bit key, exponent 65537,
   with release replacement required. No stock key or stock key offset has
   been identified here; no private key was read.
3. **Input and target gates.** Signature verification precedes model, size and
   image checks (`firmware/updater/updater.c:419–479`). `--check` hashes and
   compares the image, then returns without opening a slot
   (`firmware/updater/updater.c:483–500`). An install requires `running_slot`
   rather than inferring it from the next-boot `bootslot`, rejects the running
   slot, and allowlists the rootfs device
   (`firmware/updater/updater.c:503–566`).
4. **Persistence and readback.** The installer marks the target `writing`, streams
   it, compares the streamed hash, reads it back and compares again. It then
   records size/hash/version/status, changes `bootslot`, clears `boot_ok`, syncs
   and optionally reboots (`firmware/updater/updater.c:569–659`). Hash comparisons
   are integrity checks anchored to the authenticated manifest; they are not
   additional public-key signature verifications.
5. **A/B layout.** Moggie defines two 8 MiB ranges inside a 16 MiB rootfs device
   (`firmware/updater/slotdev.h:23–28`), while the chooser seeks named
   `rootfs_a`/`rootfs_b` partitions (`firmware/bootchooser/init:58–86`). The
   architecture requires a rebuilt kernel with command-line partition support
   (`docs/ARCHITECTURE.md:68–74`). None proves that stock has those partitions.
6. **Boot acceptance and fallback.** The chooser checks status, squashfs magic,
   and the stored hash if present (`firmware/bootchooser/init:103–150`). A
   matching `boot_ok` resets failures, sets status `ok`, and truncates the flag
   (`firmware/bootchooser/init:192–203`). Otherwise it counts attempts before
   handoff, skips a slot at the default limit of two, and tries the other
   (`firmware/bootchooser/init:30–36,316–347`). This is a health fallback rule,
   not evidence of stock cryptographic downgrade protection.
7. **Producer of `boot_ok`.** `sonosd` calls `boot_ok_commit` at readiness
   (`firmware/sonosd/main.c:364–379`); the helper reads `bootslot`, writes a
   temporary flag, fsyncs, renames, and attempts a directory fsync
   (`firmware/sonosd/bootstate.c:78–136`). The chooser declares
   `BOOT_OK_TIMEOUT=60` but does not run a 60-second timer; its decision is made
   at the next chooser invocation (`firmware/bootchooser/init:36,192–203,316–339`).

The existing stock-update note describes a third-party report and explicitly
separates official `.upd` installation from Moggie's updater
(`docs/STOCK-FIRMWARE-UPDATE.md:7–19,32–42`). Its recorded package hashes and
metadata markers (`:23–30`) are not a disassembly of the stock signature chain.
The cited website and update server were not accessed in this analysis.

## Candidate zero-write routes: evidence and priority

Here, “zero-write” requires avoiding persistent state writes as well as image
writes. A RAM-resident payload alone does not establish that property. **There
is no proved stock execution route to rank by demonstrated success.** The
ordering below ranks follow-up evidence work by expected information value and
potential intervention risk; those assessments are engineering judgment, not
measured device behavior.

| Priority | Candidate | Current evidence / verdict | Information value and risk gate |
|---|---|---|---|
| 1 | Existing stock startup/debug hook loading code from a volatile path | UNKNOWN: stock rootfs/init and mount backing uninspected | Highest initial static-analysis priority. Must prove reachability, privileges, volatile storage and absence of associated persistent writes before any device proposal. Moggie's console/SSH settings above do not establish stock hooks. |
| 2 | Recovery or maintenance environment executing in RAM | UNKNOWN: no stock recovery entry, verifier, write behavior or maintenance image inspected | Potentially high value. Do not trigger a mode that might update, repair, reset or mark flash state. Moggie's recovery shell is custom source, not stock proof. |
| 3 | NFS root | UNKNOWN: stock kernel configuration, boot arguments, network-root support and a way to select it uninspected | Requires both support and a reachable selection mechanism. A kernel handoff address from brief line 10 proves neither. Boot-argument changes are outside this run. |
| 4 | TFTP / network RAM load and boot | UNKNOWN / not established by the brief's restricted command inventory | If the only exposed commands really are `version`, `echo`, `test` (brief line 10), no exposed loader/boot command is established. This does not prove compiled support absent. No environment changes, hidden-command probing or device packets are authorized. |
| 5 | Runtime service/debug interface providing owner-code execution | UNKNOWN: stock binaries, listeners and request handlers uninspected | Static call tracing may be useful; request side effects and watchdog behavior are unknown. No live probing or speculative exploit is proposed. |
| Exclude | Official updater as a zero-write custom-image boot mechanism | No such facility is established by the inspected evidence; local stock-update note describes installation of signed stock images | A reported stock install is not proof of unsigned execution or RAM-only boot (`docs/STOCK-FIRMWARE-UPDATE.md:32–38`). Do not invoke `BeginSoftwareUpdate`. |
| Exclude | Moggie chooser / A/B updater bootstrap | Source establishes persistent state and image writes, above | Does not qualify as zero-write and presupposes replacement firmware. |

This inventory covers the route families named by the brief plus ordinary
startup and runtime-service hooks. Exhaustive enumeration of the stock binary's
execution hooks is blocked by the evidence gate, so no exhaustive claim is made.

**Single safest next supervised experiment:** after the access gate is resolved,
perform a host-only extraction and boot-chain reconstruction of the already
captured stock rootfs. Record the input hash/provenance, identify its image
format, extract into this worktree without executing target programs, and trace
PID 1 through service/watchdog setup, mounts and updater call sites. Stop if the
capture is absent or incomplete. This adds no device interaction and is the
lowest-risk way to decide whether any later hardware experiment is justified.
No next device experiment is justified as zero-write by the present evidence.
This experiment has **not** been executed here.

## Reconciliation with the prior boot/MTD map

**BLOCKED:** `BOOT-AND-MTD-MAP.md` has not been read. No claim about its contents
or a completed reconciliation is made. The following repository assumptions
must be checked against it when access is permitted:

| Assumption to test | Existing evidence and present disposition |
|---|---|
| Stock uses Moggie's A/B, `bootslot`, `running_slot`, `boot_ok` or two-failure policy | Invalid inference from this repo: architecture explicitly proposes a new A/B split (`docs/ARCHITECTURE.md:5–20`); stock remains unknown. |
| mtd0 is the bootloader, mtd2 an initramfs and mtd4 the kernel on the captured device | These are Moggie architecture assignments (`docs/ARCHITECTURE.md:32–39`), not captured stock MTD evidence. Do not use them as device offsets. |
| Preserving mtd0 or booting a read-only root means zero persistent writes | Invalid for the custom chooser: it syncs state writes before root handoff (`firmware/bootchooser/init:48–53,214–223,329–339`). Stock behavior remains unknown. |
| UART implies a usable U-Boot recovery shell or `printenv` | Existing hardware-day instructions request `printenv` (`docs/HARDWARE-DAY.md:31–35`), but brief line 10 supplies only three commands. Treat the checklist as unverified guidance; do not infer a loader or flash-recovery capability. |
| RSA-2048/SHA-256 in this repo identifies the stock signature primitive/key | Invalid inference: the inspected chain belongs to `moggie-updater`; the compiled key is explicitly a placeholder (`firmware/updater/pubkey_dev.h:1–13`). |
| A step headed “read-only” necessarily avoids writes | Invalid for the existing checklist: `docs/HARDWARE-DAY.md:51–54` directs a kernel write. It is excluded from this lane, regardless of heading. |

These are corrections to interpretations of repository material, not a claim
that the unread prior recon contains those errors. The complete map delta,
including NAND versus NOR and actual offsets, remains outstanding.

## CONFIDENCE

| Finding | Confidence and limit |
|---|---|
| Repository implements a custom signed A/B update and boot-selection scheme | HIGH as static source analysis at the pinned baseline, with line citations above; not a hardware validation. |
| Custom chooser/install path is not zero-persistent-write | HIGH from explicit state writes, syncs and install flow; no such code was executed on a device. |
| Stock primitive, keys, slots, watchdog, mounts or RAM-only boot capability | UNKNOWN. Custom source and brief assertions are insufficient substitutes for captured stock evidence. |
| Follow-up route ordering | LOW / provisional judgment pending stock rootfs and boot-map inspection. |
| No stock route proved in this analysis | HIGH as a statement of this analysis's evidence boundary; not a universal impossibility claim. |

## UNKNOWN-OFFLINE

- **Uninspected named inputs:** rootfs image and format, input hashes and capture
  provenance; the prior five recon reports; captured kernel/vmlinux and logs.
  Resolve the directory-read gate before inspecting them (brief lines 8–9,30–33).
- **Stock boot/init:** actual PID 1, init scripts, service supervision, startup
  order, watchdog feeder/reset rules, mount options, writable aliases and
  persistent backing. Task 1 is incomplete (brief line 13).
- **Stock update trust chain:** all verification entry points, authenticated byte
  ranges, signature primitive, hash, padding, key location/rotation, pre-write
  versus post-write checks and recovery/maintenance verification. Task 2 is
  incomplete (brief line 14).
- **Stock boot selection:** actual physical partitions and medium, any A/B
  scheme, update/boot flags, `boot_ok` producer/consumer (if present), retry and
  rollback rules, and persistent writes during normal boot. No stock semantics
  are inferred from Moggie's files.
- **Zero-write reachability:** NFS/TFTP support, exposed selection mechanism,
  recovery/maintenance/debug-hook reachability and every associated persistent
  side effect. Task 3 is unresolved; route rankings remain provisional (brief
  lines 15–16).
- **Prior-map delta:** every contradicted assumption in `BOOT-AND-MTD-MAP.md`
  needs direct comparison. Task 5 is incomplete (brief line 17).
- **Limits even after static inspection:** presence of code does not establish
  live reachability, credentials, actual device state or lack of all incidental
  writes. Those may remain unknown offline and must not become guessed facts.

No device was accessed, target program executed, bootloader changed, flash
written, or power cycle performed for this report. Documentation checks and
hashes attest to this report's integrity, not completion of the blocked stock
analysis.
