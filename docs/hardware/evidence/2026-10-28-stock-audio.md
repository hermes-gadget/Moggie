# Stock audio safety evidence — 2026-10-28

Offline analysis only. Addresses below describe files, not measurements on a
speaker. Do not execute the recovered register writes on hardware from this note.

## Source identities and address conventions

`A` is `opt/bin/anacapad` and `K` is `modules/audiodev.o`, extracted from
`/home/ben/Moggie/dl/stock-firmware/34.16-37101-1-16.upd` (`P34`). The extraction
below establishes their provenance. `A` is a stripped PPC32 big-endian executable;
its `.text` and `.rodata` virtual addresses equal file offsets plus `0x10000000`
(`readelf -SW A`, sections 11 and 13). `K` is relocatable and retains function
symbols; all `K` instruction addresses here are **offsets in `.text`**, whose file
offset is `0x34` (`readelf -SW K`, section 1). Include relocations when disassembling
`K`: otherwise external call names and jump-table targets are misleading.

`G` means `/home/ben/sonos-bench/gpl/drivers/audiodev-connectx/`. These GPL sources
name the ZPS5 branches and explain the corresponding instructions in `K`; they are
not asserted to be the exact source revision used for `P34`. `M` means
`/home/ben/sonos-bench/sonos-fenway/includes/mdp.h`: lines 69, 159 and 357–359 name
submodel 4 as `ZPS5`, and distinguish it from Wembley and Eden. The bench identity
is independently recorded in
`/home/ben/.hermes/plans/2026-10-28-moggie-recon-output/MODEL-IDENTITY-CAVEAT.md:12–18`
as ZP5S/MPC8272, software 57.10-25140. **The old package is a family reference,
not proof of the currently installed build or board revision.**

| ID | Local input or extracted file | Bytes | SHA-256 |
|---|---|---:|---|
| P34 | `/home/ben/Moggie/dl/stock-firmware/34.16-37101-1-16.upd` | 4727312 | `84632e02bdce626bf10ae846839e90d66e510a627320fff8690c2bdb998733f2` |
| R34 | `out/audio-safety/34.16-37101-1-16.squashfs` | 4014084 | `d0536a934f15a2a7cdef4509d469b2a98492f34539b6583e05724cd97099d59d` |
| A | `out/audio-safety/zp5s-34.16/opt/bin/anacapad` | 4101056 | `9db3247f232fa7e2dd1a42561782d38b95c6c4905029f16b22293471d6f23870` |
| K | `out/audio-safety/zp5s-34.16/modules/audiodev.o` | 76456 | `181a848a7613d75ea3016ab1103feb23ed14661e305340fea89609db5cf0140f` |
| P57 | `/home/ben/sonos-bench-analysis/zp5s/57.23-74170-1-16.upd` | 5606749 | `e19a7bb28f2d940346c605ccfcff81f8d9a7dd1771ee991841ac1f9e07223ad4` |
| F57 | `/home/ben/sonos-bench/sonos-fenway/firmware/rootfs/57.10-25140/opt/bin/anacapad` | 5636472 | `2a0e6adb831d995a2863f4307077e2c14c59afc52a43815865f0bc09f8580962` |

`F57` is the **Fenway sibling**, not a ZP5S capture (identity caveat above).
In `P57`, the rootfs record at file `0x0fca7b` has type 4, total length 4571498,
flags 4, and begins `88 64 99 ca` at payload `0x0fca87`; it is not the plaintext
SquashFS used below. No decryption or analysis of its userspace is claimed.

## Reproduce the offline extraction

Run from the repository root. All outputs stay in ignored `out/audio-safety/`;
no target device or original input is written. The package's record length includes
the 12-byte record header. Thus `P34` record 4 starts at `0xadd43`, has total length
4014096, and its filesystem payload starts at `0xadd4f`, length **4014084**.

