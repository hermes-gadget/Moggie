"""Moggie OTA manifest: canonical form, signing and verification primitives.

Wire format (one line of JSON, then a newline):

    {"version":"1.2.3","model":8,"size":4123456,"sha256":"<64 hex>","signature":"<base64>"}

The signature is RSA-2048 / PKCS#1 v1.5 over SHA-256, covering *the bytes of
the manifest up to but not including* the `,"signature":"..."` member.  In
other words the signed body is exactly the canonical serialisation of the
other four fields, in this order, with no spaces:

    {"version":..,"model":..,"size":..,"sha256":..}

Because the signed bytes are defined positionally, an on-device verifier does
not need a JSON canonicaliser or a big JSON parser: it finds the marker
`,"signature":"` (which must be the final member), hashes everything before it,
and checks that those bytes *are* the canonical form of the four fields it
parsed.  `firmware/updater/manifest.c` does exactly that, and this module is the
reference implementation the tests compare it against.

Only stdlib + the `openssl` CLI are used, because that is all the build hosts
are guaranteed to have.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# --- platform constants -----------------------------------------------------

MODEL_ID = 8                      # Play:5 Gen 1 = model 8
SLOT_BYTES = 8 * 1024 * 1024      # one half of mtd3
FLASH_ROOTFS_BYTES = 2 * SLOT_BYTES
MAX_IMAGE_BYTES = 7_864_320       # 7.5 MiB — the hard rootfs budget
SQUASHFS_MAGIC = b"hsqs"

FIELDS = ("version", "model", "size", "sha256")
SIGNATURE_MEMBER = b',"signature":"'
VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

# `,"signature":"<b64>"}` is the tail of the file; a single trailing newline is
# allowed (and emitted).  Anything else after the closing brace is rejected.
TAIL_RE = re.compile(rb',"signature":"([A-Za-z0-9+/]+={0,2})"\}[ \t\r\n]*\Z')
B64_RE = re.compile(rb"^[A-Za-z0-9+/]+={0,2}$")


class ManifestError(Exception):
    """Any problem with a manifest, its signature or the image it names."""


# --- canonical form ---------------------------------------------------------

def sha256_file(path: str, limit: int | None = None) -> tuple[str, int]:
    """Return (hex digest, bytes hashed) for a file, streaming."""
    h = hashlib.sha256()
    n = 0
    with open(path, "rb") as fh:
        while True:
            want = 1 << 16
            if limit is not None:
                if n >= limit:
                    break
                want = min(want, limit - n)
            chunk = fh.read(want)
            if not chunk:
                break
            h.update(chunk)
            n += len(chunk)
    return h.hexdigest(), n


def canonical_body(version: str, model: int, size: int, sha256_hex: str) -> bytes:
    """The exact bytes that get signed."""
    if not isinstance(version, str) or not VERSION_RE.match(version):
        raise ManifestError(
            "version must match %s (got %r)" % (VERSION_RE.pattern, version)
        )
    if not isinstance(model, int) or isinstance(model, bool):
        raise ManifestError("model must be an integer")
    if not isinstance(size, int) or isinstance(size, bool) or size < 0:
        raise ManifestError("size must be a non-negative integer")
    if not isinstance(sha256_hex, str) or not SHA256_RE.match(sha256_hex):
        raise ManifestError("sha256 must be 64 lowercase hex characters")
    body = (
        '{"version":"%s","model":%d,"size":%d,"sha256":"%s"}'
        % (version, model, size, sha256_hex)
    )
    return body.encode("ascii")


def assemble(body: bytes, signature: bytes) -> bytes:
    """Wrap a signed body and a raw signature into the manifest file bytes."""
    if not body.endswith(b"}"):
        raise ManifestError("body must end with '}'")
    sig_b64 = base64.b64encode(signature)
    return body[:-1] + SIGNATURE_MEMBER + sig_b64 + b'"}\n'


def split_manifest(raw: bytes) -> tuple[bytes, bytes]:
    """Split manifest bytes into (signed body, raw signature)."""
    m = TAIL_RE.search(raw)
    if not m:
        raise ManifestError(
            "manifest does not end with a signature member "
            '(\',"signature":"<base64>"}\')'
        )
    # The signature member is the last member of the object: the signed bytes
    # are everything before its leading comma, plus the object's closing brace.
    body = raw[: m.start()] + b"}"
    if not body.startswith(b"{") or not body.endswith(b"}"):
        raise ManifestError("signed body is not a JSON object")
    if body.count(SIGNATURE_MEMBER) != 0:
        raise ManifestError("duplicate signature member")
    try:
        sig = base64.b64decode(m.group(1), validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ManifestError("signature is not valid base64: %s" % exc) from exc
    return body, sig


def _no_duplicate_keys(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise ManifestError("duplicate key %r in manifest" % key)
        seen[key] = value
    return seen


def parse_body(body: bytes) -> dict:
    """Strictly parse the signed body and return its four fields."""
    try:
        text = body.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ManifestError("manifest body is not ASCII: %s" % exc) from exc
    try:
        obj = json.loads(text, object_pairs_hook=_no_duplicate_keys)
    except ManifestError:
        raise
    except json.JSONDecodeError as exc:
        raise ManifestError("manifest body is not valid JSON: %s" % exc) from exc
    if not isinstance(obj, dict):
        raise ManifestError("manifest body must be a JSON object")
    missing = [f for f in FIELDS if f not in obj]
    if missing:
        raise ManifestError("manifest body is missing: %s" % ", ".join(missing))
    extra = [k for k in obj if k not in FIELDS]
    if extra:
        raise ManifestError("manifest body has unknown fields: %s" % ", ".join(extra))
    if not isinstance(obj["version"], str):
        raise ManifestError("version must be a string")
    if not isinstance(obj["model"], int) or isinstance(obj["model"], bool):
        raise ManifestError("model must be an integer")
    if not isinstance(obj["size"], int) or isinstance(obj["size"], bool):
        raise ManifestError("size must be an integer")
    if not isinstance(obj["sha256"], str):
        raise ManifestError("sha256 must be a string")
    if obj["size"] < 0:
        raise ManifestError("size must not be negative")
    return obj


def check_canonical(body: bytes, obj: dict) -> None:
    """Reject bodies that are not byte-identical to the canonical form.

    This is what stops ambiguous input (odd spacing, key reordering, escapes)
    from being signed in one shape and read in another.
    """
    want = canonical_body(obj["version"], obj["model"], obj["size"], obj["sha256"])
    if body != want:
        raise ManifestError(
            "manifest body is not in canonical form (whitespace, key order or "
            "escaping differ); refusing to trust it"
        )


# --- openssl glue -----------------------------------------------------------

def _run(cmd: list[str], *, data: bytes | None = None) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
        )
    except FileNotFoundError as exc:
        raise ManifestError("openssl not found on PATH") from exc


def keygen(key_dir: str, bits: int = 2048, force: bool = False) -> tuple[str, str]:
    """Generate a signing keypair.  Returns (private_pem, public_pem) paths.

    The private key is chmod 0600.  Never ship one of these in the repo.
    """
    if bits != 2048:
        raise ManifestError("the firmware only accepts RSA-2048 keys (got %d)" % bits)
    os.makedirs(key_dir, exist_ok=True)
    priv = os.path.join(key_dir, "ota_signing.key")
    pub = os.path.join(key_dir, "ota_signing.pub.pem")
    if os.path.exists(priv) and not force:
        raise ManifestError("%s exists (use --force to replace it)" % priv)

    res = _run(["openssl", "genpkey", "-algorithm", "RSA",
                "-pkeyopt", "rsa_keygen_bits:%d" % bits, "-out", priv])
    if res.returncode != 0:
        raise ManifestError("openssl genpkey failed: %s" % res.stderr.decode(errors="replace").strip())
    os.chmod(priv, 0o600)

    res = _run(["openssl", "pkey", "-in", priv, "-pubout", "-out", pub])
    if res.returncode != 0:
        raise ManifestError("openssl pkey -pubout failed: %s" % res.stderr.decode(errors="replace").strip())
    os.chmod(pub, 0o644)
    return priv, pub


def sign_body(body: bytes, private_key: str) -> bytes:
    """RSA-2048/SHA-256 PKCS#1 v1.5 signature over `body`."""
    if not os.path.exists(private_key):
        raise ManifestError("private key not found: %s" % private_key)
    with tempfile.TemporaryDirectory(prefix="moggie-sign-") as td:
        sig_path = os.path.join(td, "sig.bin")
        res = _run(["openssl", "dgst", "-sha256", "-sign", private_key,
                    "-out", sig_path], data=body)
        if res.returncode != 0:
            raise ManifestError(
                "openssl dgst -sign failed: %s"
                % res.stderr.decode(errors="replace").strip()
            )
        with open(sig_path, "rb") as fh:
            sig = fh.read()
    if len(sig) != 256:
        raise ManifestError("expected a 256-byte RSA-2048 signature, got %d" % len(sig))
    return sig


