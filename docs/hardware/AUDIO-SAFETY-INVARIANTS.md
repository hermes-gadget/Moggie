# Audio/DSP safety invariants before custom Play:5 output

**Admission verdict: CLOSED. This is a specification, not an implemented safety
interlock or permission to run audio.** Every invariant and hardware gate below
must be demonstrated before a Moggie build may drive the amplifier. This lane
performed offline documentation and analysis only.

Scope: Play:5 Gen 1 / ZP5S (called `ZPS5` in the GPL source). Evidence comes from
the local stock `34.16-37101-1-16` userspace and its audio module, with corresponding
GPL source. The bench is reported as 57.10-25140; equivalence to that build is
unproven. Source identities, hashes, extraction commands and address conventions
are in the [stock evidence ledger](evidence/2026-10-28-stock-audio.md#source-identities-and-address-conventions).
References `E01`–`E12` below refer to that ledger's evidence index, each with exact
file offsets, instruction addresses or source lines. Proposed requirements are
marked **MUST**; they must not be mistaken for recovered stock behavior.

## What the stock sequence actually establishes

| Phase | Recovered behavior | Evidence and limit |
|---|---|---|
| Driver startup | Stock boot script loads the audio module; its worker starts with both speaker and headphone mute requested. Initialization clears C20, sets C25, and holds register `0x13=0xff` during chip setup. | E01–E02. GPIO names mean port C, pin 20/25 in the stock API, not Linux GPIO numbers. Physical amp-rail wiring is not established. |
| Chip reset/setup | Disable clock handling and temperature acquisition; set C10; reset A11 low/high with 1500 µs at each level; perform E02's register writes with two 1 ms waits; clear C10; wait 10 ms; restore clock handling and temperature acquisition. | E02; `G/workerthread.c:105–151`, `K:0x8800–0x89b8`. Register meanings beyond the source's CS44800 identification are not inferred from their numbers. |
| Lock and settling | Require register `0x2a & 0x40`; up to five lock polls per reset attempt with one `HZ` wait after each unsuccessful poll. After lock: `0x29=0x80`, wait `10*HZ`, `0x03=0xe0`, wait `HZ`, `0x05=0x08`, `0x03=0x20`. | E03. The binary supplies 100/1000 ticks, consistent with source `HZ=100`; nominal waits are 1/10 seconds. Outer reset attempts are unbounded, and transaction results are not checked here. Neither behavior is an acceptable Moggie safety guarantee. |
| Userspace power-on request | Amp manager requests amp power on, then amp mute off on the ordinary path. | E06. The generic power ioctl has no ZPS5 MIO pin; a successful request or `AMP_HI_RAIL` notification cannot prove a supply voltage. |
| ZPS5 physical unmute work | Set C20, wait `HZ/3`, then write `0x13=0xe0`. In the captured module this is **33 ticks**, nominally 330 ms with the inferred 100 Hz tick. | E04, `K:0x8e04–0x8e30`. Actual settled time must be measured; do not replace this with Fenway/Anvil's 25 ms delays. |
| Mute / power-down work | Write `0x13=0xff` if headphones are muted, otherwise `0x3f`; clear C20; wait `HZ/5` (**20 captured ticks**, nominally 200 ms). Userspace requests amp mute before amp power off. | E04/E06. `0xff`, `0x3f`, `0xe0` share speaker/headphone control; they are not interchangeable global mute values. |
| Idle shutdown | Amp manager default is **120 seconds**, configurable via `AmpOffTimerSec`; expiry rechecks whether shutdown is still appropriate. | E07. This is a P34 software default, not a universal timeout or proof of physical rail discharge. |
| Warnings / faults | Userspace distinguishes warning and fault masks and latches a hardware fault state. One warning path logs a 75% clamp while setting an internal level to 73. | E08. Those values do not establish a safe dB curve, thermal threshold, or half-power coefficient. |
| Fault recovery | ZPS5 chip `SRC_UNLOCK` (`0x2a & 0x80`) triggers full chip initialization and restores current mute state. UV14 latching/mute and clearing a latch on unmute also exist in other model branches. | E09–E10. ZPS5 has a distinct IRQ path; Wembley UV14/36 V fault handling must not be assumed to protect ZPS5. |

The Fenway sibling strings include low/high rail, half-power and fault-clear
names, but do **not** establish their electrical or algorithmic meaning on ZP5S
(E11). `docs/AUDIO.md:146–194` now records those limits. Its diagnostics are at file offsets `0x4763e0`,
`0x4765b0`, `0x47664c`; half-power labels at `0x4b09a4`, `0x4b0a0c`; rail names at
`0x4b3e84`, `0x4b3e90`, `0x4b3e9c`. No exact ZP5S low/high-rail threshold,
hysteresis, temperature derating coefficient or amplifier-fault retry count is
claimed here.

## Numbered invariants

The state names below are proposed Moggie states. All transitions must be
serialized; pending requests never count as completed transitions. Any failed
precondition takes the machine to `FAULT_LATCHED`, closes PCM admission and invokes
the validated mute/off action. Fault containment must also work if the bus or
userspace owner stops responding (requirements 5, 13 and 15).

1. **Bind admission to a verified board and build.** A build MUST start with custom
   output disabled and enable it only for a reviewed profile naming board revision,
   driver/ABI, chip revision, channel map and protection data. Unknown identity,
   missing calibration, changed binary hashes or an unsupported profile MUST abort
   admission. Do not select the Fenway GPIO table merely because `/dev/dsp` opens.
   Basis: E01, E06, E09 and the ledger's model-identity evidence.

2. **Establish physical mute before setup or PCM.** On boot, daemon restart,
   source change, reset and recovery, the system MUST enter `OFF_MUTED` with the
   validated amplifier enable inactive and all relevant speaker/headphone paths
   muted. Neither remembered user mute nor volume zero proves physical mute. A
   failure to establish it MUST block setup that could produce output. Basis:
   E01–E04; stock starts the worker muted and explicitly programs GPIO/registers.

3. **Preserve chip initialization order and minimum settling.** A chip/clock reset
   MUST invalidate readiness, retain the mute/enable barrier, and follow the reviewed
   board's ordered reset, register and settling sequence. For the P34 reference,
   the order and source delays are in the sequence table and E02–E03. No unrelated
   DSP, volume or PCM write may interleave. Shortening a wait or replacing the
   sequence requires new board evidence; successful module loading is insufficient.

4. **Require valid lock and bounded initialization.** Before entering
   `READY_MUTED`, the system MUST obtain successful, fresh lock-register reads with
   bit `0x40` set, finish post-lock settling and verify the loaded configuration.
   A stale buffer from a failed read MUST never satisfy lock. Initial Moggie policy:
   **one reset attempt, at most five unsuccessful polls separated by the stock
   one-second reference interval**, then latch closed. Also enforce a finite
   monotonic total deadline, `T_init_max`, covering bounded bus calls, reset and the
   10+1-second post-lock waits. Its validated value is a mandatory profile field;
   absent value means no admission. This deliberately bounds E03's unbounded outer
   loop; it is a new safety policy, not a claimed stock retry limit.

5. **Acknowledge completed hardware work.** Every enable/mute/configuration
   transaction MUST complete successfully within a validated bound and be confirmed
   by meaningful readback or a verified completion signal. Cache/request bits and
   `ioctl()==0` are insufficient. Track requested, applied and failed state
   separately; reject superseded completion events. On bus timeout, failed readback
   or loss of the worker, abort and use the validated independent disable path.
   Basis: E05 and E03's unchecked I²C results.

6. **Enable while still muted, then release mute after settling.** Only
   `READY_MUTED` may enter `ENABLING_MUTED`. Confirm identity, configuration,
   clocks/lock, fresh healthy protection status and the authorized source again;
   enable the amplifier; retain speaker mute for the validated enable interval;
   recheck health, then release register mute and permit samples. For P34 the
   concrete sequence is C20 set → 33 stock ticks → `0x13=0xe0` (E04), not a generic
   “power on” notification (E06). A fault, shutdown or source revocation during
   the wait MUST cancel unmute. No early completion based on nominal wall time.

7. **Mute before disabling and complete the off transition.** On stop, idle expiry,
   shutdown or a protection trip, block new PCM, assert the correct register mute,
   disable the validated amp-enable path, and confirm the final state. Preserve
   P34's register-mute → C20 clear → 20-tick wait ordering (E04); keep PCM blocked
   until the transition is confirmed. If the register write fails, do not wait
   indefinitely to disable the independently verified output stage. Any further
   rail removal or discharge wait requires its own measured profile value.

8. **Keep idle power-down finite and independent of clients.** Use the verified
   idle timeout (P34 default 120 s, E07) and monotonic time. After the last valid
   playback activity, re-evaluate eligibility at expiry and execute requirement 7.
   Bound or reject configuration values that disable/overflow the timer. MQTT,
   discovery traffic, volume queries, paused sources and stale preemptive-wake
   requests MUST NOT renew it indefinitely. A renewed valid source may cancel a
   pending shutdown only through the same serialized state machine.

9. **Load speaker protection and verified PCM format before any audible sample.**
   Admission MUST require the board's channel routing, polarity, sample rate,
   word format, crossover/high-pass, limiter/excursion protection and calibration
   data with known provenance. Unknown, missing or failed configuration MUST keep
   mute asserted. Tone/filter ioctl acceptance cannot stand in for this graph.
   Basis: `docs/AUDIO.md:33–64,103–117` identifies filter/limiter surfaces but lacks
   coefficients; `firmware/sonosd/dsp_linux.c:196–236` cannot read filters and marks
   payload layout assumed. This requirement is conservative engineering policy;
   the full ZP5S graph was not recovered in this lane.

10. **Apply a calibrated gain ceiling and preserve protection mute.** Volume,
    loudness, tone, fixed output and source normalization MUST stay inside the
    verified board's total gain/limiter envelope, including the thermal ceiling.
    User volume changes cannot clear a safety mute or fault latch. No unknown
    readback may cause a configured nonzero gain to be applied. Basis: E08's
    warning clamp and `docs/AUDIO.md:66–76` gain/clamp evidence; current
    `firmware/sonosd/volume.c:60–63` clears user mute on nonzero volume, and
    `dsp.c:82–96` falls back to initial volume on read failure. Their user-facing
    behavior needs a separate, stronger safety latch.

11. **Validate temperature coverage and derate before fault.** Before first
    unmute and throughout playback, require valid, fresh readings or verified
    hardware protection for every relevant thermal domain. Warning triggers the
    validated derating envelope; fault or invalid/stale coverage triggers mute/off
    and a latch. Do not substitute 75% volume, internal 73, a guessed “half-power”
    gain, seeded 30°C, or newer-source 75/80°C thresholds for a measured ZP5S policy.
    Recovery needs verified hysteresis/cooldown. If those values or sensor coverage
    are unavailable, custom output remains disabled. Basis: E08–E09 and E11;
    `G/audioctl.h:282–287` defines 75/80°C, but E09/E12 limit its applicability.

12. **Monitor amplifier, supply and clock faults throughout every transition.**
    Before enable/unmute, during waits and during playback, require a fresh health
    snapshot covering the board's amp fault/current/overload, thermal, supply
    undervoltage and clock-unlock signals. Missing IRQ registration, unreadable
    status or an unknown relevant bit MUST fail closed. The validated profile must
    set maximum sample age, poll interval and fault-to-disable latency. A warning
    may allow only its reviewed derated state; a hard fault always revokes output.
    Do not assume a defined `SYSTAT_*` bit is wired or serviced (E08–E10).

13. **Latch faults; prohibit blind unmute retries.** A persistent fault, SRC unlock,
    I/O failure or deadline expiry MUST revoke readiness, close PCM admission and
    contain output. Initial policy: **zero automatic attempts to unmute to clear
    a fault**. Any later recovery must be a separately reviewed, bounded procedure
    with verified safe physical conditions, full reinitialization and a new
    admission check. Count/rate-limit attempts and require an explicit re-arm after
    the cause clears and cooldown completes. E10 shows clock recovery resets mute;
    E09/E11 show why “unmute clears a latch” is not a ZP5S recovery specification.

14. **Give one owner authority over every output path.** PCM clients, control APIs,
    headphone changes, preemptive wake, diagnostics and direct ioctls MUST pass
    through the same safety state machine. The backend must reject PCM except in
    an admitted playing state, and source revocation must cancel pending unmute.
    Preserve both speaker and headphone mute semantics (`0x13` shared masks, E04).
    For initial bring-up, unverified headphone/line/fixed-output modes stay disabled.
    Basis: E04–E05 and `firmware/sonosd/dsp.c:348–362`, which currently checks source
    arbitration but has no protection-state admission check.

15. **Contain process death, stalls and reboot independently.** Shutdown/error
    paths MUST execute the mute/off sequence; a crash, SIGKILL, hung bus or lost
    safety heartbeat must still disable the amplifier through a verified driver
    or hardware mechanism within the profile's maximum fault latency. Boot,
    watchdog reset and owner restart MUST return to requirement 2 and repeat all
    checks; never restore “playing” from persisted state. This is a required
    extension to the stock reference, not an observed stock watchdog guarantee.
    Current `firmware/sonosd/dsp.c:107–117` only closes the backend on shutdown;
    E05's worker state alone cannot contain owner death.

## Moggie implementation and proof map

**All proof below is required future evidence, not tests executed by this lane.**
The existing locations are insertion/review points at base
`3ff0a53814e6f7ba33cf4c8e8b56a9202d3af5db`. A proposed
`firmware/sonosd/audio_safety.{c,h}` would own policy, with applied-state and
emergency-disable support in the board driver. Those files do not exist in this
documentation branch. Driver selection also needs review:
`scripts/build-kernel.sh:24` currently selects `src/drivers/audiodev`, whereas the
ZPS5 reference here is `drivers/audiodev-connectx` (E01–E05).

| Invariant | Implementation / current gap | Evidence required to close it |
|---|---|---|
| 1 | Build/profile admission before `dsp_init`; `main.c:324–327`; board selection in `scripts/build-kernel.sh:24`. | Reject missing/wrong board, driver hash, calibration and ABI. Identify the actual board/build from a stock capture and correlate its driver with the selected profile. |
| 2 | Safe driver initialization plus `dsp_init` / `dsp_linux.c:93–116`; current code opens PCM before control and does not establish mute. | Cold/warm boot and restart traces show the measured output stage disabled and relevant mutes asserted before clocks/data can make output. |
| 3 | Board-specific driver reset/initialization; policy serializes all other writers. | Compare transaction/GPIO traces against E02/E03 and the installed-build capture; inject competing requests and verify no interleaving or abbreviated waits. |
| 4 | Board driver lock/error handling plus policy init deadline. | NAK/timeout/stuck-unlocked injections never reach unmute; fresh lock needed; one reset attempt/five-poll bound and total deadline observed. |
| 5 | Applied-state acknowledgement in driver/backend; replace `dsp_linux.c:156–193` success/fallback assumptions. | Delay/drop worker requests; fail every read/write; show no false completion, no fallback to another board's mute, and independent disable on bus failure. |
| 6 | Safety owner on `dsp_set_mute(0)` and all wake paths; driver owns C20/register ordering. | Hardware trace with mute, enable, lock, rails and output; health/source fault at each point cancels release; measured minimum settle and maximum deadline hold. |
| 7 | Driver off operation and `dsp_shutdown`; `dsp.c:107–117`, `dsp_linux.c:119–130`. | Stop/shutdown and injected register-write failure prove PCM revocation, register-mute-before-disable when bus works, independent disable when it fails, and measured off/discharge completion. |
| 8 | Safety owner's timer integrated into `main.c:389–410`; must run when the broker is absent. | Fake monotonic clock tests for exact deadline, clock jumps, invalid timer inputs, repeated preemptive wake and renewed source; hardware proves actual idle-off at the configured bound. |
| 9 | Verified graph loader/PCM boundary before `dsp_write_pcm`, filter path `dsp_linux.c:196–236`. | Hash/channel/format mismatch denies output; offline graph/limiter validation plus stock channel/calibration capture and muted electrical verification. |
| 10 | `volume.c:45–71`, `dsp.c:82–96,159–181,227–266`, safety gain limit applied last. | Max-volume/EQ/loudness/source-switch inputs cannot exceed measured envelope or clear safety mute; failed get-volume/mute never restores nonzero output. |
| 11 | Driver thermal acquisition/validity and policy derating; `audioctl_abi.h:95–117` is only a subset of controls/status. | Controlled sensor/status simulation first; then authorized, bounded hardware characterization of valid readings, warnings, derating, fault, stale data and cooldown. No destructive overheat experiment required. |
| 12 | Full board status/IRQ coverage in driver; polling independent of MQTT (`main.c:389–410`). | Fault injection at boot/enable/play/idle and measured fault-to-disable latency; demonstrate current/UV/thermal/clock coverage on this board and stale/unknown-bit rejection. |
| 13 | Latched safety state separate from `volume_state`; driver reset path and policy re-arm. | Repeated permanent fault/SRC-unlock injections yield zero auto-unmute attempts; failed recovery stays latched across source, volume, broker and owner changes. |
| 14 | `dsp_write_pcm`, arbiter, ioctl/backend admission and device ownership; `dsp.h:42–56` currently exposes ordinary audio operations. | Enumerate all writers; simultaneous source/headphone/control/diagnostic requests cannot bypass admission; unauthorized direct opens denied; verify shared register masks electrically. |
| 15 | Driver/hardware heartbeat containment plus `main.c:432` shutdown path. | Kill/stall owner and worker, hang I²C, reboot/watchdog under measured muted conditions; independent shutdown within bound, no restart pulse or automatic resume. |

## What we still do not know — gates before any audio test

Each gate requires evidence for the actual board and installed stock build. A
software/mock pass alone cannot close it. Start with passive stock observations;
this document authorizes no hardware access or test execution.

| Gate | Required resolution | Blocks |
|---|---|---|
| H1 — identity/driver | Confirm hardware/submodel/revision, installed userspace/audio-module hashes, ABI and GPL-source correspondence. The readable P34 package is older; P57 rootfs is not plaintext (ledger provenance). | 1–5 |
| H2 — electrical map | Determine exactly what C20/C25/C10/A11 drive, polarities, reset defaults, enable/rail relationship, rail levels/discharge and any low/high-rail control. Separate physical rails from cached/events-only `AMP_*` states (E04/E06). | 2–7, 12, 15 |
| H3 — timing/completion | Capture reset, lock, post-lock setup, mute and enable waveforms on stock. Validate tick rate/scheduler behavior and minimum settle; define finite `T_init_max`, per-I/O/transition deadlines, health ages and maximum fault latency. No unresolved timing field may have an enabling default (E02–E05). | 3–8, 12, 15 |
| H4 — full signal protection | Recover stock graph, channel routing/polarity, PCM format, gain law, limiter/crossover and factory calibration; identify any DSP firmware dependencies. Prior `/getDSP` request returned HTTP 500 (`/home/ben/.hermes/plans/2026-10-28-moggie-recon-output/RESEARCH-REPORT.md:96–100`); do not promise this endpoint will supply coefficients. | 9–10 |
| H5 — thermal | Establish real sensors and/or on-chip protection, freshness/validity reporting, warning/fault thresholds, actual derating curve and recovery hysteresis. Resolve acquisition validity across reset, P34 raw-counter versus newer-source temperature conversion (E12), the 73/75% discrepancy, and whether F57 half-power labels apply at all (E08–E11). | 11–13 |
| H6 — faults/recovery | Establish actual ZP5S amp/current/UV/clock fault sources, chip status meanings, latch-clear side effects, independent emergency disable and safe re-arm procedure. Wembley UV14/36 V code and sibling diagnostics cannot substitute (E09–E11). | 5, 12–13, 15 |
| H7 — bypasses/output modes | Verify all clients and direct device/ioctl/debug paths, speaker/headphone masks, plugs/unplugs, fixed/line output and physical fail-safe behavior on crashes/reset. Until mapped and constrained, these modes remain barred (E04–E05). | 2, 7, 14–15 |

## Acceptance record

Current result: **0 of 15 invariants proven end to end on hardware; H1–H7 OPEN.**
The code citations above identify the present implementation gaps; the evidence
ledger proves only the stated offline observations. No firmware changes, hardware
commands, custom audio output, thermal/fault exercise, deployment or runtime safety
test was performed by this lane. A future admission record must link, per invariant,
the implementation revision, executed checks and retained hardware traces, including
all failure cases and measured bounds. Passing build/unit tests or this document's
integrity checks cannot change the admission verdict.
