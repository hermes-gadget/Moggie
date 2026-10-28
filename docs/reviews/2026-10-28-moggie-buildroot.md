# L8 Moggie buildroot/toolchain audit handoff

Branch: `burn/moggie-buildroot-20261028`, from default `main` at `3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db`. This is a local branch-and-handoff result; no push or PR was made. [Branch ancestry and name](../build/evidence/rootfs-fidelity/validation.txt#L5), [remote default witness](../build/evidence/rootfs-fidelity/config-review.txt#L1).

## What changed

- Added [ROOTFS-FIDELITY-AUDIT.md](../build/ROOTFS-FIDELITY-AUDIT.md#L1), covering init, libraries, accounts, mounts, modes/owners, symlinks, devices, modules, update container/signatures, size limits, and ABI. The ranked gap table includes a proving command for each finding. [Layout review](../build/ROOTFS-FIDELITY-AUDIT.md#L21), [ABI review](../build/ROOTFS-FIDELITY-AUDIT.md#L34), [ranked gaps](../build/ROOTFS-FIDELITY-AUDIT.md#L49).
- Preserved `file`, `readelf`, `objdump`, `unsquashfs`, record offsets, image hashes, complete manifests and complete layout diffs in [the evidence directory](../build/evidence/rootfs-fidelity/containers.txt#L1). Added a capture script and a read-only verifier. [Capture](../build/evidence/rootfs-fidelity/collect.py#L1), [verifier](../build/evidence/rootfs-fidelity/verify.py#L1).
- Allowed `docs/build/` through the existing generic `build/` ignore rule so the requested audit location is visible to Git. [.gitignore](../../.gitignore#L3). No firmware implementation or build configuration was changed.

Committed increments before this handoff:

1. `9f66ca1` — preserve original image/layout/ABI evidence so the compatibility review can be reproduced.
2. `c53504d` — distinguish the stock 2.4 boot chain from Fenway and the newer tc26 artifact.
3. `f965b9c` — rank acceptance gaps and verify the kernel format gate rather than treating image size as boot fidelity.

## Findings that control the next step

The inspectable stock `34.16-…-1-16` package has Linux **2.4.22**, SquashFS **3.0 big-endian / 64 KiB**, and glibc **2.3.5**. Its kernel explicitly compares the filesystem major against **3** at inflated offset **0x657e4** and takes the mismatch path at **0x657e8**. Both Moggie images are SquashFS **4.0**, so they fail that reader's format gate. Their libc floors are **3.2.0** and **2.6.35**, both newer than that packaged stock kernel. [Kernel bytes/disassembly](../build/evidence/rootfs-fidelity/stock-kernel.txt#L1), [image formats](../build/evidence/rootfs-fidelity/containers.txt#L29), [original libc](../build/evidence/rootfs-fidelity/abi.txt#L291), [tc26 libc](../build/evidence/rootfs-fidelity/abi.txt#L466).

The separate `57.10` dump is **Fenway/MPC8315**, cramfs, Linux **2.6.35**, glibc **2.23**. The bench banner says **MPC8272/ZP5S**. The `tc26` work addresses the Fenway libc floor; it does not prove a board-correct kernel for that bench unit. Stock math code uses FPU argument registers, so soft-float must not be assumed. [Dump identity](../build/evidence/rootfs-fidelity/stock-kernel.txt#L6), [bench/rebuild contradiction](../build/evidence/rootfs-fidelity/config-review.txt#L480), [FPU disassembly](../build/evidence/rootfs-fidelity/abi.txt#L861).

The rootfs artifacts are bare filesystems, without the stock update record stream/signature. Their fit is proven only against the configured cap, not a live slot. The image inventories also expose missing audio/Wi-Fi modules and custom cutover executables, stale static MTD minors, and state/provenance gaps. No ownership or missing default-library rejection was found by the bounded static checks. [Acceptance gaps](../build/ROOTFS-FIDELITY-AUDIT.md#L49), [declared-cap qualification](../build/ROOTFS-FIDELITY-AUDIT.md#L66), [ELF closure results](../build/evidence/rootfs-fidelity/elf-closure.txt#L36).

## Verification performed

`python3 docs/build/evidence/rootfs-fidelity/collect.py` completed with:

```text
PASS: captured 3 image manifests, 2 full layout diffs, container offsets, boot content and ABI evidence; no builds
```

`python3 docs/build/evidence/rootfs-fidelity/verify.py` completed with:

```text
PASS: 24 input hashes, 3 manifests, 2 exhaustive diffs, kernel version gate, ELF closure and 77 citation targets; no builds
```

The verifier rehashes actual input files, checks full manifest/diff coverage, checks the kernel version-gate bytes, and validates citation targets. This is analysis validation, not firmware execution. [Verifier](../build/evidence/rootfs-fidelity/verify.py#L1), [recorded result and Git checks](../build/evidence/rootfs-fidelity/validation.txt#L1). `sha256sum -c docs/build/evidence/rootfs-fidelity/SHA256SUMS` verifies the delivered audit, handoff, and evidence files.

## Deliberately left out

No builds, firmware tests, target binary execution, flashing/hardware interaction, service restarts, runtime configuration or deployment changes, signature bypass, decryption, or implementation fixes. `/home/ben/Moggie/toolchain`, `/home/ben/Moggie/out`, and the stock inputs were read only; small streamed image extracts live under this worktree's ignored `out/fidelity-audit/`. The owner-controlled Hermes configuration was not touched. [Capture operations](../build/evidence/rootfs-fidelity/collect.py#L1), [audit limits](../build/ROOTFS-FIDELITY-AUDIT.md#L70).

The installed ZP5S firmware, live MTD/OOB geometry, bootloader validation rules, and trusted installation path remain unresolved. The encrypted newer `1-16` package's plaintext layout was not claimed. These are explicit evidence gaps, not a claim that every possible stock boot path has been reverse-engineered. [Scope qualification](../build/ROOTFS-FIDELITY-AUDIT.md#L19), [ranked unknowns](../build/ROOTFS-FIDELITY-AUDIT.md#L49).

## Ranked follow-ups

1. Establish the actual board, installed kernel/userspace, and live NAND/slot geometry from separately authorized read-only evidence. Keep `1-16` and Fenway witnesses separate. This resolves P0.1/P0.5. [Evidence](../build/ROOTFS-FIDELITY-AUDIT.md#L49).
2. Choose a matching board/kernel/filesystem/libc baseline. A 2.6.35 libc floor alone does not make the observed 2.4.22 stock chain usable. Resolve the trusted stock-container/bootstrap path and metadata writer before proposing installation. This resolves P0.2–P0.4. [Evidence](../build/ROOTFS-FIDELITY-AUDIT.md#L49).
3. Package and verify a matching kernel/module set and the intended initramfs/chooser/updater cutover; bind persistence and device nodes to the measured map. This resolves P1.1–P1.3. [Evidence](../build/ROOTFS-FIDELITY-AUDIT.md#L49).
4. Repair read-only-root SSH/state assumptions, make toolchain-floor checks fail on incompatibility, and emit truthful toolchain/revision provenance. Rebuild only in a separately authorized run with disk capacity. This addresses P2.1/P2.2. [Evidence](../build/ROOTFS-FIDELITY-AUDIT.md#L49).

PROOF: `docs/build/ROOTFS-FIDELITY-AUDIT.md` SHA-256 `176815e91fac2283484c10e47703af39e848628154773c62593dd5ab9c23d8b5`; exact `file`/`readelf` evidence at `docs/build/evidence/rootfs-fidelity/abi.txt` SHA-256 `3703aee584923c092e1bb3fba02b40973faace5fa386fe597cc063e21d023cd6`; verifier command/result recorded above.
