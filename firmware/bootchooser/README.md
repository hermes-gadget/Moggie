# firmware/bootchooser

`init` is the `/init` of the initramfs in mtd2, run as PID 1 by a kernel that
U-Boot loaded from mtd4. It decides which half of mtd3 to boot, and it is the
only thing that ever decides that — the bootloader environment is never touched.

```
U-Boot (mtd0, never written)
  └─ kernel (mtd4)
       └─ initramfs (mtd2)   ← this script
            └─ rootfs_a or rootfs_b (mtd3, two 8 MB slots)
                 └─ switch_root
```

## Boot flow

1. Mount `/proc`, `/sys`, `/dev` and mtd1 (jffs2) at `/jffs`.
2. Read `/jffs/bootslot` (`a`/`b`; missing or nonsense → `a`, and write it back).
3. If `/jffs/boot_ok` names that slot, the previous boot of it worked: clear
   `bootfail.<slot>`, set `slot_status.<slot>=ok`, truncate `boot_ok`.
4. Try that slot, then the other one. A slot is *usable* if:
   - its `slot_status` is `ok` (or the file is absent, as on a stock/factory slot),
   - its first four bytes are the squashfs magic `hsqs`,
   - its first `slot_size` bytes hash to `slot_sha256`.
5. If the slot already has `FAIL_LIMIT` (2) recorded failed boots, skip it —
   this is the fallback.
6. Otherwise count the attempt (`bootfail.<slot> = n+1`, written and synced
   **before** handing over, so a hang or panic is counted), mount it read-only
   and `exec switch_root`.
7. Both slots failed → recovery shell on UART, plus `telnetd` if the image has it.

## Failure counting, worked through

Nothing writes `boot_ok`:

| boot | `bootfail.a` at start | outcome |
|---|---|---|
| 1 | 0 | attempt 1 → boots a; counter 1 |
| 2 | 1 | attempt 2 → boots a; counter 2 |
| 3 | 2 | 2 ≥ limit → slot a marked bad, **boots b** |

So two failed attempts are tolerated before the chooser falls back. `boot_ok`
resets the counter and gives the slot a fresh budget: a slot that keeps checking
in never accumulates failures, and a slot that stops checking in is gone two
boots later.

When the chooser boots a slot other than the one in `bootslot` it rewrites
`bootslot` first — userspace reads that file to know which slot it is before it
writes `boot_ok`.

The 60 s deadline itself lives in userspace (`sonosd` writes `boot_ok` after its
self-check). The chooser only enforces the consequence: no `boot_ok` by the next
boot, no credit.

## State files

| file | meaning |
|---|---|
| `bootslot` | slot to try next |
| `boot_ok` | slot that booted successfully; consumed at the next boot |
| `bootfail.<slot>` | consecutive unconfirmed boots |
| `slot_status.<slot>` | `ok` / `writing` / `bad` — anything but `ok` blocks the slot |
| `slot_sha256.<slot>`, `slot_size.<slot>` | what the slot must contain |

`slot_status=writing` is what an interrupted update leaves behind, so a
half-written slot can never be booted even if `bootslot` somehow pointed at it.

## Shell

Plain POSIX `sh`: no bashisms, no `local`, no arrays. It is tested under `dash`
and `busybox ash`. `sha256sum`, `dd`, `head -c`, `cut`, `mount` and
`switch_root` come from busybox; `awk` is not used.

## Simulation mode

The test suite has no NAND and no initramfs, so the chooser also runs against
files:

```sh
MOGGIE_SIM=1 MOGGIE_SLOT_DIR=DIR MOGGIE_STATE_DIR=DIR sh firmware/bootchooser/init
```

- slots are `DIR/slot_a.img` and `DIR/slot_b.img` instead of `/dev/mtdblockN`
- `switch_root` is replaced by writing the chosen slot to `DIR/booted` and
  exiting 0 (or running `MOGGIE_SIM_BOOT_HOOK` with the slot as `$1`)
- recovery exits 3 instead of spawning a shell

Exit status in simulation: `0` a slot was chosen, `3` both failed.

## Partition names

The chooser resolves `rootfs_a` / `rootfs_b` through `/proc/mtd`, which requires
`CONFIG_MTD_CMDLINE_PARTS` and a cmdline that splits mtd3:

```
mtdparts=...:16m(rootfs),8m(rootfs_a),8m(rootfs_b)
```

If the names are not there, the slot is reported as missing and the chooser
falls through to the other slot and then to recovery. It does not guess offsets:
mounting the second half of a device needs a partition (or a loop device), and
loop devices are not worth carrying in an initramfs.

## Not done yet

- The real-device path (`/proc/mtd` lookup, `mount -t squashfs`, `switch_root`)
  has not run on hardware; only the simulation path is exercised by CI.
- Hashing 7.5 MiB of NAND on a 300 MHz CPU is a real cost at every boot
  (`MOGGIE_HASH_CHECK=0` skips it and leaves the magic check in place). It has
  not been timed on a speaker yet.
- No image roll-forward: recovery means re-flashing a slot with `moggie-updater`.
