#!/usr/bin/env python3
"""Offline evidence capture; never builds, mounts, runs target code, or writes inputs.

Run from the repo root. Only this evidence directory and out/fidelity-audit are
written. Stock password fields are redacted; shadow files are never opened.
"""
import hashlib
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[4]
os.chdir(ROOT)
DEST = Path(__file__).resolve().parent
SCRATCH = ROOT / "out/fidelity-audit"
SCRATCH.mkdir(parents=True, exist_ok=True)
BENCH = Path("/home/ben/sonos-bench/sonos-fenway/firmware")
STOCK = BENCH / "rootfs/57.10-25140"
BOOTLIN = Path("/home/ben/Moggie/toolchain/powerpc-e300c3--glibc--stable-2026.08-1")
TC26 = Path("/home/ben/Moggie-tc/toolchain/powerpc-buildroot-linux-gnu")


def command(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, check=True)
    return result.stdout.decode(errors="replace"), result.stderr.decode(errors="replace")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def capture(stream, args, allowed=(0,)):
    result = subprocess.run([str(a) for a in args], capture_output=True)
    stdout = result.stdout.decode(errors="replace")
    stderr = result.stderr.decode(errors="replace")
    stream.write("$ " + shlex.join([str(a) for a in args]) + "\n")
    stream.write(stdout)
    if stderr:
        stream.write("[stderr]\n" + stderr)
    stream.write(f"[exit {result.returncode}]\n\n")
    assert result.returncode in allowed, (args, result.returncode, stderr)
    return stdout


def cat_image(image, name):
    return subprocess.run(["unsquashfs", "-cat", str(image), name],
                          capture_output=True, check=True).stdout


images = {
    "stock16": SCRATCH / "stock-34.16-1-16.squashfs",
    "buildroot": ROOT / "out/rootfs.squashfs",
    "tc26": Path("/home/ben/Moggie-wt/tc26/out/rootfs.squashfs"),
}

with (DEST / "containers.txt").open("w") as log:
    for path in sorted(Path("/home/ben/Moggie/dl/stock-firmware").glob("*.upd")):
        data = path.read_bytes()
        log.write(f"INPUT {path} bytes={len(data)} sha256={digest(path)}\n")
        offset = 0
        while offset < len(data):
            magic, kind, size, flags = struct.unpack_from("<4sIII", data, offset)
            assert magic == bytes.fromhex("497f1635") and size >= 16
            assert offset + size <= len(data)
            payload = data[offset + 16:offset + size]
            log.write(f"record=0x{offset:x} type=0x{kind:x} total={size} "
                      f"flags=0x{flags:x} payload=0x{offset+16:x} "
                      f"payload_bytes={len(payload)} prefix={payload[:16].hex()}\n")
            if kind == 4 and path.name.startswith("34.16"):
                images["stock16"].write_bytes(payload)
            if kind == 6 and path.name.startswith("34.16"):
                kernels = re.findall(rb"Linux version [^\x00\r\n]+", payload)
                log.write(f"kernel_version_strings={kernels!r}\n")
            offset += size
        assert offset == len(data)
        log.write(f"EXACT-EOF {offset}\n\n")
    for label, image in images.items():
        log.write(f"IMAGE {label} {image} bytes={image.stat().st_size} sha256={digest(image)}\n")
        capture(log, ["file", image])
        capture(log, ["unsquashfs", "-s", image])
        padded = image.with_name("rootfs.nand.img")
        if label != "stock16" and padded.exists():
            log.write(f"PADDED {padded} bytes={padded.stat().st_size} sha256={digest(padded)}\n")
            raw = image.read_bytes()
            tail = padded.read_bytes()
            log.write(f"prefix_matches={tail[:len(raw)] == raw} "
                      f"padding_bytes={len(tail)-len(raw)} "
                      f"padding_values={sorted(set(tail[len(raw):]))}\n\n")

manifests = {}
pattern = re.compile(r"^(\S+)\s+(\d+/\d+)\s+(.+?)\s+\d{4}-\d\d-\d\d\s+\d\d:\d\d\s+squashfs-root(?:/(.*))?$")
for label, image in images.items():
    listing, _ = command(["unsquashfs", "-lln", image])
    manifests[label] = {}
    for line in listing.splitlines():
        match = pattern.match(line)
        assert match, line
        mode, owner, size, path = match.groups()
        path, _, target = (path or "").partition(" -> ")
        manifests[label]["/" + path] = "\t".join([mode, owner, size.strip(), target or "-"])
    with (DEST / f"{label}-layout.tsv").open("w") as out:
        out.write("path\tmode\tuid/gid\tsize_or_device\tsymlink_target\n")
        for name, attrs in sorted(manifests[label].items()):
            out.write(f"{name}\t{attrs}\n")

for label in ("buildroot", "tc26"):
    with (DEST / f"stock16-vs-{label}.tsv").open("w") as out:
        out.write("path\tstatus\tstock_mode\tstock_uid/gid\tstock_size_or_device\tstock_link\t"
                  "moggie_mode\tmoggie_uid/gid\tmoggie_size_or_device\tmoggie_link\n")
        for name in sorted(manifests["stock16"].keys() | manifests[label].keys()):
            old = manifests["stock16"].get(name)
            new = manifests[label].get(name)
            if old != new:
                status = "added" if old is None else "removed" if new is None else "changed"
                blank = "-\t-\t-\t-"
                out.write(f"{name}\t{status}\t{old or blank}\t{new or blank}\n")

