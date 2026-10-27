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

Uses the crosstool-NG `powerpc-buildroot-linux-gnu` toolchain (glibc 2.23,
`--enable-kernel=2.6.35`), located through `toolchain.lock`
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

Produces `out/vmlinux.bin`, `out/audiodev.ko` and `out/artifacts.sha256`.

The config comes from **`arch/powerpc/configs/fenway_op_defconfig`**, which Sonos ships
in the GPL drop. It is not recoverable from the shipped kernel image — `CONFIG_IKCONFIG`
is off, so there is no embedded config to extract.

The build script applies three changes to that defconfig:

| Option | Why |
|---|---|
| `CONFIG_CC_OPTIMIZE_FOR_SIZE=y` | the kernel partition is 2 MB; a `-O2` build does not fit |
| `CONFIG_SQUASHFS=y` | absent from the stock config; needed for the A/B rootfs |
| `CONFIG_BLK_DEV_INITRD=y` | absent entirely; needed for the boot chooser |

It also has to coax a 2011 kernel into building under GCC 15 — see the comments at the
top of `scripts/build-kernel.sh`. Those are not optional; remove one and the build fails.

Expected result on the current tree:

```
vmlinux.bin   4,204,896 B
gzip payload  1,703,280 B   (81% of the 2 MB kernel partition)
audiodev.ko     120,720 B
```

## 5. Root filesystem

```bash
make rootfs
```

Produces `out/rootfs.squashfs` (and `out/rootfs.nand.img`, padded to the
128 KiB NAND eraseblock). Must be ≤ 7,864,320 bytes (7.5 MiB) — that's half of
the 16 MB rootfs partition, and the whole A/B scheme depends on it.

The rootfs is built by Buildroot 2026.02.3 from the br2-external tree in
`buildroot/`, against the pinned Bootlin toolchain. `buildroot.lock` pins the
Buildroot tarball; `buildroot/MANIFEST.md` lists every package and its licence.

> **The image is built for the device's kernel, but has never run on it.** The
> toolchain's libc declares `Linux, ABI: 2.6.35`, every ELF in the image carries
> the same tag, and a scan for `li r0,<nr>; sc` pairs finds **no** syscall above
> the 2.6.35 PowerPC table. That removes the known incompatibility; it is not
> the same as a boot. See `buildroot/MANIFEST.md`.

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
