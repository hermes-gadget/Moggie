# Engineering run log

_2026-10-27, hardware-free autonomous run. Newest entries first._

## Status at time of writing

| Area | State | Evidence |
|---|---|---|
| Kernel + `audiodev` | done | `vmlinux` 4,204,896 B, gzip 1,703,280 B (81% of the 2 MB partition) |
| MQTT control plane | done | 20/20 integration checks against a real broker |
| Runtime compatibility probe | done | 15/15 under qemu |
| PowerPC execution | emulated | 6/6, `--selftest` 108 checks under qemu |
| Audio analysis | done (analysis only) | DSP graph, amp state machine, thermal derating mapped |
| Toolchain for 2.6.35 | **in progress** | stock glibc 2.23 vs ours 2.44 — issue #22 |
| Rootfs (Buildroot) | **in progress** | hand-staged 5.87 MiB; reproducible build not yet landed |
| DLNA renderer | **in progress** | `firmware/dlna/` being written |
| Boot / NAND / OTA | OTA done, boot untested | 23/23 OTA negative cases; chooser never booted |
| Hardware | **blocked** | UART header not fitted, speaker not powered |

## Pull requests merged

| PR | What |
|---|---|
| #16 | `sonosd`: apply every key in a command payload; real-MQTT integration tests |
| #17 | `docs/audio`: map the stock DSP graph; `tools/dsp-capture.py` |
| #18 | `sonosd`: publish the MQTT control surface it can actually back (7 entities) |
| #19 | `docs/status`: correct the MQTT/HA claims |
| #20 | `test/emu`: run PowerPC binaries under qemu-user |
| #21 | `docs`: toolchain libc requires kernel 3.2, speaker runs 2.6.35 |
| #23 | `docs`: measure the actual syscall exposure |
| #24 | `docs/status`: record the toolchain finding |
| #25 | `test/compat`: runtime probe for libc-on-2.6.35 |
| #26 | `chore`: untrack `.tokensave` |
| #27 | `docs/audio`: amplifier state machine, rails, thermal derating |

## Bugs found and fixed

1. **MQTT commands silently dropped.** The handler was an `else if` chain, so
   `{"mute":true,"loudness":true}` applied mute and discarded loudness. Invisible
   to the unit tests because they only used the mock broker.
2. **HA switch payloads rejected.** HA's `command_template` sends `"ON"` as a
   quoted string; `json_get_bool()` only accepted bare `true`/`false`/`0`/`1`.
   Now accepts `ON`/`OFF` case-insensitively and rejects `"online"`.
3. **The HA contract was fiction.** `docs/HA.md` documented 16 entities on a
   per-entity topic scheme the daemon never spoke. Split into target vs
   implemented, with the divergence recorded.
4. **A 3.5 MB tool database in the repo.** A `git add -A` swept `.tokensave/`
   into a commit. Untracked and ignored.
5. **Buildroot agent's toolchain prefix typo** (`${PREFIX}gcc`), self-corrected.
6. **A healthy speaker would have rolled itself back.** The initramfs chooser
   charges a boot attempt before `switch_root` and only clears it when userspace
   writes `/jffs/boot_ok`. `sonosd` never wrote it, so every boot counted as a
   failure and the third boot would have fallen back to the other slot — then
   ping-ponged forever. Fixed with a `bootstate` module; verified on the host and
   cross-compiled under qemu.

## Toolchain

| | Version |
|---|---|
| Stock firmware | glibc **2.23**, crosstool-NG 1.22.0, gcc-era 2015 |
| Pinned Bootlin | gcc **15.3.0**, glibc **2.44**, `Minimum supported kernel: 3.2.0` |
| Device kernel | **2.6.35** |

Measured exposure: `sonosd` imports nothing newer than 2.6.35; `libmosquitto`
adds `getrandom` (3.17+) with a glibc `/dev/urandom` fallback. The glibc runtime
itself is configured for 3.2+ and cannot be audited at symbol level.

A replacement toolchain (glibc 2.23, `--enable-kernel=2.6.35`) is being built in
Docker, since the host has no root and no gperf/bison/flex.

## Build artefacts

```
vmlinux.bin            4,204,896 B
gzip payload           1,703,280 B   (81% of the 2 MB kernel partition)
audiodev.ko              120,720 B
sonosd (PPC, +mosquitto) 308,816 B   links libmosquitto.so.1
sonosd (PPC, no mqtt)     81,656 B
compat-probe (static)  1,117,920 B
rootfs (hand-staged)   6,152,200 B   = 5.87 MiB uncompressed
```

Compressed firmware size: **not yet measured** — the reproducible Buildroot image
is still being built.

## Test results

```
sonosd unit            272 checks, 0 failures
MQTT integration        20 checks, 0 failures   (real Mosquitto broker)
OTA negative            23 cases,  all pass
PowerPC emulation        6 checks, 0 failures
compat probe            15 checks, 0 failures   (static and dynamic)
dsp-capture selftest     4 checks, 0 failures
```

## DSP findings

- **The DSP is a configuration graph, not fixed coefficients.** `anacapad` runs
  an IIR block engine; filter types include `highpass1/2`, `lowpass1/2`,
  `hfshelf`, `lfshelf`, `allpass1`, `DynamicHighPass` and two variable-notch
  variants, plus `Crossover` / `SubCrossover`.
- **The graph is readable over HTTP on a real unit:** `GET /getDSP`, `POST
  /putDSP`, `POST /save_eq_presets`, with presets at `/jffs/eqdata.txt`.
- **Volume is dB with a clamp**, not a linear 0–100.
- **The amplifier is a state machine**: pre-emptive turn-on, rail switching
  (`AMP_OFF`/`LO`/`HI`), an idle off timer, mute-cycle fault recovery.
- **Thermal response is graded**: `HalfPowerOverTemp` with primary and secondary
  stages — warn, halve power, fault.

## Remaining hardware requirements

1. Solder the 4-pin UART header and fit a permanent tail. Not done.
2. First power-on since ~2020; expect possible NAND bit rot.
3. Dump all six MTD partitions twice and verify hashes before anything is written.
4. Run `tests/compat/compat-probe` first — it answers whether the toolchain's
   libc works on 2.6.35 in one shot.
5. `GET /getDSP` to capture the real filter graph.
6. `POST /unlock` over the network as the cheapest jailbreak attempt.

## Outstanding blockers

- Replacement toolchain under construction (#22).
- Buildroot rootfs not yet reproducibly built or size-verified.
- DLNA renderer not yet merged.
- Boot chooser has never been executed by a real kernel.
- NAND behaviour is simulated only.

## Hardware-day readiness

**Not ready.** Nothing here has touched a speaker. The strongest claim this run
supports is: the kernel and module build reproducibly within their size budget,
the daemon runs on the target ABI under emulation, and its MQTT plane works
against a real broker. No audio has ever been produced.
