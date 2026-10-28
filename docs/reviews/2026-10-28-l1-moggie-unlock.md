# L1 Moggie unlock: offline handoff

2026-10-28. **BLOCKED for exact ZP5S behavior; sibling-build evidence delivered.** The report's scope and target-package inventory explain the blocker ([UNLOCK-CODE-PATH.md:3](../hardware/UNLOCK-CODE-PATH.md), [target-package.json:2–17](../hardware/evidence/unlock-20261028/target-package.json)). Required input is already-captured matching ZP5S userspace, including `anacapad`, its libraries, init/service scripts and extraction/model provenance. This is not a request for device access or credentials. No physical verification is authorized by this handoff.

## Changed and why

- Added [UNLOCK-CODE-PATH.md](../hardware/UNLOCK-CODE-PATH.md): exact handler addresses/file offsets, network predicates, signed-input construction, side effects, persistence/reversal limits, independent root authentication, entry-point matrix and negative-results register. Claims refer to source bytes or stock script lines.
- Added a [bounded evidence packet](../hardware/evidence/unlock-20261028/manifest.json) with source hashes, complete exact-route table, selected constants/relocations, five version inventories, package magic-search result and 50 annotated disassembly excerpts. These preserve enough evidence to review the conclusions without copying entire firmware images or credential files.
- Added [stock_unlock_evidence.py](../../tools/stock_unlock_evidence.py) to reproduce the packet and [check_stock_unlock_evidence.py](../../tools/check_stock_unlock_evidence.py) to check original byte provenance, VA/file mapping, source hashes, document offsets/links and deterministic regeneration. The checker explicitly does not certify RE semantics or hardware behavior.

The main corrections are the TLS-context route predicate versus the mistaken directory-mode `0x4000` theory (report N1–N5), a zero reboot outflag on the normal success path (U5), a fixed script with output descriptors rather than submitted code as an argument (U4), conditional DevMode persistence and its default `/jffs/debug/devmode.bin` path (D3), and TestEnv's signed bit 7 plus resolved `syssettings.xml` transaction (T1–T3). Each section contains the binary offsets that support that correction. The root-authentication section also distinguishes password-method behavior from a separate conditional `none` path (R1).

## Branch and commits

The local `main` and cached `origin/main` were both `3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db`; no symbolic `origin/HEAD` existed. Branch `burn/moggie-unlock-20261028` was cut from that local default-branch baseline, without fetching. Repository evidence is reproducible with:

```sh
git merge-base burn/moggie-unlock-20261028 main
git log --oneline main..burn/moggie-unlock-20261028
git status --short --branch
```

First commit: `0d0a2e5` (`docs(unlock): preserve binary evidence to correct misleading recon paths`). Commit `3775ffd` retains the completed handler/policy/storage report and generated evidence. The final commit adds the general peer-filter limit, byte checker, verification receipt and this handoff. Nothing was pushed or published. The repository's shared exclude rules ignore `docs/hardware/`, so only the requested new evidence paths were explicitly force-added; shared excludes were not edited.

## Verification executed

Run from `/home/ben/Moggie-wt/sonosd`:

```sh
python3 tools/stock_unlock_evidence.py \
  --rootfs /home/ben/sonos-bench/sonos-fenway/firmware/rootfs/57.10-25140 \
  --objdump /home/ben/sonos-bench/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin/powerpc-buildroot-linux-gnu-objdump \
  --target-package /home/ben/sonos-bench-analysis/zp5s/57.23-74170-1-16.upd \
  --out docs/hardware/evidence/unlock-20261028

python3 tools/stock_unlock_evidence.py \
  --rootfs /home/ben/sonos-bench/sonos-fenway/firmware/rootfs/57.10-25140 \
  --objdump /home/ben/sonos-bench/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin/powerpc-buildroot-linux-gnu-objdump \
  --target-package /home/ben/sonos-bench-analysis/zp5s/57.23-74170-1-16.upd \
  --out build/unlock-analysis/reproduced

python3 tools/check_stock_unlock_evidence.py \
  --evidence docs/hardware/evidence/unlock-20261028 \
  --compare build/unlock-analysis/reproduced \
  --document docs/hardware/UNLOCK-CODE-PATH.md

git diff --check
git diff --cached --check
```

Result lines are retained in [validation.txt](../hardware/evidence/unlock-validation-20261028.txt). The byte audit is an evidence-integrity check; semantic conclusions were manually traced through call sites and the saved excerpts. No repository runtime tests were needed for these documentation/host-analysis additions, and no speaker program was executed.

## Deliberately left out

No device/network-to-speaker access, credential submission/cracking, flash/GPIO action, target execution/emulation, service restart, deployment or live configuration modification was performed. No account database, runtime key material, private-key recovery, valid unlock response, arbitrary-program upload procedure or firmware-install procedure is delivered. Prior recon's stronger model/key/reboot assertions were not silently promoted into new facts; uncertainties are explicit in the report.

No complete ZP5S userspace was extracted in this session. The UPD signature scan does not prove encryption or impossibility of offline extraction. The full TLS trust/client-certificate policy, manufacturing envelope's nested verifier/HAL physical commit semantics, DevMode callback-selected trust chain, SSH public-key authorization chain, telnet login implementation and all downstream configuration consumers are unclosed. They are not represented as tested or absent.

TokenSave context lookup was attempted before direct code analysis and did not return; `.tokensave/tokensave.db` is absent in this worktree. Existing host PPC objdump plus pyelftools provided the bounded fallback. The old SuperH capture was inventoried with its actual ELF endianness and was not disassembled as PPC.

## Ranked follow-ups

1. **Close model applicability first:** provide already-captured matching ZP5S userspace and provenance, or a documented format-aware offline extraction path for already-captured target packages. Repeat the report's entry, policy, crypto and storage traces against those exact hashes. No conclusion here authorizes a live test.
2. **Complete static trust-chain provenance:** trace initialization of the normal unlock key buffer, DevMode verifier callbacks, and manufacturing `ZZ0160`; classify source/format without disclosing key or credential material. Relevant starting offsets are report U3, D2 and M1.
3. **Complete secondary effects offline:** follow TestEnv's property consumers, DevMode callbacks, root option initialization and public-key/telnet policy. Preserve the difference between property indices and signed bitmap indices (T1/R1).
4. **Only after matching evidence, propose a separately authorized verification:** an operation-specific plan with expected observations, persistence boundaries and reversal prerequisites. Any action requiring the physical device is outside this lane and must stop at that separate authorization boundary (brief lines 29–40).

The proof hashes accompanying delivery establish the exact written artifacts, not success at unlocking a ZP5S.