def verify_body(body: bytes, signature: bytes, public_key: str) -> None:
    """Raise ManifestError unless `signature` is valid for `body`."""
    if not os.path.exists(public_key):
        raise ManifestError("public key not found: %s" % public_key)
    if len(signature) != 256:
        raise ManifestError("signature is %d bytes, expected 256 (RSA-2048)" % len(signature))
    with tempfile.TemporaryDirectory(prefix="moggie-verify-") as td:
        sig_path = os.path.join(td, "sig.bin")
        with open(sig_path, "wb") as fh:
            fh.write(signature)
        res = _run(["openssl", "dgst", "-sha256", "-verify", public_key,
                    "-signature", sig_path], data=body)
    if res.returncode != 0:
        detail = res.stdout.decode(errors="replace").strip() or res.stderr.decode(errors="replace").strip()
        raise ManifestError("signature verification failed: %s" % (detail or "openssl said no"))


# --- build / verify ---------------------------------------------------------

def build_manifest(
    image: str,
    version: str,
    private_key: str,
    *,
    model: int = MODEL_ID,
    force_oversize: bool = False,
) -> bytes:
    """Hash `image`, sign the manifest and return the manifest file bytes."""
    if not os.path.isfile(image):
        raise ManifestError("image not found: %s" % image)
    size = os.path.getsize(image)
    if size == 0:
        raise ManifestError("image is empty: %s" % image)
    if size > MAX_IMAGE_BYTES and not force_oversize:
        raise ManifestError(
            "image is %d bytes, over the %d-byte (7.5 MiB) slot budget by %d bytes"
            % (size, MAX_IMAGE_BYTES, size - MAX_IMAGE_BYTES)
        )
    if size > SLOT_BYTES:
        raise ManifestError(
            "image is %d bytes, larger than the %d-byte slot" % (size, SLOT_BYTES)
        )
    digest, hashed = sha256_file(image)
    if hashed != size:
        raise ManifestError("short read hashing image")
    body = canonical_body(version, model, size, digest)
    return assemble(body, sign_body(body, private_key))


