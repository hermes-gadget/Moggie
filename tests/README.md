# tests/

- `ota/` — negative tests for the update path. Every case must fail safely.
- `ha/` — MQTT discovery payload validation. Stdlib only, pytest-compatible.
- `mqtt/` — sonosd against a **real** Mosquitto broker (see below).
- `emulated/` — the PowerPC binaries running under qemu-user (see below).
- `compat/` — the runtime probe that decides whether a libc works on 2.6.35.
- `../firmware/sonosd/tests/` — sonosd unit tests (arbiter, volume, jsonlite, config).

Run with `make ota-test`, `make ha-test`, `make mqtt-test`,
`make emulated-test` and `make compat-probe`, and `make test` in
`firmware/sonosd`.

## compat/ — can this libc actually run on the device?

Compiling and linking proves nothing about the kernel the binary will meet. The
compat probe exercises what Moggie depends on and reports each result:

| area | what it does |
|---|---|
| heap | malloc/free 1 MiB |
| filesystem | open/write/lseek/read/stat/rename/unlink |
| threads | 4 threads, mutex-protected counter must reach 4000 |
| sockets | loopback socket/bind/listen/connect/accept/send/recv |
| timing | `clock_gettime(CLOCK_MONOTONIC)` + `nanosleep(50ms)` |
| entropy | `getrandom()` and `/dev/urandom` |
| memory | anonymous `mmap` |
| signals | `sigaction` + `raise` |

Built twice: **static**, which runs in a bare rootfs with no dynamic loader, and
**dynamic**, which loads the real `ld.so.1` and `libc.so.6` that would ship.

```sh
make compat-probe
```

```
  ok   static build
  ok   static binary is 32-bit MSB
  ok   static probe under qemu (15 passed, 0 failed)
  ok   dynamic build
  ok   dynamic probe under qemu (15 passed, 0 failed)
  info libc:   GNU C Library (Buildroot) stable release version 2.44.
  info floor:  Minimum supported kernel: 3.2.0
5 passed, 0 failed
```

`getrandom()` is Linux 3.17+. On the current libc it is reported as a note
rather than a pass when it falls back, because on a 2.6.35 device that is the
expected path.

The same binary is the first thing to run on hardware day. A pass under qemu
only proves it works on the host kernel.

## emulated/ — does it actually run on the target?

A cross-compiled ELF that links is not evidence that it executes. Endianness
bugs, unaligned access, ABI mismatches and missing syscalls only show up when the
binary runs. `tests/emulated/run.sh` runs `sonosd` under `qemu-ppc-static`
(qemu-user, pulled from `multiarch/qemu-user-static` — this host has docker but
no root for a package).

| check | what it proves |
|---|---|
| qemu present | the emulator is available and runnable |
| cross build | the target build still succeeds |
| arch check | `file` reports 32-bit MSB PowerPC |
| `--version` | the binary loads and executes on the target ABI |
| `--selftest` | 108 in-process checks pass under emulation |
| bad config | an unknown key is rejected on the target ABI |

```sh
make emulated-test
```

```
6 passed, 0 failed
PowerPC binaries execute correctly under qemu-user
```

This is EMULATED, not HARDWARE VERIFIED. It cannot exercise the DSP, NAND, the
amplifier or the codec — those need the speaker.

## mqtt/ — the test that was missing

The sonosd unit tests only ever used the in-RAM mock broker, so "the MQTT backend
compiles" was never evidence that it could connect, subscribe or handle a command.
`tests/mqtt/run.sh` builds the daemon against the real libmosquitto, starts
`eclipse-mosquitto:2` in a container, and drives it end to end:

| check | what it proves |
|---|---|
| build | the daemon links the real library, not the stub |
| broker up | the container is actually reachable |
| availability | retained `online` is published on start |
| discovery | the discovery document is retained and well-formed |
| single-key command | `{"volume":42}` reaches the device state |
| multi-key command | every key in one payload is applied, not just the first |
| tone group | bass/treble/balance apply as one update, not three clobbers |
| clamping | `{"bass":99}` clamps to 10 rather than being accepted |
| bad payload | the daemon survives unparseable input |
| command payload | the daemon survives a `{"cmd":"status"}` payload |

There is no system-wide libmosquitto on the build host and no root to install one,
so build it into `dl/` first:

```sh
make mqtt-lib     # downloads, verifies and builds libmosquitto 2.0.20 into dl/
make mqtt-test
```

The multi-key case is a regression test. The command handler used an `else if`
chain, so `{"mute":true,"loudness":true}` applied mute and silently dropped
loudness — worse than refusing it.

```sh
bash tests/ota/run.sh        # or: make ota-test
```

The suite builds `firmware/updater` first, then runs 23 cases against a 16 MiB
regular file standing in for mtd3 and the boot chooser in simulation mode. **No
`/dev/mtd*` device is opened, ever** — that is a hard rule, not a convenience.

Cases, in the order they run:

| case | what it proves |
|---|---|
| `bad_magic` | a signed non-squashfs file is refused |
| `bad_signature` | a forged signature aborts before the first write |
| `boot_ok_commits` | `boot_ok` clears the failure count and is consumed |
| `check_only` | `--check` verifies without touching anything |
| `chooser_shells` | the chooser runs under `sh`, `dash` and `busybox ash` |
| `chooser_slot_integrity` | bad magic / bad hash is skipped; both bad → recovery |
| `corrupt_image_midstream` | a flipped image byte is caught by the streaming hash |
| `embedded_key` | a production build with a compiled-in key verifies, and rejects another key |
| `fallback_without_boot_ok` | two unconfirmed boots, then the other slot |
| `fuzz_manifest` | 40 mutated manifests: refused, never a crash |
| `happy_path` | install, commit, boot, `boot_ok`, verify.py agrees |
| `install_boot_commit` | end-to-end, then the next update alternates back |
| `manifest_shape` | validly signed but re-ordered / duplicated fields are refused |
| `missing_signature` | an unsigned manifest is refused |
| `oversize_image` | over 7.5 MiB cannot be signed, and is refused if it is |
| `power_cut_mid_write` | `kill -9` mid-write leaves the old slot intact and bootable |
| `production_refuses_regular_file` | the release build will not write a plain file |
| `readback_mismatch` | a bad read-back never makes a slot bootable |
| `refuses_mtd0` | `/dev/mtd0` is not a legal target |
| `refuses_running_slot` | the running slot is never written |
| `tampered_manifest_field` | editing a signed field invalidates the signature |
| `truncated_image` | a short image is caught by the size check |
| `wrong_model` | a manifest for another speaker is refused |

Artifacts land in `build/tmp/ota-test/` and are removed on success; a failing run
keeps them (and the per-case logs) for inspection. `OTA_KEEP=1` keeps them
always.

`mutate.py` is the byte-surgery helper the cases use — corrupt an image, truncate
it, flip a signature bit, re-sign a re-ordered body. None of it is used by the
firmware.