```sh
mkdir -p out/audio-safety
python3 - <<'PY'
import hashlib, struct
from pathlib import Path
p = Path('/home/ben/Moggie/dl/stock-firmware/34.16-37101-1-16.upd')
d = p.read_bytes()
assert hashlib.sha256(d).hexdigest() == '84632e02bdce626bf10ae846839e90d66e510a627320fff8690c2bdb998733f2'
assert d[:4] == bytes.fromhex('497f1635')
o = 4
while o + 12 <= len(d):
    kind, length, flags = struct.unpack_from('<III', d, o)
    assert length >= 12 and o + length <= len(d)
    if kind == 4:
        assert (o, length, flags) == (0xadd43, 4014096, 0)
        payload = d[o + 12:o + length]
        assert payload[:4] == b'sqsh'
        Path('out/audio-safety/34.16-37101-1-16.squashfs').write_bytes(payload)
        break
    o += length
else:
    raise AssertionError('rootfs record missing')
PY
unsquashfs -s out/audio-safety/34.16-37101-1-16.squashfs
unsquashfs -f -no-xattrs -d out/audio-safety/zp5s-34.16 \
  out/audio-safety/34.16-37101-1-16.squashfs \
  opt/bin/anacapad modules/audiodev.o etc/Configure etc/inittab \
  etc/runanacapa opt/conf/anacapa.conf
```

Observed: `Found a valid big endian SQUASHFS 3:0 superblock`; targeted extraction
created regular files and directories, **0 devices**. `-f` permits rerunning into
this analysis directory. Binaries and full disassembly remain ignored local evidence,
not redistributed source files.

The disassembler used was
`/home/ben/sonos-bench/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin/powerpc-linux-objdump`:

```sh
readelf -SW out/audio-safety/zp5s-34.16/opt/bin/anacapad
readelf -SW out/audio-safety/zp5s-34.16/modules/audiodev.o
readelf -rW out/audio-safety/zp5s-34.16/modules/audiodev.o
/home/ben/sonos-bench/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin/powerpc-linux-objdump \
  -dr out/audio-safety/zp5s-34.16/opt/bin/anacapad > out/audio-safety/anacapad-34.16.dis
/home/ben/sonos-bench/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin/powerpc-linux-objdump \
  -dr out/audio-safety/zp5s-34.16/modules/audiodev.o > out/audio-safety/audiodev-34.16.dis
```

## Evidence index

**Observed** means literal bytes/instructions/source. **Inference** names the
interpretation; it does not establish physical pin wiring or elapsed wall time.

