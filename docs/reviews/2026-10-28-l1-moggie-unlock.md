# L1 Moggie unlock: offline handoff

2026-10-28. **Versioned offline traces delivered; exact installed ZP5S 57.10 applicability remains BLOCKED.** [UNLOCK-CODE-PATH.md](../hardware/UNLOCK-CODE-PATH.md) now covers the captured 34.16 `-1-16` userspace separately from 57.10 Fenway. The older package was successfully extracted using host tools; its model-family association is the explicit capture-based hypothesis in the prior recon, not a hardware observation (report Z0). This materially narrows the earlier artifact gap, but these other builds do not prove the installed 57.10 `-1-16` implementation.

The input needed to close that remaining gap is **already-captured matching ZP5S 57.10 `-1-16` userspace plus extraction/model provenance**. An expanded inventory has now located an already-captured plaintext 57.23 component; W0–W2 preserve a bounded comparison and its incomplete archive provenance. It remains a different version. No device access or credentials are requested or authorized by this handoff.

## Changes and evidence

- Added the report's Z0–Z8 trace: UPD record provenance, exact 24-byte route table, SSL-listener predicate, wildcard IPv4 bind, challenge/hash/signature path, fixed inetd command, temporary unlock state, DevMode persistence/delete/startup, TestEnv's resolved XML transaction, and independent BusyBox login policy. Every section gives binary file offsets or numbered stock-file references.
- Resolved normal-unlock key initialization independently in Z3a/U3a and the default DevMode key lookup in D2a. The latter uses a compiled-in library input and a public-key identifier check, so it must not be conflated with the normal runtime manufacturing slot. No key bytes are included. M2/M3 also resolve the nested manufacturing input/callbacks and the HAL's separate write/failure boundaries; neither write success nor HTTP status is proof of safe rollback.
- Retained the independent Fenway N/U/M/D/T/R trace and added its telnet login implementation alongside Dropbear authentication. This keeps differences in startup, manufacturing mode, route flags, CSRF parsing and property indices visible instead of transferring them between builds.
- Preserved the original two generated evidence packets: [Fenway manifest](../hardware/evidence/unlock-20261028/manifest.json) and [34.16 manifest](../hardware/evidence/zp5s-34-20261028/manifest.json). They contain hashes, mappings, routes, constants, selected relocations, annotated instructions and numbered non-credential text. Firmware binaries remain outside git in captured storage or ignored build output.
- Added a third [57.23 packet](../hardware/evidence/zp5s-5723-20261028/manifest.json) and [bounded generator](../../tools/zp5s_5723_evidence.py). Eight plaintext re-extractions match prior captured files; 83 route rows and command/execve traces are preserved. W0–W2 qualify the archive association and establish that this captured run_sshd.sh executes telnetd.
- Added [zp5s_unlock_evidence.py](../../tools/zp5s_unlock_evidence.py), retained [stock_unlock_evidence.py](../../tools/stock_unlock_evidence.py), and generalized [check_stock_unlock_evidence.py](../../tools/check_stock_unlock_evidence.py) for all three packets. The checker validates source hashes, instruction bytes, VA/file mappings, relocation entries/symbols, route words/paths, constants, SSH option-table jumps, document offsets/links and deterministic regeneration. The isolated [negative-check driver](../../tools/test_stock_unlock_evidence.py) rejects tampered metadata in worktree scratch copies. It does not certify RE semantics or behavior on hardware.

The consequential findings, with their evidence locations, are:

| Finding | Report evidence |
|---|---|
| 34.16 `-1-16` is extractable offline; a blanket encrypted/unextractable claim was wrong | Z0; package record `0xadd3f`, payload `0xadd4f`; host filesystem inspection |
| Captured 57.23 run_sshd.sh executes telnetd, whose WB binary differs from Z34 BusyBox | W0–W2; script line 4, filesystem links and WB manifest |
| TLS setup tries a registered credential object before compiled-input fallback | Z2c; startup file `0x006e58`, callback assignment file `0x2394a8` |
| Dropbear signature names select three ECDSA curves, separately from HTTP RSA approval | R5; name parser `0x11de4`, ELF relocations and exact name strings |
| Normal approval attempts inetd/telnet in Z34 versus run_sshd.sh in A57 | Z4 / U4; Z file `0x262194`, A file `0x2a02b0`; corresponding stock scripts |
| Located unlock flags test SSL/TLS, and reviewed listeners use wildcard binds | Z2 / N1–N2; shared-peer and Host qualifications remain explicit |
| Prior `0x4000` loopback proof was a stat-directory check | N4; A file `0x427f6c` |
| Ordinary normal-unlock success does not force a reboot in either reviewed implementation | Z4 / U5; full worker/wrapper dataflow |
| Z34 manufacturing/development aliases exit the nonzero parsed POST branch with 500; A57 manufacturing mode writes MDP | Z5 / M1; Z file `0x261cf8`, A file `0x2a03dc` |
| DevMode stores validated binary statements; signed bit 0 controls persistence at default `/jffs/debug/devmode.bin` | Z6 / D2–D3; each build's own verifier/writer/root mapping |
| TestEnv uses signed bit 7, with property index 49 in Z34 and 40 in A57; default persistence is `/jffs/settings/syssettings.xml` | Z7 / T1–T3; resolved virtual calls and transactions |
| HTTP approval is distinct from root login; empty-field and preauthentication branches prevent a universal “password always required” claim | Z8 / R1–R2; no actual account data inspected |

