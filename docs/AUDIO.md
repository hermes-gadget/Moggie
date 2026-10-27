# Audio behaviour of the original Play:5 Gen 1

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
Moggie's `sonosd` currently has a linear 0–100 volume with no dB mapping and no clamp.

## The DSP configuration is readable over HTTP

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

**This is the difference between guessing and knowing.** On hardware day, one `GET
/getDSP` yields the exact crossover, shelf, notch and limiter parameters the speaker
ships with — no coefficient reverse-engineering required.

Not yet known: whether `/getDSP` itself is access-controlled on 57.10, or only the HTML
page. That is a two-minute check with a real unit.

## What this means for Moggie

1. `sonosd` needs a real biquad engine — highpass, lowpass, shelf, allpass — not just
   the gain/filter-level ioctls it exposes today.
2. Volume must become a dB curve with clamping, matching `calculateVolume`, or the
   speaker will behave nothing like stock at low levels.
3. The amp rail/fault states must be honoured. They are protection, not features.
4. The filter graph should be **loaded from configuration**, mirroring the
   `iirblock`/`getDSP` model, so the captured stock parameters can be dropped in.

## Still unknown

- The actual coefficient values. Needs a hardware capture (`getDSP`).
- The exact dB↔percent curve. The strings show the computation happens; the table is
  not in the strings.
- Whether the DSP graph differs between 16.6, 34.16 and 57.10. The binaries grow by
  1.6 MB across those releases, so something changed, but not necessarily the audio.
- Anything about acoustic output. No measurement exists.
