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
Initial custom-source report commits: `b75e217`, `54d3832`.
Stock-init increment: `49218b6`, already pushed. Stock updater/kernel evidence
and replacement of the obsolete access-gated report: `8458e4e`. The next commit
adds independent evidence checks and their receipt. Envelope framing/key
selection is committed in `adf46de`; later commits preserve the validated legacy
signed range and direct-call inventory.

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

1. Finish older updater virtual/network phase orchestration, remaining
   authenticated-range cases, and inline flash/OOB commit ordering. The pinned
   legacy prefix is verified; other paths must retain their own evidence limits.
2. Trace watchdog/reset ownership and remaining kernel boot-selection logic from
   existing captures. Keep unknown physical layout and U-Boot semantics explicit.
3. To close installed-build claims, obtain the path/provenance of an
   already-captured exact matching 57.10 `-1-16` image. Do not repeat device probes
   or substitute Fenway/34.16 evidence for absent target bytes.

No source/runtime test suite is applicable to unchanged firmware behavior.
Nothing was merged, deployed, restarted or written on the speaker. No claim of
full brief/goal completion is made while the audit above remains incomplete.