| Ref | Finding and exact evidence |
|---|---|
| E01 | **Observed startup order:** extracted `etc/Configure:32–33` loads `chk_driver.o` then `audiodev.o`. `K:.text+0x36fc` calls `worker_thread_init`. `G/workerthread.c:48–56` initializes both current/requested amp and headphone mute to 1, then calls `initialize()`; corroborated by `K:0x8b10–0x8b3c`. |
| E02 | **Observed CS44800 initialization:** `G/workerthread.c:99–151`, `K:0x8800–0x89b8`: configure/clear port C pin 20, configure/set C25; disable audio clock handling and temperature measurement; set C10; reset A11 low, wait 1500 µs, high, wait 1500 µs; program registers in the order listed below; clear C10, wait 10 ms; enable clock handling/temperature measurement. Source delay values are corroborated by the repeated calibrated `__delay` calls, not a measured waveform. |
| E03 | **Observed lock gate:** `G/workerthread.c:153–179`, `K:0x89c0–0x8a80`: read register `0x2a`, require bit `0x40`; up to five polls per reset attempt, with `schedule_timeout(100)` in this binary (`HZ` in source). On failure write `0x02=0x01` and retry initialization. On lock, write `0x29=0x80`, wait 1000 ticks (`10*HZ` in source), write `0x03=0xe0`, wait 100 ticks (`HZ`), then `0x05=0x08`, `0x03=0x20`. **The outer reset loop is unbounded; I²C return values here are not checked.** |
| E04 | **Observed mute-down:** `G/workerthread.c:212–219`, `K:0x8b78–0x8bc8`: when amp mute changes 0→1, write `0x13=0xff` if headphone muted, else `0x3f`; clear C20; wait 20 ticks (`HZ/5` in source). **Observed unmute:** `G/workerthread.c:222–228`, `K:0x8e04–0x8e30`: on 1→0 set C20, wait 33 ticks (`HZ/3`), then write `0x13=0xe0`. Interpretation: C20 is part of the amp enable path. Which physical supply it controls needs board verification. |
| E05 | **Observed asynchronous API:** `G/audioctl.c:1093–1116` queues ZPS5 `bAmpMute` and wakes the worker, then caches `muteOn`. `K:0x6efc–0x6f84` branches on model 16/17, submodel 4; `0x7798–0x7804` updates/wakes the worker. Requested state is copied to current before physical work (`G/workerthread.c:206–210`, `K:0x8b4c–0x8b78`). A successful ioctl or cached state does **not** acknowledge settled GPIO/register output. |
| E06 | **Observed userspace request ordering:** `A:0x100663d8–0x10066518` uses stack values 1 and 0, requests `0x8004fc1c` (amp power 1) then `0x8004fc0c` (amp mute 0), except model-specific branches. Off path `A:0x10065ad0–0x10065d00` requests amp mute 1 before amp power 0. Names/numbers: `G/audioctl.h` definitions of `AUDIODEV_CTL_AMP_PWR` (28) and `AUDIODEV_CTL_AMP_MUTE` (12). **ZPS5 qualification:** `G/audioctl.c:79–98,1078–1084` gives generic amp-power MIO no ZPS5 pin; its cached power state cannot prove a physical rail. The ZPS5 worker in E04 provides the concrete C20 transition. |
| E07 | **Observed idle policy:** `A:0x10065e74–0x10065e80` and `0x10065f1c–0x10065f28` initialize amp manager field `+0x138` to 120. `AmpOffTimerSec` string is at file `0x31dda4`, referenced at `A:0x10058fd8–0x10058fec`; its setter `0x10059240–0x10059278` calls `0x10066208`, which stores the parsed value to `+0x138` at `0x10066234`. `A:0x100662a0–0x100662b4` forms `time(NULL)+field`; `0x10066358–0x100663a4` waits for the deadline and rechecks eligibility before calling the off routine at `0x100663c8`. Thus **120 seconds is a default in P34, configurable, not a universal measured timeout**. |
| E08 | **Observed thermal/fault distinction in userspace:** `A:0x10056884–0x100568b4` applies warning mask `0x010a000a` and fault mask `0x00f50005`; `0x100568c4–0x10056920` latches/reports entry to hardware fault state. The warning clamp path compares a stored level with 73 and sets 73 (`0x10056d9c–0x10056db8`), while its message says “75%” (file `0x31eb3c`, xref `0x10056f1c–0x10056f30`). **Do not interpret 73 or 75% as output power or a measured gain.** |
| E09 | **Observed model-specific protection:** `G/audioctl.c:644–649` disables temperature acquisition initially for ZPS5/Eden and seeds temperature values; this is **not permanent disable**: `G/workerthread.c:150–151` calls `audioctl_tm_enable`, whose `G/audioctl.c:275–280` clears the flag, corroborated by `K:0x89b8,0x4b7c–0x4b84`; `:884–905` uses validity bits before CPU threshold checks, but the audio/CPU2 hard mute + 36 V shutdown path is Wembley-only. `:747–751` installs ZPS5 amp interrupt, whereas UV14 IRQ is Wembley-only at `:742–746`. `K:0x5db4–0x5df8` latches UV14 (`0x00400000`) and calls MIO mute; this does not prove the ZPS5 branch can receive that IRQ. **Generic protection names do not prove board coverage.** |
| E10 | **Observed clock fault recovery:** `G/workerthread.c:190–203` describes read `0x2a`, bit `0x80` (`SRC_UNLOCK`), followed by full initialization and resetting current state to muted. `K:0x8cec–0x8d64` actually tests the request flag, reads the register and reinitializes. **Source caveat:** the published `G` source tests local `req.bInterrupt` before assigning `req` at line 207; do not copy this source verbatim or claim it exactly matches K. |
| E11 | **Observed sibling evidence only:** `F57` strings include `HalfPowerOverTemp` (file `0x4b0a0c`), failed mute/unmute and fault-clear diagnostics (`0x4763e0,0x4765b0,0x47664c`), and `AMP_HI_RAIL`/`AMP_LO_RAIL`/`AMP_OFF` (`0x4b3e84,0x4b3e90,0x4b3e9c`). These do not establish ZPS5 low/high rail control, a half-power coefficient, recovery retry bounds, or thermal thresholds. `docs/AUDIO.md:146–194` now distinguishes these names from verified behavior. E04/E09 provide the narrower, model-specific evidence. |
| E12 | **Observed thermal source drift:** `K:0x5514–0x551c` skips acquisition while disabled; `0x554c–0x5584,0x5790–0x57c0` updates/clamps a counter at `audioctlpriv+0x38` and sends it to PWM. Once validity bits are both set (`0x4c40–0x4c4c`), `0x4db0–0x4dc8` compares that same counter with **44 and 49**, setting CPU fault for `<=44` (branch `0x4fac`) and CPU warning for `45..49`. These are counter units, **not degrees Celsius**. In contrast, `G/audioctl.c:303–312` converts raw values to temperatures, then `:884–887` compares them with 75/80°C constants from `G/audioctl.h:282–287`. The source and packaged module therefore cannot be treated as interchangeable threshold implementations; board calibration and installed-build matching are mandatory. |

