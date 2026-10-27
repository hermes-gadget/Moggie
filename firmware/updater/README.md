# firmware/updater

`moggie-updater` applies a signed OTA image to the **inactive** half of mtd3.

Two binaries come out of one set of sources:

| binary | build | writes | key |
|---|---|---|---|
| `moggie-updater` | release | `/dev/mtd3` only, via `nandwrite` | compiled in (`pubkey_dev.h`) |
| `moggie-updater-test` | `-DMOGGIE_TEST_MODE` | a regular file, with `--target` | `--pubkey-txt` allowed |

```
make                      # both, into ./build
make PUBKEY_HEADER=/abs/path/pubkey.h    # embed a real key
```

The release binary refuses a regular file, refuses `--test-mode`, and refuses any
device that is not `/dev/mtd3` or `/dev/mtdblock3` — so it cannot be talked into
writing mtd0.

## Usage

```
moggie-updater --manifest manifest.json --image rootfs.squashfs [options]

  -m, --manifest FILE   signed manifest.json
  -i, --image FILE      rootfs squashfs to install
  -d, --device PATH     rootfs MTD device (default /dev/mtd3)
  -s, --state-dir DIR   where bootslot/boot_ok live (default /jffs)
      --slot a|b|auto   target slot; auto = the inactive one (default)
      --check           verify only, write nothing
      --no-reboot       do not reboot after a successful install
  -v, --verbose
```

Exit codes: `0` ok · `1` usage · `2` manifest · `3` image · `4` device ·
`5` read-back · `6` state · `7` write failure.

`--check` is what the Home Assistant `update` entity and the installer call: it
verifies the signature, the model, the size, the hash and the squashfs magic
without touching any device.

## Order of operations

Each step only starts if the previous one passed.

1. **Verify the manifest signature** against the compiled-in RSA-2048 key.
2. **Check the manifest metadata**: `model == 8`, `0 < size <= 7,864,320`.
3. **Check the image**: regular file, size exactly `manifest.size`, first four
   bytes `hsqs`. All of this happens before a byte is written.
4. **Pick the target**: the slot that `/jffs/bootslot` does *not* name. Asking
   for the running slot is an error, not a no-op.
5. **Mark the target `writing`** in `/jffs`, so a crash leaves an obviously
   untrustworthy slot behind.
6. **Stream the image** into the slot with `nandwrite`, 64 KiB at a time,
   hashing as it goes. The image is never buffered: a 8 MB image plus a
   decompressor does not fit in 32 MB of RAM.
7. **Compare the streamed hash** with the manifest; a mismatch aborts and marks
   the slot bad.
8. **Read the slot back** with `nanddump`, hash the same number of bytes, and
   compare again. Only this read-back makes a slot bootable.
9. **Commit**: write `slot_size.<slot>`, `slot_sha256.<slot>`, `slot_version.<slot>`,
   `slot_status.<slot> = ok`, and *then* `/jffs/bootslot`. Each is an
   fsync-then-rename, and `bootslot` is last, so a power cut at any point leaves
   either the old boot slot or a fully verified new one.
10. **`sync` and reboot** (unless `--no-reboot`).

## Manifest format

One line of JSON and a newline:

```json
{"version":"1.0.0","model":8,"size":4123456,"sha256":"<64 hex>","signature":"<base64>"}
```

The signature is RSA-2048 / PKCS#1 v1.5 over SHA-256 of **everything except the
signature member** — that is, of the canonical serialisation of the other four
fields, in that order, with no spaces:

```
{"version":..,"model":..,"size":..,"sha256":..}
```

Because the signed bytes are defined *positionally*, the verifier does not need
a JSON canonicaliser or a general JSON parser. It:

1. requires the file to end with `,"signature":"<base64>"}`,
2. hashes every byte before the comma,
3. parses the four fields strictly (no duplicates, no unknown keys, no
   escapes, no leading zeros),
4. re-serialises them canonically and requires a **byte-for-byte match** with
   the bytes it hashed,
5. RSA-verifies the signature over those bytes.

Step 4 is what stops a manifest from being signed in one shape and read in
another. A validly signed but re-ordered body is rejected, and so is a body with
a duplicated field — both are covered by the test suite.

`nandwrite` pads the tail of the slot to the erase boundary with `0xFF`, so the
read-back compares exactly `manifest.size` bytes, not the whole slot.

## State files in `/jffs`

| file | written by | meaning |
|---|---|---|
| `bootslot` | updater, boot chooser | slot to try next: `a` or `b` |
| `boot_ok` | userspace, within 60 s of a good boot | slot that just booted successfully |
| `slot_status.<slot>` | updater, chooser | `ok` / `writing` / `bad` |
| `slot_sha256.<slot>` | updater, after read-back | hash of the slot image |
| `slot_size.<slot>` | updater, after read-back | image length, for the chooser's hash check |
| `slot_version.<slot>` | updater | version string, for diagnostics |
| `bootfail.<slot>` | boot chooser | consecutive unconfirmed boots |

## Test mode

`moggie-updater-test` takes `--test-mode --target FILE`, where `FILE` is a
16 MiB regular file standing in for mtd3 (slot a at 0, slot b at 8 MiB), plus
`--pubkey-txt FILE` to load a throwaway key as `n=<hex>` / `e=<decimal>`. The
write, hash and read-back code paths are the same as production; only the
transport differs (`pwrite`/`pread` instead of `nandwrite`/`nanddump`).

Three environment knobs exist only in that build, and only to let the tests
provoke failures that hardware would otherwise have to provide:

| variable | effect |
|---|---|
| `MOGGIE_TEST_WRITE_DELAY_MS` | sleep this long between chunks, so a test can `kill -9` mid-write |
| `MOGGIE_TEST_CHUNK_BYTES` | chunk size, 512..65536 |
| `MOGGIE_TEST_READBACK_CORRUPT` | flip a bit in the read-back stream (bad NAND block) |

## Not done yet

- The `nandwrite`/`nanddump` path has **never been executed on hardware**. It is
  written against the mtd-utils CLI and compiled in both builds, but until Phase 7
  the only exercised backend is the regular-file one.
- No resume after a failed download; the whole image must be present locally.
- `reboot` is `execlp("reboot")` and is untested (tests pass `--no-reboot`).
- No retry on a transient NAND write error; the slot is marked bad instead.
