# Root filesystem dependency manifest

Everything in the Moggie squashfs, why it is there, and what it costs. Built
by `scripts/build-rootfs.sh`; sizes and hashes in `out/rootfs.sizes.txt`.

- Image format: squashfs 4.0, **gzip** compression, 128 KiB blocks, no xattrs
  (the 2.6.35 kernel has `CONFIG_SQUASHFS` with zlib only — no XZ/LZMA
  decoder — and `CONFIG_SQUASHFS_XATTRS` off).
- Target: `powerpc-buildroot-linux-gnu`, big endian, e300c3, hard float,
  glibc 2.44.

## Build inputs

| Input | Version | Pinned by | Verified |
|---|---|---|---|
| Buildroot | 2026.02.3 | `buildroot.lock` (`BUILDROOT_SHA256`) | SHA-256 checked before every build |
| Bootlin toolchain | `powerpc-e300c3--glibc--stable-2026.08-1` | `toolchain.lock` (`TOOLCHAIN_SHA256`) | SHA-256 checked by `scripts/fetch-toolchain.sh` |
| gcc | 15.3.0 | toolchain | `-dumpversion` asserted |
| binutils | 2.45.1 | toolchain | — |
| glibc | 2.44 | toolchain | — |
| SOURCE_DATE_EPOCH | 1700000000 | `buildroot.lock` | exported to the build |

No internal toolchain is built: the defconfig uses
`BR2_TOOLCHAIN_EXTERNAL_CUSTOM` against the pre-installed Bootlin tree.

## Target packages

| Package | Version | License | Why it is present |
|---|---|---|---|
| busybox | 1.37.0 | GPL-2.0; bzip2-1.0.4 (bunzip2) | PID 1 (`init`), `ash`, `mdev`, `mount`, `insmod`, `ip`/`ifconfig`/`route`/`udhcpc`, `wget`, `nc`, `dd`, `flash_eraseall`, `getty`, plus the usual coreutils applets |
| dropbear | 2026.91 | MIT, BSD-2-Clause, public domain | sshd and `dbclient`/`scp`. Does not listen unless enabled through `/jffs/ssh`; always `-s` (no password auth) |
| mtd-utils | 2.2.1 | GPL-2.0 | `flash_erase`, `nandwrite`, `nanddump`, `flashcp`, `flash_lock`, `flash_unlock`, `mkfs.jffs2`, `sumtool`, `mtdinfo` — the OTA path needs these |
| mosquitto (library only) | 2.0.22 | EPL-2.0 or EDLv1.0 | `libmosquitto.so.1` for sonosd's MQTT backend. The broker is **not** built (`BR2_PACKAGE_MOSQUITTO_BROKER` off) |
| sonosd | 0.1.0 (this repo) | GPL-2.0 | The audio daemon: `/dev/dsp`, `/dev/audioctl`, arbiter, MQTT |
| libxcrypt | 4.5.2 | LGPL-2.1 | `crypt()` for shadow/dropbear; Buildroot pulls it because glibc no longer ships libcrypt |
| skeleton, skeleton-init-sysv, initscripts, ifupdown-scripts, urandom-scripts | (Buildroot 2026.02.3) | GPL-2.0 / GPL-2.0-or-later | Base `/etc` skeleton and init plumbing. The package `S*` init scripts are removed in post-build; rcS is the only entry point |

### Toolchain runtime that ends up on the target

| Library | Version | License | Why |
|---|---|---|---|
| `libc.so.6`, `ld.so.1`, `libm`, `libresolv`, `libcrypt` | glibc 2.44 | LGPL-2.1+ | C library |
| `libstdc++.so.6.0.34` | gcc 15.3.0 | GPL-3.0-with-GCC-exception | **Nothing links it.** Buildroot's external-toolchain check requires C++ to be enabled whenever the toolchain has a C++ compiler; `BR2_TOOLCHAIN_EXTERNAL_CXX` therefore copies libstdc++ (~2.9 MB raw, ~0.8 MB compressed) |
| `libgomp.so.1.0.0` | gcc 15.3.0 | GPL-3.0-with-GCC-exception | Same reason: the toolchain has OpenMP, so Buildroot insists it be selected |
| `libgcc_s.so.1` | gcc 15.3.0 | GPL-3.0-with-GCC-exception | Unwinder, used by glibc |

Removing libstdc++/libgomp would save roughly 1 MB compressed. They are kept
because deleting them behind Buildroot's back makes a future C++/OpenMP
package fail at runtime instead of build time. If the size budget ever gets
tight, removing them in `post-build.sh` is the first lever.

## Host-only tools (not in the image)

| Tool | Version | Why |
|---|---|---|
| squashfs-tools (`mksquashfs`, `unsquashfs`) | 4.6.1 | Builds and inspects the image |
| fakeroot | 1.37.1.2 | Applies the fake root ownership that mksquashfs records |
| gcc, binutils, make, perl, python3, rsync, cpio, bc, bison, flex, gawk, texinfo, help2man, ncurses | host distro (Ubuntu 24.04) | Buildroot host requirements |

