# L2 execution surface — continuation handoff, 2026-10-28

**Versioned stock findings delivered; installed-build analysis remains incomplete.**
The original access-only stop has been superseded by read-only analysis of the
brief-named captures and their referenced older package. This does not authorize
device access, flash/bootloader writes, power cycles, service/live-config changes,
credentials, target execution or new sessions/agents.

## Branch and increments

Worktree `/home/ben/Moggie-wt/ota`; branch
`burn/moggie-execution-surface-20261028`, based on default/main
`3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db`.
Committed increments (all pushed before the final handoff update):

| Commit | Coherent result |
|---|---|
| `b75e217`, `54d3832` | Initial custom-source/access-boundary checkpoint |
| `49218b6` | Recovered stock init and package provenance |
| `8458e4e` | Versioned kernel/updater evidence; replaced obsolete report |
| `59af893` | Independent byte checker and deterministic reproduction |
| `adf46de` | Encrypted-header gate and distinct envelope public anchors |
| `a85a26f` | Host-verified legacy signed range and direct-call inventory |
| `aa01191` | Normal sequential phase gate and bounded NAND-writer evidence |
| `d4d4791` | Published evidence/audit checkpoint |
| `e0f7481` | Matching older OOB writer, kernel consumer and raw-image checks |
| `56540e9` | Board NAND setup, table replacement and Linux section choice |

Draft PR: [#63](https://github.com/hermes-gadget/Moggie/pull/63), based on main.
The final handoff update follows these commits on the same branch. Nothing was
merged. The installed-target gaps in the audit remain substantive; publishing
this partial evidence does not declare the original goal achieved.

Primary deliverable: [EXECUTION-SURFACE.md](../hardware/EXECUTION-SURFACE.md).
Reproduction tool: [stock_execution_evidence.py](../../tools/stock_execution_evidence.py).
Evidence packet: [execution-20261028](../hardware/evidence/execution-20261028/packages.json).
Firmware binaries remain only in ignored `build/stock-execution/34.16/`.

## Consequential findings

- Older `34.16-37101-1-16` is extractable big-endian SquashFS 3.0. Fourteen
  allowlisted non-credential files were recovered; seven requested paths are
  absent, including libflash.so.1. The package provenance is attribution, not a
  measured physical model identification (report evidence/applicability section).
- Its kernel decompresses to Linux **2.4.22-rincon**. The separate raw vmlinux
  capture is Linux **2.6.35** with an MPC8315/eLBC FDT; those are not interchangeable
  target proofs (kernels.json and captured-device-tree.json).
- Init's repartition/format/entropy writes precede `/jffs/Configure`, so that
  persistent hook does not establish zero-write execution (numbered Configure,
  repartition.sh and Srandom).
- The old updater has separate legacy and envelope verification handlers.
  The legacy path uses RSA PKCS#1 v1.5/MD5 and a 1024-bit public key located at
  crypto file offset `0xb2c4`; the envelope caller supplies a SHA-256 digest and
  callback-based verification. Envelope trust provenance and complete byte
  coverage remain open (report updater section with file offsets).
- Extended `boot-to-linux.log` directly records S0 KP=1 G51, S1 KP=4 G50 and
  handoff `0x00400000`; the first-log-only recon had called them second-hand.
  One captured selection does not establish wrap/tie/fallback or boot acceptance.
- Captured 57.23 `-1-16` type-3/4/6 inner envelopes remain undecoded. Exact
  installed 57.10 `-1-16` code is not supplied by the reviewed artifacts.

## Completed next queue item: later-envelope framing and older trust selection

Host-only parsing now establishes the three 57.23 encrypted headers, their
314-byte boundaries and one 256-byte wrapped blob apiece. The older parser
recognizes the format, but requires a caller-supplied RSA private-operation
capability; its callback is identity, not public-key derivation. No private
material was inspected and no payload was decrypted (report later-envelope
experiment and encrypted-headers.json).

The older signature callback's two-entry table is also resolved to distinct
RSA-2048 public DER anchors with deny-mask selection; it is separate from the
legacy RSA-1024 anchor (envelope-public-anchors.json). Full caller/coverage and
installed-build claims remain incomplete.

## Completed coverage increment

The old UPD signature now verifies on the host under the captured legacy public
key over exact prefix `[0,0x482180)`, with signature bytes at `0x482190`.
A changed prefix is rejected. Direct executable-section BL decoding identifies
two parser and two install call sites; the network phase functions are preserved,
but virtual/state-machine orchestration remains unproved. This validates one
package/range and does not establish later signatures or device acceptance.

## Completed phase/write-boundary increment

The normal network action-3 path now has a traced state-4/result-zero gate
before install dispatch. Completion stores the action-2 result. Asynchronous
variants and complete state-writer coverage remain open. The previously inspected alternate
userspace writer at `0x8f10` has no direct read/memcmp call; geometry/OOB ioctls and any
kernel verification still need their own trace. A later-family userspace
readback guarantee must not be copied to this function (report updater section).

## Matching older kernel/OOB increment

The older partition-table path uses a different writer, at `U@0x85c8`, rather
than the alternate writer at `0x8f10`. Its OOB magic occupies bytes 8–9 and its
generation bytes 10–11 are low-byte first. The matching captured 2.4 kernel
partition checker consumes exactly those fields and has a final-marker path;
this directly invalidates transfer of the Fenway OOB offsets/endianness.

Raw-kernel link labels are supported by six export-table pointer pairs.
MTD OOB ioctl dispatch is traced through the generic NAND callback and its
status/readback-verification sequence. The captured board initialization now
binds the NAND scan and partition checker to the same global MTD object.
OOB metadata is submitted before the associated data page; atomic commit and
U-Boot rules remain unproved. See the new report section
and kernel/partition-program evidence, all checked against original file bytes.

## Table replacement and Linux section-choice increment

The changing-table branch erases/writes the table at logical good block 1
before destination invalidation and component programming. Its immediate
caller does not test the table helper's return value. This is a bounded local
control-flow finding; downstream handling and power-cut outcomes remain open.

The matching kernel checks component generations by section and chooses section
1 only when its generation is strictly greater than section 0's; a positive
tie selects section 0. Its stores match the captured bootsection/bootgeneration
sysctl table. This is Linux partition selection after kernel entry, not proof
of U-Boot selection, wrap handling or successful userspace-health rollback.

## Kernel data-readback and partition ioctl increment

The payload write now traces through character MTD offset 0x114 to the NAND
page programmer, status gate and device-buffer comparison before completion.
The partition vendor ioctls are also bound: 0x20 snapshots partition reads into
RAM, 0x21 frees that cache and 0x22 updates in-memory offset/size. The root
partition snapshot request precedes root programming. These mechanisms do not
authenticate payloads or make table/OOB/data writes atomic.

## Verification executed so far

The report's generation and independent-check commands were executed with
existing host tools. A second generation uses the same cache and a separate
output directory, then the checker compares every file. Current commands,
counts, negative checks and report hash are preserved in the
[validation receipt](../hardware/evidence/execution-validation-20261028.txt).
This includes source hashes, disassembly bytes, public-anchor/record/header
metadata, document links/offsets, and the explicit older RSA/MD5 verification.

Five malformed framing cases, altered instruction bytes, changed envelope
boundaries and an altered signed prefix were rejected. These checks do not
certify complete RE semantics, current-device applicability or hardware behavior.
Draft PR: https://github.com/hermes-gadget/Moggie/pull/63 (OPEN, draft;
no CI checks reported at the last inspection). No merge was performed.

