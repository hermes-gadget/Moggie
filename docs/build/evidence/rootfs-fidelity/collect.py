#!/usr/bin/env python3
"""Read-only evidence capture; never builds, mounts, runs target code, or writes inputs.

Run from the repo root. Only this evidence directory and out/fidelity-audit are
written. Stock password fields are redacted; shadow files are never opened.
"""
import hashlib
import gzip
import os
import posixpath
from pathlib import Path
import re
import shlex
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[4]
os.chdir(ROOT)
os.environ.update(LC_ALL="C", TZ="UTC")
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
    cap = int(re.search(r"^ROOTFS_MAX_BYTES=(\d+)", (ROOT / "buildroot.lock").read_text(), re.M)[1])
    for label in ("buildroot", "tc26"):
        padded_size = images[label].with_name("rootfs.nand.img").stat().st_size
        log.write(f"DECLARED_CAP_CHECK {label} cap={cap} padded={padded_size} "
                  f"headroom={cap-padded_size} fits_declared_cap={padded_size <= cap} actual_stock_slot=UNVERIFIED\n")

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
    capture(log, [objdump, "-d", "--start-address=0xa478", "--stop-address=0xa4a8", path])
    for label in ("buildroot", "tc26"):
        path = SCRATCH / label / "usr/sbin/sonosd"
        capture(log, ["readelf", "--version-info", path])

with (DEST / "stock-kernel.txt").open("w") as log:
    package = Path("/home/ben/Moggie/dl/stock-firmware/34.16-37101-1-16.upd")
    payload = package.read_bytes()[0xd3f:0xadd3f]
    for match in re.finditer(b"\x1f\x8b\x08", payload):
        try:
            raw = zlib.decompress(payload[match.start():], 31)
        except zlib.error:
            continue
        log.write(f"SOURCE {package} record6_payload=0xd3f gzip_relative=0x{match.start():x} "
                  f"gzip_absolute=0x{0xd3f+match.start():x} inflated_bytes={len(raw)} "
                  f"inflated_sha256={hashlib.sha256(raw).hexdigest()}\n")
        path = SCRATCH / "stock16/kernel-inflated.bin"
        path.write_bytes(raw)
        for string in re.finditer(rb"[\x20-\x7e]{8,}", raw):
            if re.search(rb"Linux version 2|squashfs: version|trying to mount newer|Please update your kernel", string.group()):
                log.write(f"inflated_offset=0x{string.start():x} {string.group().decode()}\n")
    for name in ("rootfs", "kernel"):
        path = BENCH / f"dump/57.10-25140/{name}.gz"
        raw = gzip.decompress(path.read_bytes())
        log.write(f"SOURCE {path} sha256={digest(path)} outer_inflated_bytes={len(raw)} "
                  f"prefix={raw[:16].hex()}\n")
        if name == "rootfs":
            magic, size, flags = struct.unpack_from(">III", raw)
            assert magic == 0x28cd3d45
            log.write(f"CRAMFS big_endian size={size} flags=0x{flags:x} signature={raw[16:32]!r}\n")
        else:
            assert raw[:4] == bytes.fromhex("27051956")
            size = struct.unpack_from(">I", raw, 12)[0]
            log.write(f"UIMAGE payload_size={size} name={raw[32:64]!r}\n")
            kernel = zlib.decompress(raw[64:64+size], 31)
            log.write(f"kernel_inflated_bytes={len(kernel)} sha256={hashlib.sha256(kernel).hexdigest()}\n")
            for string in re.finditer(rb"[\x20-\x7e]{8,}", kernel):
                if re.search(rb"Linux version 2|fsl,mpc8315erdb|cramfs: wrong|squashfs", string.group()):
                    log.write(f"inflated_offset=0x{string.start():x} {string.group().decode()}\n")
            log.write(f"literal_squashfs_count={kernel.lower().count(b'squashfs')}\n")
    capture(log, [BOOTLIN / "bin/powerpc-buildroot-linux-gnu-objdump", "-D",
                  "-b", "binary", "-m", "powerpc:common", "-EB",
                  "--start-address=0x65758", "--stop-address=0x65844",
                  SCRATCH / "stock16/kernel-inflated.bin"])

with (DEST / "elf-closure.txt").open("w") as log:
    for label in ("buildroot", "tc26"):
        manifest = manifests[label]

        def exists_in_image(path):
            for _ in range(40):
                attrs = manifest.get(path)
                if attrs is None:
                    return False
                if not attrs.startswith("l"):
                    return True
                target = attrs.split("\t")[-1]
                path = posixpath.normpath(posixpath.join(posixpath.dirname(path), target))
            return False

        count = 0
        missing = []
        log.write(f"IMAGE {label}: regular ELF files in bin/sbin/lib/usr/bin/usr/sbin/usr/lib; default loader paths only\n")
        for name, attrs in sorted(manifest.items()):
            if not attrs.startswith("-") or not name.startswith(("/bin/", "/sbin/", "/lib/", "/usr/bin/", "/usr/sbin/", "/usr/lib/")):
                continue
            data = cat_image(images[label], name[1:])
            if data[:4] != b"\x7fELF":
                continue
            path = SCRATCH / "elf-current"
            path.write_bytes(data)
            dynamic, _ = command(["readelf", "-d", path])
            phdr, _ = command(["readelf", "-l", path])
            needs = re.findall(r"Shared library: \[(.*?)\]", dynamic)
            interpreters = re.findall(r"Requesting program interpreter: (.*?)\]", phdr)
            versions, _ = command(["readelf", "--version-info", path])
            needs_section = versions.partition("Version needs section")[2]
            required = sorted(set(re.findall(r"Name: (GLIBC_[\d.]+)", needs_section)))
            count += 1
            log.write(f"{name} sha256={hashlib.sha256(data).hexdigest()} "
                      f"NEEDED={needs!r} INTERP={interpreters!r} GLIBC_versions={required!r}\n")
            for dep in needs:
                if not any(exists_in_image(f"{prefix}/{dep}") for prefix in ("/lib", "/usr/lib")):
                    missing.append((name, dep))
            missing.extend((name, dep) for dep in interpreters if not exists_in_image(dep))
        log.write(f"RESULT {label} ELF_count={count} missing_default_path_dependencies={missing!r}\n\n")

