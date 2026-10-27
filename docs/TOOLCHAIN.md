# Toolchain compatibility

**This is the biggest open risk in the project.** Everything in `firmware/` is built
with a toolchain whose C library requires a newer kernel than the speaker runs.

## The numbers

| | Stock firmware | Ours |
|---|---|---|
| C library | glibc **2.23** (crosstool-NG 1.22.0, built by Sonos) | glibc **2.44** (Buildroot/Bootlin) |
| Minimum kernel, as declared by libc | 2.6.32-era | **3.2.0** |
| Target kernel | 2.6.35 | 2.6.35 |

The stock figure comes from the shipped `lib/libc.so.6`:

```
GNU C Library (crosstool-NG crosstool-ng-1.22.0 - allen.antony@sonos.com)
stable release version 2.23
```

Ours, from the pinned Bootlin toolchain's sysroot:

```
GNU C Library (Buildroot) stable release version 2.44
Minimum supported kernel: 3.2.0
```

## What it means

- **The kernel build is unaffected.** `vmlinux` and `audiodev.ko` do not link
  against a C library, so building them with this toolchain is fine.
- **Every userspace binary is affected.** `sonosd`, the DLNA renderer, and
  anything Buildroot produces are compiled against a libc that expects kernel
  3.2.0 or newer. On a 2.6.35 kernel they may fail at load time, or pass
  `--version` and then hit `ENOSYS` on a call that only exists in a newer
  kernel.
- **The qemu test does not clear this.** `tests/emulated/run.sh` proves the
  binary executes on the target *architecture*, but qemu-user runs on the host
  6.8 kernel. It says nothing about 2.6.35.

So the honest verification level for anything built with this toolchain is
**BUILDS**, not EMULATED-for-the-device — and definitely not TESTED on target.

## Why this matters now

This would have been discovered on hardware day as a mystery: a binary that
links, passes its own selftest on the build host, and dies on the speaker.
Finding it from the firmware dump costs nothing.

## Options

| Option | Effort | Confidence |
|---|---|---|
| **crosstool-NG with glibc 2.23 and `--enable-kernel=2.6.35`** | hours (one-off build) | highest — matches what Sonos shipped |
| Older Bootlin release with glibc ≤ 2.33 | low if one exists for `powerpc-e300c3` | good, but pre-2021 releases are not obviously still published |
| Buildroot internal toolchain with uClibc-ng | ~1–2 h build | decent; uClibc-ng supports 2.6.x |
| musl | low | questionable — musl targets 2.6.39+ |
| Ignore it | none | unacceptable |

## What to do

1. Build a toolchain whose libc declares a minimum kernel ≤ 2.6.35.
2. Verify it by reading the banner out of the new `libc.so.6`, not by trusting
   the build system.
3. Rebuild `sonosd` with it and re-run `tests/emulated/run.sh`.
4. Keep the kernel on the current toolchain — it works and changing it would
   risk the size budget for no gain.

Tracked in #21.

## Measured exposure

Checked the dynamic imports of the binaries we actually ship (`objdump -T`).

`sonosd` imports only long-established POSIX symbols — `open`, `close`, `access`,
`fcntl`, `ioctl`, `nanosleep`, `gettimeofday`, `localtime_r`, `fopen`, `fprintf`,
`getenv`, `openlog`, `syslog`, `signal`, `strtol`, string routines, and
`__libc_start_main`. Nothing newer than 2.6.35.

`libmosquitto.so.1` adds the network and thread surface — `getaddrinfo`, `connect`,
`pthread_*` — and exactly one import newer than the target kernel:

```
getrandom        (Linux 3.17+)
```

glibc implements `getrandom()` with a fallback to `/dev/urandom` when the syscall
returns `ENOSYS`, so that path should degrade rather than fail. That is glibc's
behaviour, not something we control or have tested.

### Where that leaves the risk

| Component | Exposure |
|---|---|
| `sonosd` alone | low — no post-2.6.35 imports |
| `sonosd` + libmosquitto | low, with one `getrandom` fallback on the MQTT path |
| glibc runtime itself | **unknown** — startup, TLS and locale are configured for 3.2+ and cannot be audited at the symbol level |
| the rootfs Buildroot produces | unknown until it is built with the same check applied |

So this is a *narrowed* risk, not a cleared one. The libc's declared floor is
still 3.2.0. Issue #22 covers producing a toolchain that removes the question
rather than reasoning about it.
