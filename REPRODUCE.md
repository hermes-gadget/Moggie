# Reproducing a build

From a clean machine to a verified image. Nothing here needs the speaker.

## 1. Host requirements

Linux x86-64 or arm64. Docker is optional but recommended for the rootfs build.

```bash
make check
```

## 2. Toolchain

```bash
make toolchain
```

Downloads Bootlin's `powerpc-e300c3--glibc--stable-2026.08-1`, verifies its SHA-256
against `toolchain.lock`, and extracts it to `toolchain/`.

The hash check is not advisory. If it fails, the build stops.

## 3. Sources

```bash
make sources
```

Fetches from Sonos's GPL drop:

- `drivers.txz` — includes `drivers/audiodev/`, the GPL audio driver
- `linux-2.6.35.txz` — the kernel

Both land in `dl/` and extract into `src/`.

## 4. Kernel

```bash
make kernel
```

Produces `out/vmlinux`, `out/audiodev.ko`, and appends their SHA-256s to
`out/artifacts.sha256`.

The stock kernel config is recovered from the shipped kernel image with
`extract-ikconfig`. Two options must be present for the flash layout to work:

- `CONFIG_MTD_CMDLINE_PARTS`
- `CONFIG_SQUASHFS`

## 5. Root filesystem

```bash
make rootfs
```

Produces `out/rootfs.squashfs`. Must be ≤ 7,864,320 bytes (7.5 MiB) — that's half of
the 16 MB rootfs partition, and the whole A/B scheme depends on it.

## 6. Verify

```bash
make ota-test          # negative tests for the update path
sha256sum -c out/artifacts.sha256
```

## Expected artifacts

| File | What it is |
|---|---|
| `out/vmlinux` | kernel |
| `out/audiodev.ko` | GPL audio driver module |
| `out/rootfs.squashfs` | root filesystem, ≤ 7.5 MB |
| `out/artifacts.sha256` | hashes of the above |

Flashing these to a speaker is a separate, hardware-gated step. See `ROADMAP.md`
Phase 7.
