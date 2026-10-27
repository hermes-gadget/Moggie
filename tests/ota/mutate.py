#!/usr/bin/env python3
"""Test-only byte surgery for OTA manifests and images.

Every mutation here is something an attacker, a flaky transfer or a power cut
could do to a real update in flight.  Nothing in this file is used by the
firmware.
"""

from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))

import otamanifest as om  # noqa: E402


def flip(path: str, offset: int | None, at: str) -> None:
    with open(path, "r+b") as fh:
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
        if size == 0:
            raise SystemExit("refuse to flip a byte in an empty file")
        if at == "middle":
            offset = size // 2
        elif at == "first":
            offset = 0
        elif at == "last":
            offset = size - 1
        if offset is None or offset < 0 or offset >= size:
            raise SystemExit("offset %r is outside %s (%d bytes)" % (offset, path, size))
        fh.seek(offset)
        byte = fh.read(1)
        fh.seek(offset)
        fh.write(bytes([byte[0] ^ 0xFF]))
    print("flipped byte %d of %s" % (offset, path))


def resize(path: str, size: int, fill: int) -> None:
    with open(path, "r+b") as fh:
        fh.truncate(size)
        fh.seek(0, os.SEEK_END)
        if fh.tell() < size:            # extend if the file was shorter
            fh.seek(size - 1)
            fh.write(bytes([fill]))
    print("%s is now %d bytes" % (path, os.path.getsize(path)))


def bad_signature(path: str, out: str) -> None:
    with open(path, "rb") as fh:
        raw = fh.read()
    body, sig = om.split_manifest(raw)
    broken = bytearray(sig)
    broken[7] ^= 0x40
    with open(out, "wb") as fh:
        fh.write(om.assemble(body, bytes(broken)))
    print("wrote %s with one flipped signature bit" % out)


def tamper(path: str, field: str, value: str, out: str) -> None:
    with open(path, "rb") as fh:
        raw = fh.read()
    body, sig = om.split_manifest(raw)
    obj = om.parse_body(body)
    if field in ("size", "model"):
        new_value = int(value)
    else:
        new_value = value
    args = {
        "version": obj["version"],
        "model": obj["model"],
        "size": obj["size"],
        "sha256": obj["sha256"],
    }
    args[field] = new_value
    new_body = om.canonical_body(args["version"], args["model"], args["size"],
                                 args["sha256"])
    with open(out, "wb") as fh:
        fh.write(om.assemble(new_body, sig))   # signature deliberately not recomputed
    print("wrote %s with %s changed to %r, signature left as it was" % (out, field, value))


def reorder(path: str, key: str, out: str) -> None:
    """A manifest whose signature is valid but whose body is in a different key
    order.  The signed bytes are defined positionally, so this must be refused
    even though the RSA check passes: otherwise two parsers could disagree about
    which bytes were signed."""
    with open(path, "rb") as fh:
        body, _sig = om.split_manifest(fh.read())
    obj = om.parse_body(body)
    new_body = ('{"sha256":"%s","size":%d,"model":%d,"version":"%s"}'
                % (obj["sha256"], obj["size"], obj["model"], obj["version"])).encode()
    with open(out, "wb") as fh:
        fh.write(om.assemble(new_body, om.sign_body(new_body, key)))
    print("wrote %s: valid signature over a re-ordered body" % out)


def duplicate(path: str, key: str, out: str, field: str) -> None:
    """A manifest with the same field twice, validly signed.  A last-wins parser
    would install a different image from the one that was signed."""
    with open(path, "rb") as fh:
        body, _sig = om.split_manifest(fh.read())
    text = body.decode("ascii")
    needle = '"%s":' % field
    idx = text.index(needle) + len(needle)
    if field in ("size", "model"):
        end = idx
        while text[end] in "0123456789":
            end += 1
    else:
        end = text.index('"', idx + 1) + 1
    new_body = ('%s,"%s":%s%s'
                % (text[:end], field, text[idx:end], text[end:])).encode()
    with open(out, "wb") as fh:
        fh.write(om.assemble(new_body, om.sign_body(new_body, key)))
    print("wrote %s: field %s appears twice, body validly signed" % (out, field))