## Deliberately not included

- **mosquitto broker** — a broker on a 32 MB speaker is pointless; sonosd is a
  client. `libmosquitto` only.
- **UBI tools** — the flash map is raw mtd + jffs2 + squashfs, no UBI. A few
  UBI binaries still appear because Buildroot's `ubihealthd` defaults on and
  selects the hidden `BR2_PACKAGE_MTD_UBIFS_UTILS`; they are never invoked and
  cost ~80 KiB compressed.
- **devtmpfs / udev** — the kernel has no `CONFIG_DEVTMPFS`; `/dev` is static
  plus `mdev`.
- **locales, man pages, headers, static libraries** — Buildroot strips them.

## ABI notes

**long double is 128-bit, not 64-bit.** The Bootlin e300c3 toolchain is
configured `--with-cpu-32=e300c3` with the default `-mabi=ibmlongdouble`:
`__LDBL_MANT_DIG__` is 106 and `__SIZEOF_LONG_DOUBLE__` is 16, i.e. IBM
double-double, not the 64-bit long double that a `-mlong-double-64` build
would produce. There is no Buildroot option for an external toolchain's long
double format; it is baked into the compiler.

Why it does not break this firmware: nothing here passes `long double` across
a binary boundary. The kernel ABI in `firmware/sonosd/audioctl_abi.h` uses
`int`, `unsigned long` and fixed-width payloads only, so `/dev/dsp` and
`/dev/audioctl` ioctls are unaffected. It *would* matter if we ever linked
against pre-built vendor binaries that exchange `long double` with us — we do
not. `printf("%Lf")`, `strtold()` etc. are self-consistent within this rootfs.

## Kernel compatibility risk — the image is NOT hardware-ready

This is the single most important caveat in this file, and it is not a size
problem.

**The toolchain's glibc targets a much newer kernel than the device runs.**

| Fact | Value | Evidence |
|---|---|---|
| C library | glibc 2.44 | `strings libc.so.6` → "GNU C Library (Buildroot) stable release version 2.44" |
| Minimum kernel declared by that libc | **3.2.0** | `readelf -n libc.so.6` → `.note.ABI-tag: OS: Linux, ABI: 3.2.0` |
| Kernel headers the toolchain ships | 5.10 | `LINUX_VERSION_CODE 330495` in the sysroot's `linux/version.h` |
| Kernel on the device | **2.6.35** | Sonos GPL drop (`src/linux-2.6.35`) |

glibc 2.34 and later dropped support for kernels older than 3.2. The Bootlin
2026.08 toolchain was built with `BR2_PACKAGE_GLIBC_KERNEL_COMPAT=y`, which in
Buildroot means *no* `--enable-kernel=` override, so the libc's built-in floor
(3.2.0) is what ships. The device kernel is 2.6.35 — older than the floor by
the whole 2.6→3.2 span.

Buildroot cannot even be told the truth here: its external-toolchain header
check is strict for any series that is not the newest, so declaring 3.0 fails
with `Incorrect selection of kernel headers: expected 3.0.x, got 5.10.x`, and
`BR2_TOOLCHAIN_EXTERNAL_HEADERS_REALLY_OLD` (2.6.x) selects no version symbol
at all, which makes `support/scripts/check-kernel-headers.sh` compile
`KERNEL_VERSION(,0,0)`. `5.10` is therefore the only value that builds — it
describes the toolchain, not the target.

### Quantified: syscalls the binary expects that 2.6.35 does not have

The 2.6.35 PowerPC table (`arch/powerpc/include/asm/unistd.h`) ends at
`__NR_rt_tgsigqueueinfo` = **322**. Scanning every ELF in the image for
`li r0,<nr>; sc` pairs (script: `out/syscall-scan.py`) finds **63 distinct
syscall numbers above 322**, with these concrete, named sites in
`lib/libc.so.6`:

| Syscall | nr | Added in | Found in |
|---|---|---|---|
| `prlimit64` | 325 | 2.6.36 | `getrlimit64`, `setrlimit64`, `prlimit` |
| `send` | 334 | 4.3 (ppc32) | `__send` |
| `sendto` | 335 | 4.3 (ppc32) | `sendto` |
| `recv` | 336 | 4.3 (ppc32) | `recv` |
| `recvfrom` | 337 | 4.3 (ppc32) | `recvfrom` |
| `setsockopt` | 339 | 4.3 (ppc32) | `__setsockopt64` |
| `getsockopt` | 340 | 4.3 (ppc32) | `__getsockopt64` |
| `accept4` | 344 | 4.3 (ppc32) | `accept4` |
| `statx` | 383 | 4.11 | present in `ld.so.1` |

The socket numbers matter most: on PowerPC 32-bit, 2.6.35 has **only**
`socketcall` (nr 102); the direct `socket`/`connect`/`send`/… syscalls were
added for ppc32 in Linux 4.3. glibc 2.44, built against 5.10 headers, emits the
direct numbers, so on a 2.6.35 kernel those wrappers return `ENOSYS`. That
breaks DHCP, MQTT and SSH — i.e. everything sonosd needs to be useful — even if
the kernel successfully mounts the image.

