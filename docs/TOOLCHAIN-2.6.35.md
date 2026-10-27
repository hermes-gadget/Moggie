# A toolchain whose libc runs on 2.6.35

Issue #22 in one line: every userspace binary we built was linked against a libc
that declares `Minimum supported kernel: 3.2.0`, on a device running 2.6.35.

This is the fix, and the evidence that it works.

`docs/TOOLCHAIN.md` records the problem and the measured exposure. This document
records the solution.

## Result

| | Old (Bootlin 2026.08-1) | New (crosstool-NG) | Stock firmware |
|---|---|---|---|
| glibc | 2.44 | **2.23** | 2.23 |
| ABI tag kernel floor | **3.2.0** | **2.6.35** | 2.23-era |
| gcc | 15.3.0 | 5.5.0 | 2015-era |
| binutils | 2.45.1 | 2.27 | — |
| long double | 128-bit IBM | **unspecified** (= IBM double-double) | unspecified |
| `getrandom()` | glibc wrapper present | **no `SYS_getrandom` at all** | absent |

That last row is the point. The new toolchain is built against 2.6.35-era kernel
headers, so the syscall does not exist in its headers — which is exactly the
world the speaker lives in. The old toolchain assumed a syscall 14 releases newer
than the device.

## Recipe

crosstool-NG **1.26.0**, driven by `toolchain-2.6.35.ctng.config` in this repo:

```sh
ct-ng defconfig DEFCONFIG=toolchain-2.6.35.ctng.config
ct-ng build
```

Key settings:

```
CT_ARCH_POWERPC=y          CT_ARCH_CPU="e300c3"
CT_ARCH_ENDIAN="big"       CT_ARCH_BITNESS=32
CT_ARCH_powerpc_ABI_DEFAULT=y            # hard float
CT_TARGET_VENDOR="buildroot"             # -> powerpc-buildroot-linux-gnu
CT_BINUTILS_VERSION="2.27"
CT_GCC_VERSION="5.5.0"
CT_LIBC_GLIBC=y            CT_GLIBC_VERSION="2.23"
CT_GLIBC_EXTRA_CONFIG_ARRAY="--enable-kernel=2.6.35"
```

`--enable-kernel=2.6.35` is the whole point: it tells glibc which syscalls it may
assume exist, so it stops emitting fallback paths for kernels it will never meet
and stops assuming ones that do not exist yet.

Built in Docker, because this host has no root and no `gperf`/`bison`/`flex`.
The build is a one-off; the tree is **not** committed.

## Evidence

**The libc's own declaration** — this is the check #22 asked for:

```
$ readelf -n <sysroot>/lib/libc.so.6
Displaying notes found in: .note.ABI-tag
  GNU  0x00000010  NT_GNU_ABI_TAG (ABI version tag)
    OS: Linux, ABI: 2.6.35
```

```
$ strings -a <sysroot>/lib/libc.so.6 | grep "GNU C Library"
GNU C Library (crosstool-NG 1.26.0) stable release version 2.23, ...
```

**`sonosd` built with it:**

```
$ readelf -n sonosd
    OS: Linux, ABI: 2.6.35

$ readelf -V sonosd | grep -oE 'GLIBC_[0-9.]+' | sort -u -V | tail -1
GLIBC_2.4

$ readelf -A sonosd
  Tag_GNU_Power_ABI_FP: hard float, unspecified long double
```

The ABI attributes now match the stock firmware's libc exactly. The Bootlin
toolchain produced `128-bit IBM long double`; this one produces `unspecified`,
which is what Sonos shipped.

**Runtime, under qemu-ppc:**

```
$ qemu-ppc-static -L <new-sysroot> ./sonosd --selftest
selftest: 108 checks, 0 failures
selftest: PASS

$ qemu-ppc-static -L <new-sysroot> ./probe-dyn
  ok    heap: malloc/free 1 MiB
  ok    fs: open/write/lseek/read/stat/rename
  ok    threads: 4 threads, mutex-protected counter = 4000
  ok    net: loopback connect/accept/send/recv
  ok    time: clock_gettime + nanosleep(50ms)
  note  entropy: no SYS_getrandom for this architecture
  ok    entropy: /dev/urandom
  ok    mmap: anonymous 64 KiB
  ok    signal: sigaction + raise(SIGUSR1)
14 passed, 0 failed
```

Both the static and dynamic builds pass: `tests/compat` was built with this
toolchain to prove the probe itself is portable to a 2.23 libc.

`tests/compat/compat-probe.c` had to be changed for this. It used
`<sys/random.h>` and `getrandom()`, which glibc did not expose until 2.25. It now
calls `SYS_getrandom` through `syscall(2)` under `#ifdef`, and treats `ENOSYS` as
the *expected* answer on 2.6.35 rather than a failure. The `note` line above is
the probe confirming the toolchain really is built against 2.6.35 headers.

## What this does not prove

- **Nothing has run on the speaker.** qemu-user runs on the host's 6.8 kernel;
  it cannot model a 2.6.35 kernel. This removes the *known* incompatibility, it
  does not demonstrate compatibility.
- **The rootfs still needs rebuilding** against this toolchain. The current
  Buildroot image is built with glibc 2.44 and inherits the 3.2.0 floor.
- **Binaries are larger.** The static compat probe went from 1,117,920 B
  (gcc 15, glibc 2.44) to 3,600,944 B (gcc 5.5, glibc 2.23). Static linking is
  unusual here; the dynamic `sonosd` is 192,044 B. The rootfs budget still needs
  measuring with this toolchain.
- **gcc 5.5 is twelve years old.** It lacks modern diagnostics and hardening
  defaults. For a 32 MiB device running a 2010 kernel that is the correct
  trade, but it should be stated rather than discovered.

## Next

1. Rebuild the rootfs with this toolchain and re-measure the squashfs.
2. Re-run `tests/compat` against the **image's** libc, not the toolchain's.
3. Confirm `sonosd --selftest` inside the built rootfs under qemu.
4. Then, and only then, the hardware question.
