# Audio behaviour of the original Play:5 Gen 1

**Safety update (2026-10-28):** use
[Audio/DSP safety invariants](hardware/AUDIO-SAFETY-INVARIANTS.md) before planning
custom amplifier output. The [stock evidence ledger](hardware/evidence/2026-10-28-stock-audio.md)
distinguishes ZP5S `-1-16` code paths from the Fenway sibling examined below.
In particular, rail labels, half-power names and fault-clear diagnostics in strings
do not prove ZP5S GPIO wiring, derating values or safe recovery ordering. The prior
recon records `/getDSP` returning HTTP 500; it is not a guaranteed coefficient
capture route (see the invariants' H4 source citation).

Everything here comes from the firmware dumps in `~/sonos-bench/sonos-fenway/` and the
GPL audio driver. **Nothing has been measured acoustically** — there is no physical
speaker in this loop. Treat this as a map of the original signal path, not a guarantee
that reproducing it sounds the same.

Verification levels used throughout: IMPLEMENTED / BUILDS / TESTED / EMULATED /
HARDWARE VERIFIED. This document is analysis only — no row here is above EMULATED.

## What runs the audio

`anacapad` is the process that owns audio on the stock firmware. Three versions are in
the dumps, all 32-bit MSB PowerPC ELF executables, all stripped (no `.symtab`):

| build | size |
|---|---|
| 16.6-00002-diag | 4,006,500 B |
| 34.16-37101 | 4,503,524 B |
| 57.10-25140 | 5,636,472 B |

It opens `/dev/dsp` and `/dev/audioctl` — the same two nodes `sonosd` targets.

## The signal chain is a filter graph, not hardcoded coefficients

This is the important finding. `anacapad` contains an IIR block engine:

```
iirblock %s: filter set number not found in parameter name
iirblock %s: filter section number not found in parameter name
iirblock %s: filter set %d or section %d out of range
iirblock %s: IIR filter type %s not supported
iirblock %s: IIR filter type %s given %d parameters
iirblock %s: IIR filter missing/invalid coefficients
iirblock: Block Param Array too small for filter set
```

So filters are described by **named parameters** of the form `<kind>_<name>`, values
comma-separated, organised into filter sets and sections. Supported filter types, from
the strings in `anacapad`:

```
highpass1  highpass2  lowpass1  lowpass2
hfshelf    lfshelf    allpass1
DynamicHighPass
DynamicHiPass_VarNotchFilter
DynamicHiPass_VarNotchDetFilter
```

Plus `Crossover`, `SubCrossover` and a set of "EB notch" parameters — freq, Q, depth,
smooth, action threshold — which look like an extended-bass notch limiter.

There is also an amplifier protection surface: `AMP_OFF`, `AMP_LO_RAIL`, `AMP_HI_RAIL`,
`AMP_FAULT_STATUS`, `AMP_CURRENT_WARN_STATUS`, `AMP_CURRENT_FAULT_STATUS`. **These are
safety limits and must not be removed or widened.** Moggie has not implemented them yet.

Volume is computed in dB, not percent:

```
calculateVolume %s: sg:%.4f ng:%u sv:%u nv:%.4f
changed playback volume from %f dB to %f dB, sv: %u
changed m_extSrcVolume from %f dB to %f dB, v:%u
ClampVolume
```

So the stock volume control is a dB curve with gain staging, and a `ClampVolume` step.
Moggie clamps percent to 0–100 (`firmware/sonosd/volume.c:7–13`); that is not a
calibrated dB curve or a speaker-protection gain ceiling (safety invariant 10).

## DSP configuration endpoints in the sibling firmware

The stock firmware ships a DSP configuration UI. On 57.10 it is locked behind
`htdocs_locked`, but the page and the endpoints it calls are in the dump:

```
GET  /dsp/configDSP.htm        the UI        ("DSP System (Version: 0.25.3)")
GET  /getDSP                   whole block graph as XML
POST /putDSP                   write it back
POST /save_eq_presets          persist EQ presets
     /dsp/eqdata.txt -> /jffs/eqdata.txt     the preset data
```

The XML is a tree of `<block name=...>` elements holding
`<parameter name="<kind>_<name>" value="v1,v2,...">` children, with coefficients as
comma-separated text. `tools/dsp-capture.py` fetches, pretty-prints and diffs it.

The UI advertises a graph export, but the prior bench request to `/getDSP` returned
HTTP 500. Availability and completeness of an export on the actual ZP5S remain open;
see [hardware gate H4](hardware/AUDIO-SAFETY-INVARIANTS.md#what-we-still-do-not-know--gates-before-any-audio-test)
for the exact recon citation. No coefficient capture is claimed here.

## What this means for Moggie

1. `sonosd` needs a real biquad engine — highpass, lowpass, shelf, allpass — not just
   the gain/filter-level ioctls it exposes today.
2. Volume must become a dB curve with clamping, matching `calculateVolume`, or the
   speaker will behave nothing like stock at low levels.
3. The amp rail/fault states must be honoured. They are protection, not features.
4. The filter graph should be **loaded from configuration**, mirroring the
   `iirblock`/`getDSP` model, so the captured stock parameters can be dropped in.

## Still unknown

- The actual coefficient values and a working, verified capture route (gate H4).
- The exact dB↔percent curve. The strings show the computation happens; the table is
  not in the strings.
- Whether the DSP graph differs between 16.6, 34.16 and 57.10. The binaries grow by
  1.6 MB across those releases, so something changed, but not necessarily the audio.
- Anything about acoustic output. No measurement exists.

## Amplifier control and protection

The stock firmware does not just send audio to an amplifier — it runs a state
machine around it. Strings recovered from `anacapad` (57.10) name the inputs:

```
manageAmpState_notifyPreemptiveTurnOn
manageAmpState_notifyPlaying
manageAmpState_notifyPlayingUnmuteableSource_p
manageAmpState_notifyPlayingUnmuteableSource_np
manageAmpState_notifyVolume
manageAmpState_notifyHeadphoneMode
manageAmpState_notifyOutputFixed
```

Read together, that describes:

- **Pre-emptive turn-on** — the amp is woken before audio is expected, so the
  first moments of a track are not clipped while it settles.
- **Unmuteable sources** — some inputs (`_p` / `_np`, presumably pauseable and
  not-pauseable) cannot be paused, so pause becomes mute. The binary says so:
  `executing un-mute instead of pause`.
- **Fixed vs variable output** — the amp is bypassed or held at a fixed level
  when the speaker is used as a line-out.
- **Headphone mode** — a separate output path.

### Supply rails

Three states appear: `AMP_OFF`, `AMP_LO_RAIL`, `AMP_HI_RAIL`, alongside
`AmpOffTimerSec` and `ampTimer`.

These names do not establish low/high rail wiring, thresholds or hysteresis on
ZP5S. The stock ZPS5 worker changes C20 as part of mute/enable sequencing, while the
generic amp-power MIO has no ZPS5 pin. See the stock ledger E04/E06/E11 for exact
code locations. Its userspace amp-off timer defaults to 120 seconds in the older
`34.16-37101-1-16` package and is configurable (E07); physical rail discharge remains
unmeasured.

### Fault handling

```
failed to mute amps (%d)
failed to unmute amps (%d)
failed to unmute amps to clear fault (%d)
AMP_FAULT_STATUS
AMP_FAULT_WARN_STATUS
AMP_CURRENT_FAULT_STATUS
AMP_CURRENT_WARN_STATUS
```

These sibling strings identify a fault-clear path but do not prove its model,
ordering, retry bounds or safety on ZP5S (ledger E11). The recovered ZPS5 clock-unlock
path reinitializes the chip and resets current mute state (E10). The proposed Moggie
safety specification requires zero automatic unmute-to-clear attempts (invariant 13).

### Thermal

```
HalfPowerOverTemp
HalfPower Over Temp Pri
HalfPower Over Temp Sec
AUDIO_FAULT_TEMPERATURE_STATUS
AUDIO_WARN_TEMPERATURE_STATUS
CPU_FAULT_TEMPERATURE_STATUS
CPU_WARN_TEMPERATURE_STATUS
CPU2_FAULT_TEMPERATURE_STATUS
CPU2_WARN_TEMPERATURE_STATUS
```

These names suggest graded thermal handling; they do not establish a ZP5S
half-power gain or the meaning of primary/secondary (ledger E11). The older ZP5S
userspace warning path logs 75% while setting an internal level to 73 (E08).
Its packaged module uses raw-counter thresholds, whereas the later GPL source
converts to degrees before comparison (E12). Exact installed-build thresholds,
derating and sensor coverage must be established before custom output (gate H5).

### What this means for Moggie

1. Implement the verified board's enable/mute sequence and supply protection;
   do not infer rail switching from shared labels (invariants 1–7).
2. Bound idle power-down and confirm actual off state (invariant 8).
3. Keep faults latched and recovery bounded (invariants 12–13).
4. Require verified thermal coverage/derating and a separate safety mute
   (invariants 10–11).

The [implementation and proof map](hardware/AUDIO-SAFETY-INVARIANTS.md#moggie-implementation-and-proof-map)
names the current code gaps and evidence required. These are requirements, not
implemented interlocks.

### Still unknown

- The actual board/build's temperature calibration, derating and supply behavior.
- Whether `Pri`/`Sec` mean two channels or two stages. The strings do not say.
- Whether the P34 default `AmpOffTimerSec=120` matches the installed build (E07/H1).