CS44800 initialization register writes, in observed order (E02; values hexadecimal):

```text
04=c0 31=04 33=0d 06=00 07=00 13=ff 02=01 05=1b 02=00
wait 1 ms
5a=55 43=20 5a=00 07=01 08=01
wait 1 ms; clear C10; wait 10 ms; enable clock/temperature handling
```

These are recovered transactions, not a complete DSP graph or a register programming
recommendation. The exact channel mapping, electrical interpretation and protection
limits remain hardware gates in the [invariants](../AUDIO-SAFETY-INVARIANTS.md).

## GPL source fingerprints

SHA-256 for the source revisions cited above (paths relative to `G`, except `M`):

```text
deb3c9617b83e0645b830b62012610247650c2278283a6aa9afcd0dd1f56ca87  audioctl.c
dedbd2626078936f46783f1e2293cee0800f54c3b711da9119c53cc7ee11cef7  audioctl.h
60a9df551fea570f694775d974668a603aed0c6192c2708c0ac907a2d55a5259  workerthread.c
ba836110a3971130f673c0710ad2cd2f44f80e8e1ca03f8f1e214f87cc81b41f  workerthread.h
04555f77843addca1731e2afe4fec83336c03497a5bbfe6c53c4cfa9fa2dbfb0  M (mdp.h)
```

## Offline integrity check

After extraction, run this Python block from the repository root. It verifies the
input hashes, selected literal PPC instructions/strings behind the principal
findings, and document coverage/links. It **does not** test runtime safety, infer
physical wiring, or establish exact correspondence between GPL source and module.
The full addresses in the index still require disassembly review.