`prlimit64` is 2.6.36, exactly one release after the target, and glibc's
`getrlimit`/`setrlimit` wrappers call it unconditionally.

**Consequences:**

1. The image **may not boot** as built (the dynamic loader may hit a missing
   syscall while mapping libraries), and
2. even if it boots, **networking and resource-limit calls will fail**, and
3. this cannot be fixed with Buildroot options. The C library is the problem.

**Options, in order of effort:**

- Build a **musl** or **uClibc-ng** e300c3 toolchain and point
  `BR2_TOOLCHAIN_EXTERNAL_PATH` at it. Both still support 2.6.x kernels.
  This is the only option that keeps the pinned-Buildroot approach.
- Pin an **older glibc toolchain** whose `--enable-kernel` is ≤ 2.6.35
  (glibc ≤ 2.33 supported 2.6.32). Bootlin's older `powerpc-e300c3` releases
  are the natural candidates; `toolchain.lock` would need a new URL and hash.
- Run a **newer kernel** on the device — not possible without reverse
  engineering the Sonos board support, which is out of scope.

None of these were done here: the task pinned the existing toolchain, so the
conflict is recorded rather than papered over.

**Not verified:** whether the image actually boots. There is no speaker
attached, and `qemu-ppc` is not installed. Even with qemu-user it could not be
proven: qemu executes syscalls against the *host* 6.8 kernel, so every syscall
above would succeed there and the failure would only appear on real hardware.

## Size and budget

| Measurement | Bytes | MiB |
|---|---|---|
| Raw squashfs | 4,181,070 | 3.99 |
| Padded to 128 KiB eraseblock | 4,194,304 | 4.00 |
| Budget (half of 16 MiB mtd3) | 7,864,320 | 7.50 |
| **Headroom (padded)** | **3,670,016** | **3.50** |
| Headroom (raw) | 3,683,250 | 3.51 |

**PASS** — the padded image fits with 44% of the slot unused. Inodes: 432.
Because the budget assumes the *padded* image, padding is what is compared;
both numbers are in `out/rootfs.sizes.txt`.

Ten largest files in the image:

| Bytes | Path |
|---|---|
| 2,957,744 | `/usr/lib/libstdc++.so.6.0.34` (unused, see above) |
| 2,236,568 | `/lib/libc.so.6` |
| 1,050,412 | `/bin/busybox` |
| 918,980 | `/lib/libm.so.6` |
| 461,016 | `/usr/sbin/dropbear` |
| 395,612 | `/usr/lib/libgomp.so.1.0.0` (unused) |
| 264,428 | `/lib/ld.so.1` |
| 263,408 | `/usr/lib/libcrypt.so.2.0.0` |
| 132,504 | `/lib/libgcc_s.so.1` |
| 132,356 | `/usr/lib/libmosquitto.so.1` |

## Reproducibility

Two clean builds (`scripts/build-rootfs.sh --clean`) produce byte-identical
images:

```
FINAL_HASH_PLACEHOLDER  out/final-a.squashfs
FINAL_HASH_PLACEHOLDER  out/final-b.squashfs
```

What makes it deterministic:

- `SOURCE_DATE_EPOCH=1700000000`, exported to Buildroot and to every package;
- `post-build.sh` normalises every inode mtime (`touch -h -d @$SOURCE_DATE_EPOCH`).
  Buildroot's squashfs arguments do not expose `mksquashfs -all-time`
  (`BR2_TARGET_ROOTFS_SQUASHFS_COMP_OPTS` is a promptless symbol), so this is
  done on the tree instead;
- mksquashfs takes its superblock creation time from `SOURCE_DATE_EPOCH`
  (the image reports `Creation or last append time Tue Nov 14 22:13:20 2023`);
- the build output directory is fixed (`out/buildroot`), which Buildroot needs
  because absolute paths leak into some intermediate files.

Caveat: reproducibility is only claimed for the same output directory and the
same host toolchain (Ubuntu 24.04 host gcc for host tools), which is what both
builds used.

## Other deviations from the brief

- **`BR2_TOOLCHAIN_EXTERNAL_HEADERS_5_10`** instead of a 2.6-compatible value:
  explained above; Buildroot offers no workable way to declare 2.6.
- **C++ and OpenMP enabled** (`libstdc++`, `libgomp` in the image): forced by
  Buildroot's external-toolchain consistency check, not by any package.
- **`FAKEROOTDONTTRYCHOWN=1`** in the build environment: this host's user
  namespace makes a real `chown` to uid 0 return `EINVAL`, and fakeroot tries
  the real syscall first. The flag makes fakeroot record the fake ownership
  instead. The resulting image is verified `root:root` throughout with
  `unsquashfs -ll`.
- **Docker was tried and is unusable here**: the daemon has `userns` enabled,
  container root maps to an unprivileged host uid, and even `touch` inside the
  bind mount fails with `Permission denied`. A container build is therefore
  not an option on this machine.