def load_manifest(path: str) -> tuple[bytes, dict, bytes]:
    """Read a manifest from disk; return (body, fields, signature), validated
    for structure and canonical form but *not* for signature validity."""
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise ManifestError("cannot read %s: %s" % (path, exc)) from exc
    if not raw:
        raise ManifestError("manifest is empty: %s" % path)
    if len(raw) > 8192:
        raise ManifestError("manifest is %d bytes, refusing anything over 8 KiB" % len(raw))
    body, sig = split_manifest(raw)
    obj = parse_body(body)
    check_canonical(body, obj)
    return body, obj, sig


def verify_manifest(
    manifest: str,
    public_key: str,
    *,
    image: str | None = None,
    model: int = MODEL_ID,
    max_size: int = MAX_IMAGE_BYTES,
    check_image: bool = True,
) -> dict:
    """Full verification: structure, signature, model, size and (optionally) the
    image's own hash.  Returns a dict of verified facts; raises ManifestError."""
    body, obj, sig = load_manifest(manifest)
    verify_body(body, sig, public_key)

    if obj["model"] != model:
        raise ManifestError(
            "manifest model is %r, this firmware is model %d" % (obj["model"], model)
        )
    if obj["size"] > max_size:
        raise ManifestError(
            "manifest size %d exceeds the %d-byte slot budget" % (obj["size"], max_size)
        )
    if obj["size"] == 0:
        raise ManifestError("manifest size is zero")

    digest = None
    if check_image:
        if image is None:
            raise ManifestError("image path is required to verify the image")
        if not os.path.isfile(image):
            raise ManifestError("image not found: %s" % image)
        actual_size = os.path.getsize(image)
        if actual_size != obj["size"]:
            raise ManifestError(
                "image is %d bytes but the manifest says %d (%s by %d bytes)"
                % (actual_size, obj["size"],
                   "truncated" if actual_size < obj["size"] else "oversize",
                   abs(obj["size"] - actual_size))
            )
        digest, hashed = sha256_file(image)
        if hashed != obj["size"]:
            raise ManifestError("short read hashing image")
        if digest != obj["sha256"]:
            raise ManifestError(
                "image sha256 is %s but the manifest says %s" % (digest, obj["sha256"])
            )
        with open(image, "rb") as fh:
            magic = fh.read(4)
        if magic != SQUASHFS_MAGIC:
            raise ManifestError(
                "image does not start with the squashfs magic %r (got %r)"
                % (SQUASHFS_MAGIC, magic)
            )

    return {
        "version": obj["version"],
        "model": obj["model"],
        "size": obj["size"],
        "sha256": obj["sha256"],
        "image_sha256": digest,
        "signature_bytes": len(sig),
    }