## Branch and commits

The work is on `burn/moggie-unlock-20261028`, cut from local `main` at `3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db`; cached `origin/main` matched. There was no symbolic `origin/HEAD`, and no fetch was performed. Recheck with:

```sh
git merge-base burn/moggie-unlock-20261028 main
git log --oneline main..burn/moggie-unlock-20261028
git status --short --branch
```

The commits explain why each evidence/report addition was needed. Initial evidence is in `0d0a2e5`, the Fenway trace in `3775ffd`, its initial byte audit in `055b46f`, and the recovered `-1-16` packet in `0de5e2d`; subsequent commits retain the version-specific conclusions and final verification. The continuation pushed this branch to `origin` and opened [PR #62](https://github.com/hermes-gadget/Moggie/pull/62), related to [issue #61](https://github.com/hermes-gadget/Moggie/issues/61). The branch has coherent network, key-source, DevMode trust, manufacturing and SSH follow-up increments. Remote-head verification and updated PR description are recorded at final delivery; all coherent increments are pushed immediately. Shared git exclude rules ignore `docs/hardware/`, so only the requested artifact paths were explicitly force-added; the shared excludes were not changed.

## Verification executed

From `/home/ben/Moggie-wt/sonosd`, all three generators were run into their committed evidence directories and independently regenerated into ignored build output. Exact source/objdump commands are in the report and [34.16 reproduction note](../hardware/evidence/ZP5S-34-20261028.md). Each disassembly excerpt also records its exact command.

The final integrity checks were:

```sh
python3 tools/check_stock_unlock_evidence.py \
  --evidence docs/hardware/evidence/unlock-20261028 \
  --compare build/unlock-analysis/reproduced \
  --document docs/hardware/UNLOCK-CODE-PATH.md
python3 tools/check_stock_unlock_evidence.py \
  --evidence docs/hardware/evidence/zp5s-34-20261028 \
  --compare build/unlock-analysis/zp5s-34-reproduced \
  --document docs/hardware/UNLOCK-CODE-PATH.md
python3 tools/check_stock_unlock_evidence.py \
  --evidence docs/hardware/evidence/zp5s-5723-20261028 \
  --compare build/unlock-analysis/zp5s-5723-reproduced \
  --document docs/hardware/UNLOCK-CODE-PATH.md
git diff --check
git diff --cached --check
```

Actual result lines, counts and report SHA-256 are retained in [validation receipt](../hardware/evidence/unlock-validation-20261028.txt). These are evidence-integrity checks. Semantic conclusions were manually traced through the preserved control flow. No speaker program or target emulation was run, and repository runtime tests were not applicable to these documentation/host-analysis additions.

## Deliberate exclusions and limits

No device or network-to-speaker access, credential submission/cracking, flash/GPIO action, target program/script execution, service restart, deployment or live configuration modification occurred. Captures were opened read-only; extraction and generated files were written only inside this worktree. No account database or runtime key bytes were inspected. No valid unlock response, private-key recovery, arbitrary-program upload recipe or persistent firmware-install procedure is delivered.

No local decryption of the 57.23 flags-4 envelope was performed. An expanded scan located an existing plaintext rootfs component and extracted userspace under the prior zero-pound review notes. W0 records component and package hashes, the archive-run association claim, the absent per-package receipt, and eight byte-identical re-extractions; it does not claim cryptographic package binding. A magic-search miss, high entropy or a filename is not a proof that device access is required for extraction. Matching installed 57.10 userspace is still absent from the reviewed evidence.

Open static questions include Z34's complete shared peer/Host policy and TLS credential helper/envelope semantics; DevMode's full envelope-format semantics and dynamic interposition; A57 manufacturing's full envelope-parser correctness, kernel/physical commit semantics and rollback; Dropbear's individual signature primitives, key-option semantics and filesystem permission policy; and downstream consumers of changed debug/settings properties. Runtime account/TTY contents, process identity, service health, actual persistence and physical rollback were deliberately not exercised. Each scope limit is kept beside its corresponding report claim.

TokenSave context lookup was attempted before direct code analysis and supplied no usable graph result; `.tokensave/tokensave.db` is absent in this worktree. Existing host PPC objdump, pyelftools and unsquashfs supplied the bounded fallback. The SuperH capture was identified using its real ELF machine/endianness and was not disassembled as PPC (report version inventory).

## Completion audit against the brief

The final audit used the current committed branch, source manifests, generated excerpts, report and fresh [bounded capture inventory](../hardware/evidence/unlock-capture-inventory-20261028.txt). This audit preserves the full installed-target objective; the recovered older version remains version-specific evidence.

| Requirement | Authoritative evidence | Audit result |
|---|---|---|
| Actual handlers, binary/function/file offsets and exact commands | Report Z1 and handler-discovery section; original registration/dispatch excerpts and all three route tables; W1 is static-table evidence only | Proven for the named captured hashes; installed 57.10 `-1-16` mapping missing |
| Versions, socket/interface preconditions, signed bytes and root checks | Report Z0/Z2/Z3/Z8, applicability table, N1–N5, Z2a–Z2c, W0–W2, U2/U3a, D2a and R1–R5 | Hash/input/login traces proven within their stated bounds; target model/version binding and Z34's complete shared network policy remain unproven |
| Side effects, persistence and reversibility | Report Z4–Z7, U4–U5, M1–M3, D3 and T3; numbered stock scripts and resolved writers | Static effects/reversal limits documented for captured builds; actual installed-target behavior unverified |
| Execute versus persistent firmware install | Report execute-versus-install section and complete normal-mode workers/scripts | Bounded source distinction proven; no installation or device operation performed |
| Eight-column entry-point matrix | Report entry-point matrix | Present with all requested columns and build-specific applicability/confidence |
| Negative-results register | Report negative-results register; original and expanded capture inventory | Present; records model/version, format, network, reboot, startup, storage and tooling dead ends |
| Required branch from default main and reviewable WHY commits | `git merge-base`, current branch/status and `git log main..HEAD` | Correct baseline/branch and committed artifacts; continuation also pushes the branch and opens PR #62 |
| Report, ranked handoff, factual citations, proof SHA-256 | Current files, validation receipt, byte/offset/link checks and manual control-flow review | Deliverables present and checked; integrity checks do not prove the missing target bytes |
| Offline boundaries | Executed host-tool commands recorded in generators/excerpts; task envelope | No target execution, hardware/contact, credentials, flashing, GPIO, deployment or live-config write |

**Completion is not established.** Matching installed-build evidence is missing from the recorded inventory. Further analysis of 34.16, 57.23 or Fenway can refine those versions but cannot establish the absent 57.10 `-1-16` instructions. The required next input is the path and provenance of an already-captured matching userspace/package; a physical-device step remains outside this task.

## Ranked follow-ups

The installed 57.10 bytes remain unavailable in the recorded captures. The new 57.23 comparison (W0–W2) preserves version identity, 83 static route rows, command/execve flow and eight byte-identical re-extractions. Its run_sshd.sh executes telnetd; its BusyBox hash differs from Z34. Follow W's own registration/network/crypto/storage/login code and resolve the missing individual archive receipt from existing captures if available. Neither Fenway SSH effects nor Z34 login instructions transfer to W.

Z2a/Z2b trace accepted addresses and request/header parsing without finding a Host-to-local-address comparison in those preserved parsers. Z2c resolves TLS callback installation and registered-versus-compiled-input selection. Remaining scope includes other application/interface policies, credential helper/envelope semantics, callback output and runtime branch selection. Do not relabel the request-target syntax parser as Fenway's Host authorization helper.

D2a/D2b resolve default DevMode callback sources and the header/ID/bitmap prefix passed into SHA-256; malformed/multiple envelope records remain open. Z3a/U3a trace normal-unlock initialization to a 708-byte manufacturing object or size-checked 836-byte fallback file. M2/M3 trace manufacturing input/callback forwarding and separate HAL failure boundaries. R3–R5 trace SSH defaults, matching, the signed-request decision and the ECDSA name/index gate. Credential contents, mathematical verifier completeness, physical durability and rollback remain outside the established results.

1. **Close installed-build applicability:** supply already-captured matching ZP5S 57.10 `-1-16` userspace/provenance. Repeat handler, policy, crypto and storage traces against those exact hashes. Preserve the distinction between the extracted older family image, Fenway's same-number version, and actual installed code.
2. **Finish shared network policy and trust provenance offline:** compare the newly available W paths against their own bytes; finish remaining Z34 application/interface policy, TLS credential helper/envelope validation and full DevMode envelope-format validation. Z2c resolves the installed default callback addresses, registered-file path and compiled-input fallback without copying credential bytes. Z2b closes only the preserved Host-parser dataflow. D2a closes the default key-selection callback lookup in both builds without copying embedded key bytes. Normal key-slot source paths are traced in Z3a/U3a; do not inspect key contents. Classify source/format without reading account records or disclosing key material.
3. **Finish indirect/persistent effects:** M2/M3 now trace A57 `ZZ0160` signed input/callback forwarding and the complete userspace HAL write sequence. Remaining: full envelope-parser validation, kernel/physical durability, DevMode/TestEnv consumers and remaining Dropbear signature primitives/key-option/filesystem policy. R3–R5 now establish the captured CLI defaults, fixed authorized-key path, exact key matching, separate signed-request decision, and three-name ECDSA index/key-slot gate. The mathematical verifier remains open; do not import the HTTP RSA approval algorithm into SSH login. Do not represent a configuration write as successful firmware installation or service health.
4. **Physical work requires a separate task:** any future operation requiring a speaker, credentials, flashing or GPIO lies outside this lane. Stop at that boundary; this handoff authorizes no such operation (brief's task envelope/hard limits).

The proof hash identifies the delivered report; it does not claim that a speaker was unlocked or that the installed-build gap is closed.