Final receipt: 20 UPD records, 14 extracted hashes, 7,440 source-matching
disassembly rows; 62 document links and 66 cited offset starts checked; all 87
regenerated files identical. Older legacy signature validates the exact prefix
and rejects alteration. Latest report SHA-256:
`dc5f4b3ac2948a17909aa7262bfb1756a07e9481162076689dae088ff51b3f6d`.

## Requirement audit

| Brief requirement | Evidence now obtained | Remaining limit |
|---|---|---|
| Init, services, watchdog, mounts, writable media | Older-family init/services, aliases and explicit persistent writes | Matching installed build, actual mounts/media and full watchdog trace missing |
| Every updater verification point/key, slots, boot flags/boot_ok/rollback | Two older handler entries, primitive/key metadata, local validation/install and destination arithmetic | Complete authenticated ranges/callers, envelope trust and physical commit/rollback unproved |
| NFS/TFTP/recovery/debug/maintenance zero-write boot | Each named route classified; old NFS literal preserved as a lead | No reachable zero-write route established; support/reachability kept unknown |
| Ranked routes and single safest experiment | Report table and pinned later-envelope host-only experiment | Ranking is provisional; no device experiment approved |
| Reconcile prior map | Five reports read; explicit correction table | Physical target map still unknown |
| Report CONFIDENCE/UNKNOWN-OFFLINE + handoff/hash | Report, handoff, independently checked packet and validation receipt/hash present | Evidence-integrity checks do not close substantive analysis gaps |
| Commit + push + PR | Stock evidence and verification pushed; draft PR #63 open | Later increments continue on the same branch; no merge |

## Ordered remaining queue

1. Trace watchdog/reset ownership and updater failure paths, now that data/OOB
   readback and vendor partition RAM callbacks join the recovered board/table
   sequence. Preserve the normal sequential gate;
   asynchronous/virtual variants and complete state writers remain unclosed.
2. Close remaining indirect/asynchronous verification and state-writer coverage
   where the captured bytes support it. Keep unknown physical layout and U-Boot
   semantics explicit.
3. To close installed-build claims, obtain the path/provenance of an
   already-captured exact matching 57.10 `-1-16` image. Do not repeat device probes
   or substitute Fenway/34.16 evidence for absent target bytes.

No source/runtime test suite is applicable to unchanged firmware behavior.
Nothing was merged, deployed, restarted or written on the speaker. No claim of
full brief/goal completion is made while the audit above remains incomplete.
