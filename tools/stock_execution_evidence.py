#!/usr/bin/env python3
"""Collect bounded, host-only boot/update evidence from pinned stock captures.

No firmware program or script is executed. Only selected non-credential files
are extracted; binaries and filesystem payloads stay in ignored build output.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


PACKAGES = {
    '34.16-37101-1-16': '84632e02bdce626bf10ae846839e90d66e510a627320fff8690c2bdb998733f2',
    '57.23-74170-1-16': 'e19a7bb28f2d940346c605ccfcff81f8d9a7dd1771ee991841ac1f9e07223ad4',
}
TEXTS = (
    'etc/Configure', 'etc/inittab', 'etc/fstab', 'etc/arch', 'etc/arch_attrs',
    'etc/soc_arch', 'etc/runanacapa', 'etc/runmdns', 'etc/runchrony',
    'etc/runsddp', 'etc/init.d/Srandom', 'etc/init.d/Krandom',
    'etc/init.d/rcK', 'opt/bin/anacapactl', 'build.properties',
    'bin/repartition.sh',
)
BINARIES = ('bin/upgrade', 'lib/libflash.so.1', 'lib/libsonoscrypto.so.1',
            'bin/busybox', 'bin/formatjffs')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse_upd(data):
    """Validate every adjacent record, including EOF; do not interpret signatures."""
    records, offset = [], 0
    while offset < len(data):
        if len(data) - offset < 16:
            raise ValueError(f'truncated header at {offset:#x}')
        magic, kind, size, flags = struct.unpack_from('<4I', data, offset)
        if magic != 0x35167f49 or size < 16 or size > len(data) - offset:
            raise ValueError(f'invalid record at {offset:#x}')
        payload = data[offset + 16:offset + size]
        records.append(dict(offset=hex(offset), type=hex(kind), size=size,
                            flags=hex(flags), payload_offset=hex(offset + 16),
                            payload_size=len(payload), prefix=payload[:16].hex(),
                            sha256=digest(payload)))
        offset += size
    if not records:
        raise ValueError('empty package')
    return records


def run(command):
    result = subprocess.run(command, capture_output=True, check=True, timeout=30)
    return result.stdout


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--older-package', type=Path, required=True)
    parser.add_argument('--current-package', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    workspace = Path(__file__).resolve().parents[1]
    cache, out = args.cache.resolve(), args.out.resolve()
    if not cache.is_relative_to(workspace / 'build'):
        parser.error('cache must be inside this worktree/build')
    if out == workspace or not out.is_relative_to(workspace):
        parser.error('output must be inside this worktree')
    packages = {}
    for name, path in zip(PACKAGES, (args.older_package, args.current_package)):
        data = path.read_bytes()
        if digest(data) != PACKAGES[name]:
            parser.error(f'unexpected source hash: {name}')
        packages[name] = dict(path=str(path.resolve()), size=len(data),
                              sha256=digest(data), records=parse_upd(data))
        if name.startswith('34.'):
            older_data = data
    root_records = [r for r in packages['34.16-37101-1-16']['records']
                    if r['type'] == '0x4']
    if len(root_records) != 1 or root_records[0]['flags'] != '0x0':
        parser.error('expected exactly one plain rootfs')
    root_record = root_records[0]
    start = int(root_record['payload_offset'], 16)
    rootfs = older_data[start:start + root_record['payload_size']]
    if rootfs[:4] != b'sqsh':
        parser.error('expected big-endian SquashFS')
    cache.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    section = cache / 'rootfs.section'
    section.write_bytes(rootfs)
    (out / 'filesystem.txt').write_bytes(run(['unsquashfs', '-s', str(section)]))
    # Capture symlink metadata without following links or extracting device nodes.
    (out / 'aliases.txt').write_bytes(run(['unsquashfs', '-ll', str(section),
                                         'tmp', 'var', 'sbin/init', 'dev/nandjffs']))
    manifest, missing = {}, []
    for relative in TEXTS + BINARIES:
        command = ['unsquashfs', '-cat', str(section), relative]
        result = subprocess.run(command, capture_output=True, timeout=30)
        if result.returncode:
            if b'cat: no matches for /' + relative.encode() not in result.stderr:
                raise RuntimeError(f'extraction failed for {relative}: {result.stderr!r}')
            missing.append(dict(path=relative, error=result.stderr.decode().strip()))
            continue
        data = result.stdout
        dest = cache / 'rootfs' / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        manifest[relative] = dict(size=len(data), sha256=digest(data))
        if relative in TEXTS:
            (out / (relative.replace('/', '-') + '.txt')).write_text(
                '# Stock file: ' + relative + '; display strips trailing whitespace; hashes cover original bytes\n'
                + ''.join(f'{n:4}: {line}'.rstrip() + '\n' for n, line in
                          enumerate(data.decode().splitlines(), 1)))
    # The package script is data only. Never invoke a target interpreter on it.
    scripts = [r for r in packages['34.16-37101-1-16']['records']
               if r['type'] == '0x3' and r['flags'] == '0x0']
    if len(scripts) != 1:
        parser.error('expected one plain installation-script record')
    r = scripts[0]
    start = int(r['payload_offset'], 16)
    script = older_data[start:start + r['payload_size']]
    (out / 'package-install-script.txt').write_text(
        '# Data only; never executed. Source: UPD type 3\n'
        + ''.join(f'{n:4}: {line}'.rstrip() + '\n' for n, line in
                  enumerate(script.decode().splitlines(), 1)))
    kernel_records = [r for r in packages['34.16-37101-1-16']['records']
                      if r['type'] == '0x6' and r['flags'] == '0x0']
    if len(kernel_records) != 1:
        parser.error('expected one plain kernel record')
    r_kernel = kernel_records[0]
    start = int(r_kernel['payload_offset'], 16)
    (cache / 'kernel.section').write_bytes(
        older_data[start:start + r_kernel['payload_size']])
    write_json(out / 'packages.json', packages)
    write_json(out / 'extraction.json', dict(files=manifest, missing=missing,
               allowlist=list(TEXTS + BINARIES), rootfs_record=root_record,
               script_record=r, kernel_record=r_kernel,
               boundary='Host unsquashfs only; no account data, devices or target execution.'))
    print(f'PASS: 2 pinned UPDs parsed to exact EOF; {len(manifest)} allowlisted files extracted; {len(missing)} absent')


if __name__ == '__main__':
    main()
