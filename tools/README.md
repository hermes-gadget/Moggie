# tools/

Host-side tooling. Python 3 standard library plus the `openssl` CLI — nothing to
install.

| | |
|---|---|
| `mkmanifest.py` | generate a signing key, build and sign an OTA manifest |
| `verify.py` | verify a manifest (+ image) against a public key |
| `otamanifest.py` | the shared format: canonical bytes, signing, verification |
| `ha/` | Home Assistant discovery: payload generator, publisher, docker rig ([docs/HA.md](../docs/HA.md)) |
| `installer/` | the network loader (kept out of this repo for now) |

## Signing

```sh
# once, and keep the private key off the build host if you can
tools/mkmanifest.py --keygen --key-dir keys/

# per release
tools/mkmanifest.py --image out/rootfs.squashfs --version 1.0.0 \
                    --key keys/ota_signing.key --out out/manifest.json \
                    --pubkey keys/ota_signing.pub.pem \
                    --emit-c-header out/pubkey.h
```

`--emit-c-header` writes the compile-time key the firmware verifies against;
build the updater with `make PUBKEY_HEADER=/abs/path/out/pubkey.h`. The header
checked in as `firmware/updater/pubkey_dev.h` is a **placeholder** whose private
half was discarded on purpose — releases must replace it.

Images are refused if they exceed 7,864,320 bytes (7.5 MiB, half of mtd3).
`--force-oversize` exists only so the negative tests can prove the device
rejects a correctly signed oversize manifest.

## Verifying

```sh
tools/verify.py --manifest out/manifest.json --image out/rootfs.squashfs \
                --pubkey keys/ota_signing.pub.pem

tools/verify.py --manifest out/manifest.json --pubkey keys/ota_signing.pub.pem --no-image
```

Exit `0` verified, `1` verification failure, `2` usage or I/O error.

This is the reference implementation the C verifier is tested against: it is
deliberately stricter than "valid JSON", because the signed bytes are defined
positionally. A manifest that is valid JSON but not byte-identical to the
canonical form is rejected.

## Self-test

```sh
tools/mkmanifest.py --selftest
```

Generates a throwaway key, signs a sample image, and checks that a flipped
signature bit, a tampered field, non-canonical whitespace, the wrong model, an
oversize image, a truncated image and a corrupted image are all refused.
No network, no repo state, cleans up after itself.