with (DEST / "boot-layout.txt").open("w") as log:
    for label, image in images.items():
        log.write(f"IMAGE {label}\n")
        for name in ["etc/inittab", "etc/Configure", "etc/init.d/rcS", "etc/fstab",
                     "etc/passwd", "etc/group", "etc/moggie-release", "VERSION"]:
            if "/" + name not in manifests[label]:
                continue
            data = cat_image(image, name).decode(errors="replace")
            if name == "etc/passwd":
                data = re.sub(r"(?m)^([^:\n]+):[^:\n]*:", r"\1:<redacted>:", data)
            log.write(f"unsquashfs -cat {image} {name}\n")
            for number, line in enumerate(data.splitlines(), 1):
                log.write(f"{number:4}: {line}\n")
            log.write("\n")
    log.write("FENWAY EXTRACTED TREE (content only; host modes/owners not image metadata)\n")
    for name in ["etc/arch", "etc/soc_arch", "etc/inittab", "etc/Configure", "etc/fstab", "etc/group", "etc/passwd"]:
        data = (STOCK / name).read_text()
        if name == "etc/passwd":
            data = re.sub(r"(?m)^([^:\n]+):[^:\n]*:", r"\1:<redacted>:", data)
        log.write(f"SOURCE {STOCK / name}\n")
        for number, line in enumerate(data.splitlines(), 1):
            log.write(f"{number:4}: {line}\n")
        log.write("\n")

with (DEST / "abi.txt").open("w") as log:
    binaries = []
    selections = {
        "stock16": ["bin/busybox", "lib/libc-2.3.5.so", "lib/ld-2.3.5.so", "lib/libm-2.3.5.so", "modules/audiodev.o", "bin/upgrade"],
        "buildroot": ["bin/busybox", "lib/libc.so.6", "lib/ld.so.1", "usr/sbin/sonosd"],
        "tc26": ["bin/busybox", "lib/libc-2.23.so", "lib/ld-2.23.so", "usr/sbin/sonosd", "usr/sbin/dlna-renderer"],
    }
    for label, names in selections.items():
        for name in names:
            path = SCRATCH / label / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(cat_image(images[label], name))
            binaries.append(path)
    binaries += [STOCK / "lib/libc.so.6", STOCK / "lib/ld.so.1", STOCK / "modules/audiodev.ko",
                 Path("/home/ben/Moggie/out/audiodev.ko")]
    for path in binaries:
        log.write(f"BINARY {path} bytes={path.stat().st_size} sha256={digest(path)}\n")
        capture(log, ["file", path])
        # GNU readelf returns 1 for -n on this old, note-less REL module.
        capture(log, ["readelf", "-h", "-n", "-A", path],
                allowed=(0, 1) if path.suffix == ".o" else (0,))
        phdrs, _ = command(["readelf", "-l", path])
        log.write("$ readelf -l " + str(path) + " (INTERP lines)\n")
        log.write("\n".join(x for x in phdrs.splitlines() if "interpreter" in x or "INTERP" in x) + "\n")
        dynamic, _ = command(["readelf", "-d", path])
        log.write("$ readelf -d " + str(path) + " (NEEDED lines)\n")
        log.write("\n".join(x for x in dynamic.splitlines() if "NEEDED" in x) + "\n")
        strings, _ = command(["strings", "-t", "x", path])
        log.write("$ strings -t x " + str(path) + " (version identity only)\n")
        log.write("\n".join(x for x in strings.splitlines() if re.search(
            r"GNU C Library|stable release version|vermagic=|kernel_version=|Compiled by GNU", x)) + "\n\n")
    for label, compiler in [("Bootlin", BOOTLIN / "bin/powerpc-buildroot-linux-gnu-gcc"),
                            ("tc26", TC26 / "bin/powerpc-buildroot-linux-gnu-gcc")]:
        log.write(f"COMPILER {label}\n")
        capture(log, [compiler, "-v"])
        sysroot, _ = command([compiler, "-print-sysroot"])
        libc = Path(sysroot.strip()) / "lib/libc.so.6"
        log.write(f"SYSROOT {sysroot.strip()} libc_sha256={digest(libc)}\n")
        capture(log, ["readelf", "-n", "-A", libc])
    objdump = BOOTLIN / "bin/powerpc-buildroot-linux-gnu-objdump"
    path = SCRATCH / "stock16/lib/libm-2.3.5.so"
    assembly, _ = command([objdump, "-d", path])
    log.write(f"$ {objdump} -d {path} (first eight FPU instructions)\n")
    log.write("\n".join([x for x in assembly.splitlines() if re.search(r"\b(lfd|stfd|fadd|fmul|fdiv)\b", x)][:8]) + "\n")

# Remove terminal whitespace only, retaining the tool output and its exit code.
for path in DEST.glob("*.txt"):
    path.write_text("\n".join(line.rstrip() for line in path.read_text().splitlines()) + "\n")

print("PASS: captured 3 image manifests, 2 full layout diffs, container offsets, boot content and ABI evidence; no builds")
