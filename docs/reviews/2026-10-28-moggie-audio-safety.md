# L7 handoff — Moggie audio safety

Documentation-only lane, stopped at local branch and handoff. Custom amplifier
output remains barred by the [acceptance record](../hardware/AUDIO-SAFETY-INVARIANTS.md#acceptance-record):
no invariant has been proved end to end on hardware and H1–H7 remain open.

## Branch and scope

- Branch: `burn/moggie-audio-safety-20261028`.
- Base: default branch `main`, commit
  `3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db`. Verified with
  `git ls-remote --symref origin HEAD` before branching from `origin/main`.
- Commits before this handoff: `6a8f206` pins model-specific stock evidence;
  `490fc41` defines admission requirements; `bd86d9c` records thermal source drift
  and reproducible checks; `966c3d6` removes misleading older safety claims.
- No push, PR, merge, deploy, service restart or hardware operation was performed.
  No firmware/runtime implementation changed; inspect the branch diff to verify
  that all tracked changes are Markdown documentation.

## What changed and why

1. [AUDIO-SAFETY-INVARIANTS.md](../hardware/AUDIO-SAFETY-INVARIANTS.md) defines 15
   numbered ordering, timing, health, gain, recovery and process-failure invariants.
   Each has a current implementation location/gap and the evidence required to
   close it. Seven hardware gates prevent unknown profile values from becoming
   enabling defaults. Proposed policy is distinguished from observed stock code.
2. [Stock evidence ledger](../hardware/evidence/2026-10-28-stock-audio.md) identifies
   inputs by path, size and SHA-256 and gives exact binary offsets/source lines,
   offline extraction commands and an executable integrity-check block. Extracted
   components/disassembly stay in ignored `out/audio-safety/`; no vendor binary is
   added to the branch.
3. [AUDIO.md](../AUDIO.md) now points to the safety specification and corrects
   unsupported low/high-rail, half-power and automatic fault-clear conclusions;
   its graph-export section no longer promises that `/getDSP` will work.

## Evidence that changes the bring-up decision

The ledger defines `A`/`K` as the extracted userspace/module of local
`34.16-37101-1-16.upd`, and `G` as `gpl/drivers/audiodev-connectx`. This package is
older than the bench software and is not claimed to match it (ledger provenance).

| Finding | Exact evidence |
|---|---|
| ZPS5 enable/mute ordering is concrete; physical rail identity still needs measurement. | Ledger E04: `K:.text+0x8b78–0x8bc8` writes register mute then clears C20 and waits 20 ticks; `+0x8e04–0x8e30` sets C20, waits 33 ticks, then writes `0x13=0xe0`. These are captured tick counts, not measured settling times. |
| Generic power/mute success is not physical completion. | E05/E06: ZPS5 queues worker mute; current/requested state advances before physical writes. Generic amp-power MIO lacks a ZPS5 pin (`G/audioctl.c:79–98,1078–1116`). |
| Amp-off timeout has a recovered default, not a guessed one. | E07: `A:0x10065e74–0x10065e80` initializes 120; `0x10066234` stores the `AmpOffTimerSec` override; `0x10066358–0x100663c8` handles expiry and rechecks eligibility. |
| Lock recovery has unsafe behavior to avoid reproducing. | E03: `K:+0x89c0–0x8a20` has five lock polls per reset attempt but an unbounded reset loop and unchecked transaction results. Proposed invariant 4 adds a finite admission policy. |
| Thermal numbers cannot be copied across versions. | E08/E12: userspace logs 75% while setting internal 73 (`A:0x10056d9c–0x10056db8`); packaged module compares raw counter 44/49 (`K:+0x4db0–0x4dc8`), whereas `G/audioctl.c:303–312,884–887` converts to degrees and uses newer 75/80°C definitions. |
| Driver selection is a concrete review point. | `scripts/build-kernel.sh:24` selects `src/drivers/audiodev`; ZPS5 paths examined here are in `drivers/audiodev-connectx` (ledger E01–E05). No build-script change was made. |

## Verification executed

After the ledger extraction, the documented Python block was copied verbatim to
`out/audio-safety/check-evidence.py`. Recreate it and rerun with:

```sh
python3 - <<'PY'
from pathlib import Path
p = Path('docs/hardware/evidence/2026-10-28-stock-audio.md')
code = p.read_text().split('```python\n', 1)[1].split('\n```', 1)[0]
Path('out/audio-safety/check-evidence.py').write_text(code + '\n')
PY
python3 out/audio-safety/check-evidence.py
git diff --check 3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db
```

Observed result (exit 0):

```text
PASS: 6 input hashes, 5 source hashes, 26 PPC words, strings/records; 15 invariants + maps, 7 gates, 12 evidence rows, local links
```

`git diff --check` also exited 0 without output. The checker verifies selected
literal evidence plus document integrity; it is not a runtime, driver, acoustic or
hardware-safety test. The full disassembly was also inspected at the ledger's cited
addresses. No firmware build/test suite was run for these Markdown-only changes.

Artifact fingerprints:

```text
c756a1e7c813cf8938fe395a34e1947c0d565da6239934139765648a4ed60c9e  docs/hardware/AUDIO-SAFETY-INVARIANTS.md
09dd6a8b0a66ccb0eed51d715c8e2f83cadfae73774c6089ff58062b7afef6ec  docs/hardware/evidence/2026-10-28-stock-audio.md
3c22801ddfaa9f742347db076eec54f15c585982fa8074188196b4b6785043ee  docs/AUDIO.md
```

## Deliberately left out

- Implementing or activating the proposed interlock, changing driver selection,
  building/flashing firmware or producing custom audio. Scope is documentation.
- Hardware access, electrical/thermal tests, intentional faults, source playback
  and tuning. No physical timing, voltage, protection latency or acoustic result
  is claimed; H1–H7 define the missing evidence.
- Decrypting the newer `57.23-74170-1-16` rootfs, treating it as the installed
  57.10 build, or copying Fenway/Anvil delays and thermal constants into ZPS5.
  See ledger provenance, E09–E12.
- Secrets, Hermes configuration, other worktrees, project database changes and agent
  delegation. Analysis outputs were written only inside this worktree.

## Ranked follow-ups

1. **P0 — close board/electrical/timing identity (H1–H3).** In a separately
   authorized bench phase, passively record installed stock build/driver identity,
   chip/pin/rail behavior, lock/mute/reset timing and actual completion semantics.
2. **P0 — establish protection inputs and limits (H4–H6).** Recover the board's
   graph, channel calibration, total gain limits, thermal interpretation and
   fault coverage; define finite validated deadlines and an independent disable
   mechanism. Do not replace missing information with sibling constants.
3. **P1 — implement the entire proof map offline.** Review driver selection;
   implement one admission owner, applied-state acknowledgement, bounded reset,
   separate safety latch, full health monitoring and independent crash containment.
   Keep output disabled while profile evidence is incomplete.
4. **P1 — execute failure-oriented verification (invariants 1–15/H7).** Start with
   mocks/fault injection, then authorized muted electrical validation covering
   competing clients, stale reads, bus/worker loss, permanent faults and owner death.
5. **P2 — consider a controlled audio test only after all gates close.** Record
   implementation revision and retained proof for every invariant; this handoff
   does not satisfy or authorize that test.