```python
from pathlib import Path
import hashlib
import re
import struct

ledger = Path('docs/hardware/evidence/2026-10-28-stock-audio.md')
body = ledger.read_text()
inputs = {}
for ident, path, size, digest in re.findall(
    r'^\| (\w+) \| `([^`]+)` \| (\d+) \| `([0-9a-f]{64})` \|$', body, re.M
):
    data = Path(path).read_bytes()
    assert len(data) == int(size), ident
    assert hashlib.sha256(data).hexdigest() == digest, ident
    inputs[ident] = data
assert set(inputs) == {'P34', 'R34', 'A', 'K', 'P57', 'F57'}
gpl = Path('/home/ben/sonos-bench/gpl/drivers/audiodev-connectx')
for digest, name in re.findall(r'^([0-9a-f]{64})  (.+)$', body, re.M):
    path = (Path('/home/ben/sonos-bench/sonos-fenway/includes/mdp.h')
            if name == 'M (mdp.h)' else gpl / name)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name

def word(ident, addr, expected):
    offset = addr - 0x10000000 if ident == 'A' else addr + 0x34
    assert inputs[ident][offset:offset+4].hex() == expected, (ident, hex(addr))

# A: default 120, store +0x138, configurable setter, warning/fault masks, clamp 73.
for addr, expected in {
    0x10065e74: '38000078', 0x10065e80: '901d0138',
    0x10065f1c: '38000078', 0x10065f28: '901d0138',
    0x10066234: '939d0138', 0x10056888: '3c00010a',
    0x1005688c: '6000000a', 0x100568a8: '3d2000f5',
    0x100568ac: '61290005', 0x10056db4: '38c00049',
}.items():
    word('A', addr, expected)
# K: reset/register mute, lock mask, post-lock waits, mute/enable waits,
# SRC-unlock mask, thermal raw-counter warning/fault comparisons.
for addr, expected in {
    0x8834: '38800019', 0x88f8: '388000ff', 0x88fc: '38600013',
    0x89d0: '70090040', 0x89e0: '38600064', 0x8a48: '386003e8',
    0x8a64: '38600064', 0x8bc4: '38600014', 0x8e08: '38800014',
    0x8e0c: '38a00004', 0x8e20: '38600021', 0x8e28: '388000e0',
    0x8e2c: '38600013', 0x8d0c: '70090080',
    0x4db4: '2f80002c', 0x4dbc: '2f800031',
}.items():
    word('K', addr, expected)
for ident, offset, literal in [
    ('A', 0x31dda4, b'AmpOffTimerSec'),
    ('A', 0x31eb3c, b'Hardware warning; clamping volume to 75%%'),
    ('F57', 0x47664c, b'failed to unmute amps to clear fault (%d)'),
    ('F57', 0x4b0a0c, b'HalfPowerOverTemp'),
]:
    assert inputs[ident][offset:offset+len(literal)] == literal
assert struct.unpack_from('<III', inputs['P57'], 0xfca7b) == (4, 4571498, 4)
assert inputs['P57'][0xfca87:0xfca8b] == bytes.fromhex('886499ca')

spec = Path('docs/hardware/AUDIO-SAFETY-INVARIANTS.md')
text = spec.read_text()
assert [int(n) for n in re.findall(r'^(\d+)\. \*\*', text, re.M)] == list(range(1, 16))
assert [int(n) for n in re.findall(r'^\| (\d+) \|', text, re.M)] == list(range(1, 16))
assert set(re.findall(r'^\| (H\d+) —', text, re.M)) == {f'H{i}' for i in range(1, 8)}
assert set(re.findall(r'^\| (E\d+) \|', body, re.M)) == {f'E{i:02}' for i in range(1, 13)}
for doc in (ledger, spec, Path('docs/AUDIO.md')):
    for target in re.findall(r'\]\(([^)]+)\)', doc.read_text()):
        if not target.startswith(('https:', 'http:', '#')):
            assert (doc.parent / target.split('#')[0]).is_file(), (doc, target)
print('PASS: 6 input hashes, 5 source hashes, 26 PPC words, strings/records; 15 invariants + maps, 7 gates, 12 evidence rows, local links')
```
