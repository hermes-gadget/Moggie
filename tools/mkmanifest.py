#!/usr/bin/env python3
"""Build and sign a Moggie OTA manifest.

    # one-time: generate a signing key (keep the private half off the build host
    # when you can — it is chmod 0600 here and must never be committed)
    tools/mkmanifest.py --keygen --key-dir keys/

    # sign an image
    tools/mkmanifest.py --image out/rootfs.squashfs --version 1.0.0 \
                        --key keys/ota_signing.key --out out/manifest.json

    # sanity-check the toolchain itself, no network, no repo state
    tools/mkmanifest.py --selftest

The manifest format is documented in tools/otamanifest.py and in
firmware/updater/README.md.  Size is checked against the hard 7.5 MiB slot
budget; `--force-oversize` exists only so the negative tests can prove the
device rejects an oversize manifest that is correctly signed.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import otamanifest as om  # noqa: E402


def cmd_keygen(args) -> int:
    priv, pub = om.keygen(args.key_dir, bits=args.bits, force=args.force)
    print("private key  %s  (0600, never commit this)" % priv)
    print("public  key  %s" % pub)
    if args.pubkey_txt:
        with open(args.pubkey_txt, "w", encoding="ascii") as fh:
            fh.write(om.pubkey_txt(pub))
        print("pubkey txt   %s" % args.pubkey_txt)
    if args.emit_c_header:
        hdr = om.c_header(pub, comment=args.comment or "Moggie OTA public key")
        with open(args.emit_c_header, "w", encoding="ascii") as fh:
            fh.write(hdr)
        print("C header     %s" % args.emit_c_header)
    return 0


def cmd_build(args) -> int:
    raw = om.build_manifest(
        args.image,
        args.version,
        args.key,
        model=args.model,
        force_oversize=args.force_oversize,
    )
    out = args.out
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(raw)

    body, obj, sig = om.load_manifest(out)
    # Sign-then-verify: never hand back a manifest we cannot ourselves read.
    pub = args.pubkey or args.pubkey_out
    if pub:
        om.verify_body(body, sig, pub)
    print("manifest     %s" % out)
    print("  version    %s" % obj["version"])
    print("  model      %d" % obj["model"])
    print("  size       %d bytes (%.2f MiB, budget %d)" % (
        obj["size"], obj["size"] / 1048576.0, om.MAX_IMAGE_BYTES))
    print("  sha256     %s" % obj["sha256"])
    print("  signature  %d bytes RSA-2048/SHA-256" % len(sig))

    if args.pubkey_out or args.emit_c_header or args.pubkey_txt:
        pub = args.pubkey or args.pubkey_out
        if not pub:
            print("error: --emit-c-header/--pubkey-txt need --pubkey or --pubkey-out",
                  file=sys.stderr)
            return 2
        if args.pubkey_out and not os.path.exists(args.pubkey_out):
            print("error: public key not found: %s" % args.pubkey_out, file=sys.stderr)
            return 2
        if args.pubkey_txt:
            with open(args.pubkey_txt, "w", encoding="ascii") as fh:
                fh.write(om.pubkey_txt(pub))
            print("pubkey txt   %s" % args.pubkey_txt)
        if args.emit_c_header:
            with open(args.emit_c_header, "w", encoding="ascii") as fh:
                fh.write(om.c_header(pub, comment=args.comment or
                                     "Moggie OTA public key — version %s" % obj["version"]))
            print("C header     %s" % args.emit_c_header)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="mkmanifest.py",
        description="Build and sign a Moggie OTA manifest (RSA-2048/SHA-256).",
    )
    p.add_argument("--keygen", action="store_true",
                   help="generate a signing keypair and exit")
    p.add_argument("--selftest", action="store_true",
                   help="run the format's self-tests and exit")
    p.add_argument("--key-dir", default="keys", help="where --keygen writes (default: keys/)")
    p.add_argument("--bits", type=int, default=2048, help="key size; only 2048 is accepted")
    p.add_argument("--force", action="store_true", help="overwrite an existing key")
    p.add_argument("--image", help="rootfs image to sign")
    p.add_argument("--version", help="firmware version string, e.g. 1.0.0")
    p.add_argument("--key", help="private key (PEM) to sign with")
    p.add_argument("--model", type=int, default=om.MODEL_ID,
                   help="hardware model; must be 8 for the Play:5 Gen 1")
    p.add_argument("--out", default="manifest.json", help="manifest output path")
    p.add_argument("--pubkey", help="public key (PEM) to self-verify the result")
    p.add_argument("--pubkey-out", help="public key (PEM) matching --key")
    p.add_argument("--pubkey-txt", help="also write the key as n=<hex>/e=<dec> for tests")
    p.add_argument("--emit-c-header", help="also write a C header embedding the key")
    p.add_argument("--comment", help="comment text for the generated C header")
    p.add_argument("--force-oversize", action="store_true",
                   help="sign an image over 7.5 MiB (tests only)")
    return p


def main(argv: list[str]) -> int:
    p = build_parser()
    args = p.parse_args(argv)
    try:
        if args.selftest:
            return om._selftest()
        if args.keygen:
            return cmd_keygen(args)
        missing = [n for n, v in (("--image", args.image), ("--version", args.version),
                                  ("--key", args.key)) if not v]
        if missing:
            p.error("missing required argument(s): %s" % ", ".join(missing))
        return cmd_build(args)
    except om.ManifestError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
