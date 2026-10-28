#!/usr/bin/env python3
"""Verify the frozen audit's inputs, full diffs, key findings, and citations.

Read-only: no compilation, target execution, image extraction, or hardware I/O.
"""
import hashlib
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
AUDIT = ROOT / "docs/build/ROOTFS-FIDELITY-AUDIT.md"

inputs = 0
for name in ("containers.txt", "abi.txt"):
    for path, size, expected in re.findall(
        r"^(?:INPUT |IMAGE \S+ |BINARY )(.+?) bytes=(\d+) sha256=([0-9a-f]{64})$",
        (HERE / name).read_text(), re.M,
    ):
        data = Path(path).read_bytes()
        assert len(data) == int(size), path
        assert hashlib.sha256(data).hexdigest() == expected, path
        inputs += 1
assert inputs == 24, inputs

manifests = {}
for label, expected in (("stock16", 300), ("buildroot", 431), ("tc26", 485)):
    lines = (HERE / f"{label}-layout.tsv").read_text().splitlines()[1:]
    manifests[label] = dict(line.split("\t", 1) for line in lines)
    assert len(lines) == len(manifests[label]) == expected, label
for label in ("buildroot", "tc26"):
    old, new = manifests["stock16"], manifests[label]
    expected = []
    for path in sorted(old.keys() | new.keys()):
        if old.get(path) != new.get(path):
            status = "added" if path not in old else "removed" if path not in new else "changed"
            blank = "-\t-\t-\t-"
            expected.append(f"{path}\t{status}\t{old.get(path, blank)}\t{new.get(path, blank)}")
    actual = (HERE / f"stock16-vs-{label}.tsv").read_text().splitlines()[1:]
    assert actual == expected, label
    assert new["/sbin/init"].endswith("../bin/busybox"), label
    assert new["/bin/busybox"].startswith("-rwxr-xr-x\t0/0\t"), label
    assert not any(path == "/init" or path.startswith(("/modules/", "/lib/modules/"))
                   or path.endswith((".ko", ".o", "/moggie-updater")) for path in new), label
    assert "\t90,  1\t" in new["/dev/mtd1"], label

closure = (HERE / "elf-closure.txt").read_text()
for label, count in (("buildroot", 34), ("tc26", 53)):
    assert f"RESULT {label} ELF_count={count} missing_default_path_dependencies=[]" in closure
kernel = (ROOT / "out/fidelity-audit/stock16/kernel-inflated.bin").read_bytes()
assert kernel[0x657e4:0x657ec].hex() == "2f80000340be0018"
assert kernel[0x125e68:].startswith(b"Linux version 2.4.22-rincon-")
assert b"squashfs: version 3.0" in kernel

links = 0
for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", AUDIT.read_text()):
    if "://" in target:
        continue
    filename, _, anchor = target.partition("#")
    path = AUDIT.parent / filename
    assert path.is_file(), target
    if anchor:
        assert re.fullmatch(r"L\d+", anchor), target
        assert 1 <= int(anchor[1:]) <= len(path.read_text().splitlines()), target
    links += 1
assert links >= 60, links
print(f"PASS: {inputs} input hashes, 3 manifests, 2 exhaustive diffs, kernel version gate, ELF closure and {links} citation targets; no builds")