def pubkey_txt(public_pem: str) -> str:
    """Text form of the public key for the C test binary.

    Deliberately dumb (`n=<hex>` / `e=<decimal>`, one per line) so the updater
    does not need an ASN.1 parser in its test-only code path.  Release
    firmware embeds the same numbers as a C array instead.
    """
    res = _run(["openssl", "rsa", "-pubin", "-in", public_pem, "-noout", "-modulus"])
    if res.returncode != 0:
        raise ManifestError("openssl rsa -modulus failed: %s" % res.stderr.decode(errors="replace").strip())
    line = res.stdout.decode("ascii", "replace").strip()
    prefix = "Modulus="
    if not line.startswith(prefix):
        raise ManifestError("unexpected openssl modulus output: %r" % line[:40])
    modulus = line[len(prefix):].strip().lower()
    if not re.fullmatch(r"[0-9a-f]{512}", modulus):
        raise ManifestError("modulus is not 2048 bits (%d hex chars)" % len(modulus))
    return "n=%s\ne=65537\n" % modulus


def c_header(public_pem: str, *, comment: str) -> str:
    """A C header embedding the public key, for the production build."""
    txt = pubkey_txt(public_pem)
    fields = dict(line.split("=", 1) for line in txt.strip().splitlines())
    modulus = bytes.fromhex(fields["n"])
    exp = int(fields["e"])
    if len(modulus) != 256:
        raise ManifestError("modulus must be 256 bytes")
    out = []
    out.append("/* Generated by tools/mkmanifest.py --emit-c-header. Do not edit. */")
    if comment:
        for line in comment.strip().splitlines():
            out.append("/* %s */" % line.replace("*/", "* /"))
    out.append("#ifndef MOGGIE_OTA_PUBKEY_H")
    out.append("#define MOGGIE_OTA_PUBKEY_H")
    out.append("")
    out.append("#define MOGGIE_OTA_PUBKEY_BITS 2048")
    out.append("#define MOGGIE_OTA_PUBKEY_E %du" % exp)
    out.append("")
    out.append("static const unsigned char MOGGIE_OTA_PUBKEY_N[256] = {")
    for i in range(0, 256, 12):
        row = ", ".join("0x%02x" % b for b in modulus[i:i + 12])
        out.append("    %s," % row)
    out.append("};")
    out.append("")
    out.append("#endif /* MOGGIE_OTA_PUBKEY_H */")
    out.append("")
    return "\n".join(out)


