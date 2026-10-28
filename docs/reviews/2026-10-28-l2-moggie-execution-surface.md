# L2 execution surface — continuation checkpoint

Stock analysis is in progress. The previous access gate is superseded by the
user's instruction to continue the named read-only queue. Host-only extraction
has parsed both pinned `-1-16` packages and recovered 14 allowlisted files from
34.16, with 7 explicitly absent paths. No credentials or target programs were
read/executed; firmware binaries remain in ignored `build/stock-execution/`.

First coherent increment: `tools/stock_execution_evidence.py`, generated
`docs/hardware/evidence/execution-20261028/`, and the report's current checkpoint.
Run the generator using the command in its forthcoming reproduction section;
actual result: `PASS: 2 pinned UPDs parsed to exact EOF; 14 allowlisted files extracted; 7 absent`.

Ordered remaining queue:
1. Trace each available captured updater's actual verification calls and flash
   path, keeping 34.16 `-1-16` and 57.10 Fenway distinct.
2. Inspect the older kernel and captured boot logs; reconcile the prior map,
   especially family attribution, writable hooks and unsupported rollback claims.
3. Replace the historical report with versioned findings, explicit CONFIDENCE
   and UNKNOWN-OFFLINE, route ranking and safest next experiment.
4. Regenerate evidence, verify bytes/citations, commit, push this branch and open
   a PR; update this handoff with exact proof hashes and remaining evidence gaps.

No hardware access, flash/bootloader writes, power cycles, service/config changes
or target execution are authorized. Missing exact installed-build evidence must
remain unknown; it is not a reason to repeat device probes.

---

## Initial handoff (historical)

# L2 Moggie execution surface handoff — 2026-10-28

**Verdict: BLOCKED; stock analysis incomplete.** The report is a verified partial
result, not a claim that the brief or active goal is complete.

## What changed and why

Added `docs/hardware/EXECUTION-SURFACE.md`, including explicit `CONFIDENCE` and
`UNKNOWN-OFFLINE` sections. It reconstructs the repository's custom init/update
flow with source line citations and identifies persistent writes that disqualify
that flow as a zero-write bootstrap. It separately records the unverified stock
questions, a provisional route-priority table, a host-only next experiment, and
the still-blocked prior-map comparison. The purpose is to prevent Moggie's
proposed A/B design from being reported as captured stock firmware behavior.

The relevant distinction is explicit in `docs/ARCHITECTURE.md:5–20`; actual custom
state writes are in `firmware/bootchooser/init:48–53,192–223,298–339` and custom
installation writes in `firmware/updater/updater.c:569–659`. The repository
signature chain is in `firmware/updater/manifest.c:360–401` and
`firmware/updater/rsa.c:131–183`; the checked-in public key is labeled a
placeholder at `firmware/updater/pubkey_dev.h:1–13`.

## Branch and evidence

- Worktree: `/home/ben/Moggie-wt/ota`.
- Branch: `burn/moggie-execution-surface-20261028`, created from `origin/main` at
  `3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db` after the original checkout was
  observed clean. `git ls-remote --symref origin HEAD` returned
  `ref: refs/heads/main HEAD` and that same commit. This was Git metadata only;
  no remote firmware or technical source was fetched.
- Report commit: `b75e217`, “docs(hardware): separate custom boot behavior from
  unverified stock routes”. This handoff is a separate reviewable commit.
- Evidence analyzed: repository source at the pinned baseline, with exact paths
  and line ranges in the report. The worktree image/log filename-size inventory
  and its search exclusions are recorded in the report's evidence inventory.
- The named external recon/capture directories were not inspected. No stock
  image provenance, binary offsets, signature chain or live device result is
  claimed.

## Requirement audit