def fuzz(path: str, out_dir: str, count: int, seed: int) -> None:
    """Write `count` mutants of a valid manifest: flips, deletions, insertions
    and truncations.  None of them should crash the verifier."""
    import random

    rng = random.Random(seed)
    os.makedirs(out_dir, exist_ok=True)
    with open(path, "rb") as fh:
        raw = bytearray(fh.read())
    for i in range(count):
        data = bytearray(raw)
        for _ in range(rng.randint(1, 3)):
            op = rng.choice(("flip", "delete", "insert", "truncate"))
            if op == "flip" and data:
                pos = rng.randrange(len(data))
                data[pos] ^= 1 << rng.randrange(8)
            elif op == "delete" and len(data) > 8:
                del data[rng.randrange(len(data))]
            elif op == "insert":
                data.insert(rng.randrange(len(data)), rng.randrange(256))
            elif op == "truncate":
                del data[rng.randrange(len(data)):]
        with open(os.path.join(out_dir, "mut_%03d.json" % i), "wb") as fh:
            fh.write(data)
    print("wrote %d mutants of %s to %s" % (count, path, out_dir))


def strip_signature(path: str, out: str) -> None:
    with open(path, "rb") as fh:
        raw = fh.read()
    body, _sig = om.split_manifest(raw)
    with open(out, "wb") as fh:
        fh.write(body + b"\n")
    print("wrote %s without a signature member" % out)


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="mutate.py", description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    q = sub.add_parser("flip", help="flip one byte (xor 0xff)")
    q.add_argument("--file", required=True)
    q.add_argument("--offset", type=int)
    q.add_argument("--at", choices=("middle", "first", "last"), default="middle")

    q = sub.add_parser("truncate", help="cut a file short")
    q.add_argument("--file", required=True)
    q.add_argument("--size", type=int, required=True)

    q = sub.add_parser("pad", help="extend a file with zeros")
    q.add_argument("--file", required=True)
    q.add_argument("--size", type=int, required=True)

    q = sub.add_parser("bad-signature", help="flip a bit inside the signature")
    q.add_argument("--file", required=True)
    q.add_argument("--out", required=True)

    q = sub.add_parser("tamper", help="change a signed field, keep the old signature")
    q.add_argument("--file", required=True)
    q.add_argument("--field", required=True, choices=("version", "model", "size", "sha256"))
    q.add_argument("--value", required=True)
    q.add_argument("--out", required=True)

    q = sub.add_parser("reorder", help="re-sign a re-ordered (non-canonical) body")
    q.add_argument("--file", required=True)
    q.add_argument("--key", required=True)
    q.add_argument("--out", required=True)

    q = sub.add_parser("duplicate", help="re-sign a body with a duplicate field")
    q.add_argument("--file", required=True)
    q.add_argument("--key", required=True)
    q.add_argument("--field", default="size")
    q.add_argument("--out", required=True)

    q = sub.add_parser("fuzz", help="write randomly mutated manifests")
    q.add_argument("--file", required=True)
    q.add_argument("--out-dir", required=True)
    q.add_argument("--count", type=int, default=40)
    q.add_argument("--seed", type=int, default=1)

    q = sub.add_parser("strip-signature", help="remove the signature member")
    q.add_argument("--file", required=True)
    q.add_argument("--out", required=True)

    args = p.parse_args(argv)
    if args.cmd == "flip":
        flip(args.file, args.offset, args.at)
    elif args.cmd == "truncate":
        resize(args.file, args.size, 0)
    elif args.cmd == "pad":
        resize(args.file, args.size, 0)
    elif args.cmd == "bad-signature":
        bad_signature(args.file, args.out)
    elif args.cmd == "tamper":
        tamper(args.file, args.field, args.value, args.out)
    elif args.cmd == "reorder":
        reorder(args.file, args.key, args.out)
    elif args.cmd == "duplicate":
        duplicate(args.file, args.key, args.out, args.field)
    elif args.cmd == "fuzz":
        fuzz(args.file, args.out_dir, args.count, args.seed)
    elif args.cmd == "strip-signature":
        strip_signature(args.file, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