def have_openssl() -> bool:
    return shutil.which("openssl") is not None


def _selftest() -> int:
    """Exercise the whole format without touching the network. Prints PASS/FAIL."""
    failures = 0
    tmp = tempfile.mkdtemp(prefix="moggie-selftest-")

    def check(name: str, fn):
        nonlocal failures
        try:
            fn()
        except Exception as exc:  # noqa: BLE001 - selftest wants the message
            print("FAIL  %s: %s" % (name, exc))
            failures += 1
        else:
            print("PASS  %s" % name)

    def expect_error(name: str, fn, needle: str = ""):
        def inner():
            try:
                fn()
            except ManifestError as exc:
                if needle and needle not in str(exc):
                    raise AssertionError("error %r does not mention %r" % (str(exc), needle)) from exc
                return
            raise AssertionError("expected a ManifestError, got none")
        check(name, inner)

    if not have_openssl():
        print("FAIL  openssl not found on PATH")
        return 1

    image = os.path.join(tmp, "rootfs.squashfs")
    with open(image, "wb") as fh:
        fh.write(SQUASHFS_MAGIC)
        fh.write(bytes(range(256)) * 4000)   # ~1 MiB of deterministic content

    state = {}

    def gen_keys():
        priv, pub = keygen(os.path.join(tmp, "keys"))
        assert os.path.exists(priv) and os.path.exists(pub)
        mode = os.stat(priv).st_mode & 0o777
        if mode != 0o600:
            raise AssertionError("private key mode is %o, expected 600" % mode)
        state["priv"], state["pub"] = priv, pub

    def build():
        raw = build_manifest(image, "1.0.0", state["priv"])
        state["raw"] = raw
        if not raw.endswith(b'"}\n'):
            raise AssertionError("manifest does not end with a signature member")
        body, sig = split_manifest(raw)
        if len(sig) != 256:
            raise AssertionError("signature is %d bytes, expected 256" % len(sig))
        if body != canonical_body("1.0.0", MODEL_ID, os.path.getsize(image),
                                   sha256_file(image)[0]):
            raise AssertionError("body is not canonical")
        path = os.path.join(tmp, "manifest.json")
        with open(path, "wb") as fh:
            fh.write(raw)
        state["manifest"] = path

    def verify_ok():
        facts = verify_manifest(state["manifest"], state["pub"], image=image)
        if facts["version"] != "1.0.0" or facts["model"] != MODEL_ID:
            raise AssertionError("wrong facts: %r" % facts)
        if facts["signature_bytes"] != 256:
            raise AssertionError("signature_bytes != 256")

    def verify_bad_sig():
        body, sig = split_manifest(state["raw"])
        broken = bytearray(sig)
        broken[10] ^= 0x01
        raw = assemble(body, bytes(broken))
        p = os.path.join(tmp, "bad-sig.json")
        with open(p, "wb") as fh:
            fh.write(raw)
        verify_manifest(p, state["pub"], image=image)

    def verify_tampered_body():
        body, sig = split_manifest(state["raw"])
        tampered = bytearray(body)
        idx = body.index(b'"size":')
        tampered[idx + 8] = ord("1") if body[idx + 8] != ord("1") else ord("2")
        raw = assemble(bytes(tampered), sig)
        p = os.path.join(tmp, "tampered.json")
        with open(p, "wb") as fh:
            fh.write(raw)
        verify_manifest(p, state["pub"], image=image)

    def verify_whitespace():
        body, sig = split_manifest(state["raw"])
        raw = assemble(body.replace(b',"model"', b', "model"'), sig)
        p = os.path.join(tmp, "spaced.json")
        with open(p, "wb") as fh:
            fh.write(raw)
        load_manifest(p)

    def verify_wrong_model():
        body, _ = split_manifest(state["raw"])
        obj = parse_body(body)
        b = canonical_body(obj["version"], 7, obj["size"], obj["sha256"])
        raw = assemble(b, sign_body(b, state["priv"]))
        p = os.path.join(tmp, "model7.json")
        with open(p, "wb") as fh:
            fh.write(raw)
        verify_manifest(p, state["pub"], image=image)

    def oversize_refused():
        big = os.path.join(tmp, "big.img")
        with open(big, "wb") as fh:
            fh.write(SQUASHFS_MAGIC)
            fh.truncate(MAX_IMAGE_BYTES + 1)
        build_manifest(big, "1.0.0", state["priv"])

    def oversize_forced():
        big = os.path.join(tmp, "big2.img")
        with open(big, "wb") as fh:
            fh.write(SQUASHFS_MAGIC)
            fh.truncate(MAX_IMAGE_BYTES + 4096)
        raw = build_manifest(big, "1.0.0", state["priv"], force_oversize=True)
        p = os.path.join(tmp, "big2.json")
        with open(p, "wb") as fh:
            fh.write(raw)
        verify_manifest(p, state["pub"], image=big, max_size=SLOT_BYTES)

    def export_formats():
        txt = pubkey_txt(state["pub"])
        if "n=" not in txt or "e=65537" not in txt:
            raise AssertionError("pubkey_txt output looks wrong: %r" % txt[:40])
        hdr = c_header(state["pub"], comment="selftest key")
        if "MOGGIE_OTA_PUBKEY_E 65537u" not in hdr or hdr.count("0x") < 256:
            raise AssertionError("c_header output looks wrong")
        with open(os.path.join(tmp, "pubkey.h"), "w") as fh:
            fh.write(hdr)

    def truncated_image():
        cut = os.path.join(tmp, "cut.img")
        with open(image, "rb") as src, open(cut, "wb") as dst:
            dst.write(src.read()[:-64])
        verify_manifest(state["manifest"], state["pub"], image=cut)

    def corrupt_image():
        with open(image, "rb") as fh:
            data = bytearray(fh.read())
        p = os.path.join(tmp, "corrupt.img")
        data[len(data) // 2] ^= 0xff
        with open(p, "wb") as fh:
            fh.write(data)
        verify_manifest(state["manifest"], state["pub"], image=p)

    check("generate an RSA-2048 signing keypair", gen_keys)
    check("build and sign a manifest", build)
    check("verify a good manifest + image", verify_ok)
    expect_error("reject a flipped signature bit", verify_bad_sig, "signature")
    expect_error("reject a tampered body field", verify_tampered_body, "signature")
    expect_error("reject non-canonical whitespace", verify_whitespace, "canonical")
    expect_error("reject the wrong model", verify_wrong_model, "model")
    expect_error("refuse an oversize image", oversize_refused, "7.5 MiB")
    check("allow an oversize image only with --force-oversize", oversize_forced)
    check("export pubkey.txt and a C header", export_formats)
    expect_error("reject a truncated image", truncated_image, "truncated")
    expect_error("reject a corrupt image", corrupt_image, "sha256")

    print()
    if failures:
        print("%d selftest check(s) FAILED (artifacts in %s)" % (failures, tmp))
        return 1
    print("all selftest checks passed")
    shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(_selftest())