| Brief requirement | Authoritative evidence obtained | Status |
|---|---|---|
| Stock init, services, watchdog, mounts and writable media | Custom source only; stock rootfs/captures outside permitted file-analysis scope | BLOCKED |
| Every stock verification point, primitive/key, slot layout, flags, `boot_ok`, rollback | Custom updater/chooser traced; stock binaries/captures not inspected | BLOCKED |
| Prove or mark unknown for NFS/TFTP/recovery/debug/maintenance boot | Every named family is explicitly UNKNOWN; no stock execution route is proved | Unknowns recorded; substantive investigation BLOCKED |
| Rank execution routes and name safest supervised experiment | Provisional evidence-work ranking; proposed host-only extraction of an existing capture | Partial; ranking needs stock evidence |
| Reconcile `BOOT-AND-MTD-MAP.md` and invalidate assumptions | Repository interpretation pitfalls cited; prior map not read | BLOCKED |
| Named branch from default | Local branch created from the default-branch commit above | Done |
| Report and handoff with explicit confidence/unknowns and hashes | Both requested paths created; checks and report hash below | Done for partial result |
| No device access, flash writes, bootloader changes or power cycling | Only host repository reads, documentation edits and Git operations performed | Observed throughout this analysis |

## Verification actually executed

`git diff --cached --check` returned exit 0 before the report commit. A Python
check resolved 37 explicit repository path/line-range citations within file
bounds, required both named sections, and required the blocked status. Result:

```text
PASS: 37 repository source citations resolve within file bounds; required sections present; stock status BLOCKED
```

That check verifies citation locations and required sections, not the stock
claims, semantic correctness of every citation, or completeness of the brief.
Relevant source passages were also read directly. No runtime/build/device tests
were run; documentation checks cannot validate stock boot or update behavior.
The new documentation paths require explicit `git add -f` under this checkout's
ignore rules; only the two requested documents are added.

Report integrity check executed:

```text
8d417d1fe3e9b85db746c4873b7dc5ae8e1171740c73b47c3753cdb724900ec8  docs/hardware/EXECUTION-SURFACE.md
```

## Deliberately left out

No live device access, packet/request probing, UART interaction, power cycling,
flash operations, bootloader/configuration changes, target executable execution,
new capture acquisition, service changes, firmware download or speculative
exploit. Existing hardware-day instructions were read as source material only;
their flash and recovery steps were not executed. No unrelated files, live
Hermes configuration, or private-key contents were modified/read for analysis.
No push or PR was performed; the brief does not require a PR for this lane.

## Exact blocker and ranked follow-ups

The brief at
`/home/ben/.review-notes/2026-10-28-sol-burn/L2-moggie-execution-surface.md:8–9`
names inputs outside the worktree, while its binding lines 30 and 33 permit
file analysis inside the worktree and require BLOCKED for a needed action
outside the envelope. The pending request is **read-only access to exactly**:

1. `/home/ben/.hermes/plans/2026-10-28-moggie-recon-output/`
2. `/home/ben/sonos-bench-analysis/`
3. `/home/ben/sonos-bench/logs/`

All existing prohibitions remain in force. An unanswered request is not consent.

Once that scope is explicitly resolved, the remaining work is, in order:

1. Inspect the five named recon reports; locate and hash the actual captured
   rootfs/kernel/log inputs. Establish capture provenance and completeness.
2. Extract the stock rootfs on the host without executing its programs. Trace
   PID 1, supervision, watchdog and actual mount/persistence evidence. This is
   the single proposed next supervised experiment, not an executed result.
3. Trace all stock updater verification call sites and authenticated byte
   ranges, extract the public trust anchor with offsets, and establish boot
   selection/fallback rules. Mark anything unsupported UNKNOWN-OFFLINE.
4. Reconcile the captured MTD map with every repository/prior-recon assumption;
   revise the execution-route ranking and determine whether any later device
   experiment can satisfy zero persistent writes.
5. Re-run documentation/citation checks, commit the resulting evidence, and
   report fresh SHA-256 hashes. Do not declare completion without stock evidence.

The deadline remains wind-down 06:15 UTC and hard stop 06:45 UTC on 2026-10-28
(brief line 32). Work stopped at the scope gate rather than waiting until that
deadline. This is the first blocked goal turn; the goal remains active pending
the required scope/evidence resolution.
