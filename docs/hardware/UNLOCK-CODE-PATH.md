# Stock maintenance code paths — offline evidence

Investigation date: 2026-10-28. Status: **analysis in progress**.

## Scope and evidence notation

All firmware observations below concern the captured **57.10-25140 Fenway**
userspace, not a verified Play:5 Gen 1 (ZP5S) userspace. The image identifies its
architecture as `fenway` in `etc/inittab:1`; the prior recon explicitly distinguishes
the `-1-8` packages from the target `-1-16` family
(`/home/ben/.hermes/plans/2026-10-28-moggie-recon-output/MODEL-IDENTITY-CAVEAT.md:11–40`).

`A` means
`/home/ben/sonos-bench/sonos-fenway/firmware/rootfs/57.10-25140/opt/bin/anacapad`.
Its SHA-256 is `2a0e6adb831d995a2863f4307077e2c14c59afc52a43815865f0bc09f8580962`.
All other source identities and ELF load mappings are recorded in
[the manifest](evidence/unlock-20261028/manifest.json).
`VA` denotes an ELF virtual address; `file` denotes a byte offset in that exact
file. For `A` text, `file = VA - 0x10000000`; for its second file-backed load
segment, `file = VA - 0x10010000`. BSS addresses have **no file offset**
(`A` ELF program headers, file `0x34`, recorded in the manifest).

## Verified corrections to prior recon

1. **Normal unlock success does not take the reboot branch in this binary.**
   The responder initializes its stack byte at `r1+2056` to zero
   (`A` VA `0x102a051c`, `0x102a054c`; file `0x2a051c`, `0x2a054c`).
   It passes that byte's address to the parser (`A` VA `0x102a0624–0x102a0630`,
   file `0x2a0624–0x2a0630`). The complete parser only writes zero through that
   pointer (`A` VA `0x102a0448–0x102a0514`, file `0x2a0448–0x2a0514`).
   Thus the nonzero test at VA `0x102a0634–0x102a0640`
   (file `0x2a0634–0x2a0640`) does not select VA `0x102a06d4`, which contains
   the reboot call. The ordinary 200 response returns at VA `0x102a06d0`
   (file `0x2a06d0`). This is a static, normal-control-flow conclusion;
   no target executable was run. See [complete handler disassembly](evidence/unlock-20261028/unlock.dis.txt).

2. **The previously named `0x4000` “local-only” gate is a directory-mode check.**
   The helper at `A` VA `0x1041e3e8` (file `0x41e3e8`) calls `__xstat` at
   VA `0x1041e414` (file `0x41e414`). Its output goes to `r1+4104` in the
   static-file handler; the test reads that output's mode field at `r1+4120`
   (`A` VA `0x10427f40–0x10427f74`, file `0x427f40–0x427f74`).
   This is not evidence about a socket peer. The brief's local-only premise
   remains a prior observation; its actual socket predicate is still under analysis.

## Reproduce the evidence

Run from this worktree; inputs are opened for reading and output stays here:

```sh
python3 tools/stock_unlock_evidence.py \
  --rootfs /home/ben/sonos-bench/sonos-fenway/firmware/rootfs/57.10-25140 \
  --objdump /home/ben/sonos-bench/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin/powerpc-buildroot-linux-gnu-objdump \
  --out docs/hardware/evidence/unlock-20261028
```

This executes host analysis tools only. Device contact, credentials, flashing,
GPIO, and target emulation are outside this investigation's authorized scope
(`/home/ben/.review-notes/2026-10-28-sol-burn/L1-moggie-unlock.md:44–57`).
