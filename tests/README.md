# tests/

- `ota/` — negative tests for the update path. Every case must fail safely.
- `ha/` — MQTT discovery payload validation. Stdlib only, pytest-compatible.
- `../firmware/sonosd/tests/` — sonosd unit tests (arbiter, volume, jsonlite, config).

Run with `make ota-test` and `make ha-test`, and `make test` in `firmware/sonosd`.

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
