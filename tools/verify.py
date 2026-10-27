#!/usr/bin/env python3
"""Verify a Moggie OTA manifest, and optionally the image it describes.

This is the reference verifier: the tests compare it against the C updater so
that a bug in one cannot hide a bug in the other.  It is deliberately strict —
it rejects manifests that are valid JSON but not in canonical form, because the
signed bytes are defined positionally and an on-device verifier must see
exactly the bytes that were signed.

    tools/verify.py --manifest out/manifest.json --image out/rootfs.squashfs \
                    --pubkey keys/ota_signing.pub.pem

    # signature and metadata only, no image (fast; what the boot chooser needs)
    tools/verify.py --manifest out/manifest.json --pubkey keys/ota_signing.pub.pem --no-image

Exit codes: 0 verified, 1 verification failure, 2 usage or I/O error.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import otamanifest as om  # noqa: E402


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(
        prog="verify.py",
        description="Verify a Moggie OTA manifest (+ image) against an RSA-2048 public key.",
    )
    p.add_argument("--manifest", required=True, help="manifest.json to verify")
    p.add_argument("--pubkey", required=True, help="signing public key, PEM")
    p.add_argument("--image", help="rootfs image to hash and compare")
    p.add_argument("--no-image", action="store_true",
                   help="skip image checks (signature + metadata only)")
    p.add_argument("--model", type=int, default=om.MODEL_ID,
                   help="expected hardware model (default: 8)")
    p.add_argument("--max-size", type=int, default=om.MAX_IMAGE_BYTES,
                   help="maximum accepted image size (default: 7864320)")
    p.add_argument("--quiet", action="store_true", help="print nothing on success")
    p.add_argument("--json", action="store_true", help="machine-readable result on stdout")
    args = p.parse_args(argv)

    if not args.no_image and not args.image:
        p.error("--image is required unless --no-image is given")
    if args.no_image and args.image:
        p.error("--image and --no-image are mutually exclusive")

    try:
        facts = om.verify_manifest(
            args.manifest,
            args.pubkey,
            image=args.image,
            model=args.model,
            max_size=args.max_size,
            check_image=not args.no_image,
        )
    except om.ManifestError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print("FAIL  %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print("ERROR %s" % exc, file=sys.stderr)
        return 2

    if args.json:
        out = {"ok": True}
        out.update(facts)
        print(json.dumps(out, sort_keys=True))
    elif not args.quiet:
        print("OK    %s" % args.manifest)
        print("  version    %s" % facts["version"])
        print("  model      %d" % facts["model"])
        print("  size       %d bytes" % facts["size"])
        print("  sha256     %s" % facts["sha256"])
        if facts["image_sha256"]:
            print("  image      hash matches")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