with (DEST / "config-review.txt").open("w") as log:
    capture(log, ["git", "rev-parse", "origin/main"])
    capture(log, ["git", "ls-remote", "--symref", "origin", "HEAD"])
    log.write("READ-ONLY main checkout out/ inventory (directory sizes are host metadata)\n")
    for path in sorted(Path("/home/ben/Moggie/out").iterdir()):
        log.write(f"{path.name} {'directory' if path.is_dir() else 'file'} {path.stat().st_size}\n")
    excerpts = [
        (ROOT / "buildroot.lock", 17, 42),
        (ROOT / "toolchain.lock", 5, 39),
        (ROOT / "buildroot/configs/moggie_powerpc_defconfig", 7, 67),
        (ROOT / "buildroot/configs/moggie_powerpc_defconfig", 120, 130),
        (ROOT / "scripts/build-rootfs.sh", 123, 146),
        (ROOT / "scripts/build-rootfs.sh", 204, 254),
        (ROOT / "buildroot/board/moggie/post-build.sh", 29, 64),
        (ROOT / "buildroot/board/moggie/post-build.sh", 76, 90),
        (ROOT / "buildroot/board/moggie/device_table_dev.txt", 1, 37),
        (ROOT / "scripts/build-kernel.sh", 79, 94),
        (ROOT / "firmware/bootchooser/init", 56, 90),
        (ROOT / "firmware/bootchooser/init", 169, 186),
        (ROOT / "firmware/bootchooser/init", 219, 251),
        (ROOT / "firmware/updater/slotdev.h", 20, 24),
        (ROOT / "firmware/updater/slotdev.c", 143, 150),
        (ROOT / "firmware/updater/updater.c", 51, 57),
        (Path("/home/ben/sonos-bench/logs/uboot-first-boot-2026-10-27.log"), 1, 26),
        (Path("/home/ben/Moggie/src/linux-2.6.35/arch/powerpc/configs/fenway_op_defconfig"), 224, 249),
        (Path("/home/ben/Moggie/src/linux-2.6.35/arch/powerpc/configs/fenway_op_defconfig"), 1128, 1135),
        (Path("/home/ben/Moggie/src/linux-2.6.35/drivers/mtd/nand/sonos_nand_oob.h"), 88, 161),
        (Path("/home/ben/Moggie/src/linux-2.6.35/drivers/mtd/nand/virtual_block.c"), 720, 765),
        (Path("/home/ben/Moggie/src/linux-2.6.35/fs/squashfs/super.c"), 51, 74),
    ]
    for path, first, last in excerpts:
        log.write(f"\nSOURCE {path} sha256={digest(path)} lines={first}-{last}\n")
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if first <= number <= last:
                log.write(f"{number:4}: {line}\n")
    for label, base in [("buildroot", ROOT / "out/buildroot"),
                        ("tc26", Path("/home/ben/Moggie-wt/tc26/out/buildroot"))]:
        config = base / ".config"
        log.write(f"\nEFFECTIVE {label} {config} sha256={digest(config)}\n")
        for number, line in enumerate(config.read_text().splitlines(), 1):
            if line.startswith(("BR2_TOOLCHAIN_EXTERNAL_", "BR2_TARGET_ROOTFS_SQUASHFS", "BR2_SOFT_FLOAT", "BR2_ENDIAN")):
                log.write(f"{number:4}: {line}\n")
    log.write("\nVendor updater string offsets; diagnostics alone are not a complete control-flow proof\n")
    for path in (SCRATCH / "stock16/bin/upgrade", STOCK / "bin/upgrade"):
        log.write(f"SOURCE {path} sha256={digest(path)}\n")
        strings, _ = command(["strings", "-t", "x", path])
        for line in strings.splitlines():
            if re.search(r"read_hdr: bad magic|Sonos signature|No valid signature|incompatible version|size of partition|bootsection|bootgeneration", line):
                log.write(line + "\n")

# Remove terminal whitespace only, retaining the tool output and its exit code.
for path in DEST.glob("*.txt"):
    path.write_text("\n".join(line.rstrip() for line in path.read_text().splitlines()).rstrip() + "\n")

print("PASS: captured 3 image manifests, 2 full layout diffs, container offsets, boot content and ABI evidence; no builds")
