# Hardware day

Everything here needs the speaker. Do not start until the UART is wired and the
builds in `docs/STATUS.md` still pass.

## 0. Before touching anything

- [ ] `make kernel` runs clean and the gzip payload is under 2 MB
- [ ] `make ota-test` passes 23/23
- [ ] The rootfs was built against the **2.6.35 toolchain**, and you can prove it:

```sh
readelf -n out/rootfs-extracted/lib/libc.so.6 | grep 'ABI:'   # -> Linux, ABI: 2.6.35
```

- [ ] Every ELF in the image carries that tag, and no binary uses a syscall the
      kernel lacks. A stale image built with the old glibc passes every other
      check here and then dies before `main()`:

```sh
python3 out/syscall-scan.py out/rootfs-extracted   # -> (0 distinct)
```
- [ ] A **second** machine is available to hold the dumps — never the speaker's own flash
- [ ] Power is stable. Not a bench supply that can be knocked out.
- [ ] You know how to get back: the UART is the recovery path, and it is the only one

## 1. Capture the stock state first

Nothing gets written until every partition is dumped and verified twice.

- [ ] Find the UART: J25004, **38400 8N1**, three wires (TX, RX, GND)
- [ ] Capture the boot log from cold, unmodified
- [ ] At the U-Boot prompt, record `printenv` verbatim — especially `bootcmd`,
      `bootargs`, `mtdparts`
- [ ] Boot stock, get a shell (`/unlock` POST or the debug port, whichever works)
- [ ] Dump every MTD partition:

```sh
for i in 0 1 2 3 4 5; do
    nanddump -f /tmp/mtd$i.bin /dev/mtd$i
    sha256sum /tmp/mtd$i.bin
done
```

- [ ] Pull all six files off the device
- [ ] Verify each one hashes the same on the receiving machine
- [ ] Store them somewhere that is not this VM and not the speaker

**mtd0 is the only unbrick path. If that dump does not exist, stop.**

## 2. First boot of our kernel — read-only

- [ ] Write the kernel to mtd4 only
- [ ] Boot it
- [ ] Confirm the UART shows our version string
- [ ] Confirm `audiodev` loads and `/dev/dsp`, `/dev/audioctl` appear
- [ ] Confirm `CONFIG_SQUASHFS` is present (`cat /proc/filesystems`)
- [ ] Play nothing yet

If it does not boot: rewrite the stock kernel from the dump. No other partition has
been touched.

## 3. Rootfs

- [ ] Write the squashfs to slot A
- [ ] Point the chooser at slot A
- [ ] Boot
- [ ] `sonosd` starts, publishes to MQTT, Home Assistant discovers it
- [ ] **`/jffs/boot_ok` exists before the first reboot.** The chooser charges a
      boot attempt before `switch_root` and clears it only when userspace writes
      that file. If it is missing, the third boot rolls back to the other slot
      and the speaker ping-pongs forever while looking perfectly healthy:

```sh
ls -l /jffs/boot_ok && cat /jffs/boot_ok
```

- [ ] `dlna-renderer` is running and Home Assistant's `dlna_dmr` integration
      offers a `media_player` for it (check the SSDP advertisement with
      `ssdp:alive` on port 1900)
- [ ] Check `free` — the daemon should sit around 20 MB with one decoder idle

## 4. Audio

- [ ] Play a test tone through `/dev/dsp`
- [ ] Set volume through `/dev/audioctl`, confirm the speaker responds
- [ ] AirPlay from a phone, once `shairport-sync` is cross-built
- [ ] Measure RSS with one active stream

## 5. OTA

- [ ] Sign a test image
- [ ] Install to slot B while running from A
- [ ] Confirm the read-back and the slot flip
- [ ] Reboot, confirm B
- [ ] Pull power mid-write on purpose, confirm A still boots
- [ ] Two failed boots → confirm the chooser falls back

## 6. Rollback

```sh
moggie --rollback
```

- [ ] Restores the stock images from the dumps
- [ ] Device boots stock
- [ ] Sonos firmware version matches what was captured in step 1

## If it bricks

The UART and mtd0 are the way back. If U-Boot is intact, the device is recoverable.
If mtd0 was written, it is not — which is why nothing in this project writes mtd0.
