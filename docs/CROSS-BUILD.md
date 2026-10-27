# Cross-building streaming endpoints for the Play:5

The device is a 32-bit **big-endian** PowerPC (e300c3) running Linux **2.6.35**.
That combination rules out most prebuilt material: nothing on a modern distro's
package shelf is big-endian PPC, and anything rebuilt against a current glibc
will not run on the speaker's kernel.

This document records what has been built, what it took, and what is still
blocked.

## shairport-sync 4.3.5 — AirPlay 1

**State: BUILDS, ELF verified, runs under emulation.** Not run on a speaker.

Built from source at `/home/ben/sonos-bench/src/shairport-sync` with the
project's 2.6.35 toolchain, against the Buildroot staging sysroot.

```
ELF 32-bit MSB executable, PowerPC or cisco 4500, version 1 (SYSV),
  dynamically linked, interpreter /lib/ld.so.1, for GNU/Linux 2.6.35,
  with debug_info, not stripped
```

| Property | Value |
|---|---|
| Size | 225,176 B unstripped, 200,636 B stripped |
| Class / endianness | ELF32, big endian |
| ABI | `for GNU/Linux 2.6.35` in the interpreter tag |
| Runtime deps | `libmbedtls` `libmbedcrypto` `libmbedx509` `libconfig` `libpopt` `libstdc++` `libpthread` `librt` `libm` `libgcc_s` `libc` |

Under `qemu-ppc` with the Buildroot sysroot it runs and reports its build:

```
$ qemu-ppc-static -L out/buildroot/staging ./shairport-sync --version
4.3.5-mbedTLS-tinysvcmdns-sysconfdir:/usr/etc
```

### How it was configured

```sh
export PATH=<toolchain>/bin:<buildroot>/host/bin:$PATH
export PKG_CONFIG_SYSROOT_DIR=<buildroot>/staging
export PKG_CONFIG_LIBDIR=<buildroot>/staging/usr/lib/pkgconfig
export CFLAGS="--sysroot=<buildroot>/staging -O2"
export LDFLAGS="--sysroot=<buildroot>/staging"

./configure --host=powerpc-buildroot-linux-gnu \
            --with-airplay-2=no --with-tinysvcmdns=yes --with-ssl=mbedtls
make -j4
```

Two details that cost time:

- **`autoreconf` is not on the build host.** The git tree has no generated
  `configure`. Buildroot's own host tools include it —
  `<buildroot>/host/bin/autoreconf` — so put that directory on `PATH` rather
  than installing autotools.
- **Cross builds need the sysroot, not just `--host`.** Without
  `PKG_CONFIG_SYSROOT_DIR` and `--sysroot` in `CFLAGS`/`LDFLAGS`, `pkg-config`
  returns host paths and the link line picks up host libraries.

### Dependencies, and why they are Buildroot packages

Each was added to `buildroot/configs/moggie_powerpc_defconfig` rather than
hand-built, so the whole chain stays in the reproducible pipeline:

| Package | Why |
|---|---|
| `popt` | shairport-sync command-line parsing |
| `libconfig` | its configuration file format |
| `mbedtls` | TLS (chosen over OpenSSL for footprint) |
| `libplist` | AirPlay 2 property lists |
| `libsodium` | AirPlay 2 crypto |
| `libgcrypt` | AirPlay 2, required by configure |

With those and **no ffmpeg**, the image measures:

```
raw     4,437,288 B  (4.23 MiB)
padded  4,456,448 B  (4.25 MiB)
spare   3,407,872 B  against the 7,864,320 B budget
inodes  485
```

## AirPlay 2 — blocked on size

**State: BLOCKED, measured.**

`--with-airplay-2` needs `libavutil` on top of everything above. With Buildroot's
full `ffmpeg` package the image measured **14,715,616 B** — 87 % over the
7,862,320 B budget, before shairport-sync itself is added.

A full ffmpeg is the wrong shape for a 7.5 MiB rootfs. The two plausible routes:

1. **avutil-only.** Buildroot can be told to build only `libavutil`
   (`BR2_PACKAGE_FFMPEG_AVUTIL=y` with the others `# ... is not set`). That was
   attempted, but the measured size did not change — Buildroot does not prune
   files already installed into `target/` when a package's component set
   shrinks, so **that measurement is invalid and no conclusion was drawn from
   it.** It needs a clean build of `target/` to measure honestly.
2. **Do without ffmpeg.** AirPlay 2 uses libavutil for resampling and format
   conversion. If the ALSA/DSP path can be fed the format AirPlay 2 delivers,
   the dependency may be avoidable — but that is a design question, not a
   packaging one, and it is unproven.

Until one of those is settled, **AP2 does not fit**, and the deliverable is
AirPlay 1.

## What "verified" means here

| Claim | Status |
|---|---|
| Compiles for powerpc big-endian | **VERIFIED** — `file`, `readelf`, interpreter tag |
| Links only against target libraries | **VERIFIED** — `readelf -d` dependency list |
| Executes | **EMULATED** — `qemu-ppc`, prints version |
| Plays audio from an iPhone | **NOT VERIFIED** — needs the speaker |
| AirPlay 2 | **BLOCKED** — does not fit the budget as packaged |

Stripping with the cross `strip` takes it from 225,176 B to 200,636 B — about
11 %, not the 60 % one might assume, because most of the size is real code
rather than debug info. Strip before it goes into an image anyway.
