#!/usr/bin/env python3
"""Collect bounded, host-only boot/update evidence from pinned stock captures.

No firmware program or script is executed. Only selected non-credential files
are extracted; binaries and filesystem payloads stay in ignored build output.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib

from elftools.elf.elffile import ELFFile
from cryptography.hazmat.primitives.serialization import load_der_public_key


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


def elf_evidence(path, objdump, ranges, out, label):
    data = path.read_bytes()
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        if elf['e_machine'] != 'EM_PPC' or elf.elfclass != 32 or elf.little_endian:
            raise ValueError('expected PPC32 big-endian ELF')
        loads = [dict(va=s['p_vaddr'], offset=s['p_offset'], size=s['p_filesz'])
                 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    def offset(va):
        for segment in loads:
            if segment['va'] <= va < segment['va'] + segment['size']:
                return segment['offset'] + va - segment['va']
        raise ValueError(f'VA {va:#x} has no file backing')
    count = 0
    for name, start, end in ranges:
        command = [str(objdump), '-d', f'--start-address={start:#x}',
                   f'--stop-address={end:#x}', str(path)]
        lines = run(command).decode().splitlines()
        annotated = ['# Command: ' + ' '.join(command),
                     '# VA, raw instruction bytes, instruction; file offset appended']
        for line in lines:
            match = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ){3}[0-9a-f]{2})\s', line)
            if match:
                va = int(match[1], 16)
                file_offset = offset(va)
                if bytes.fromhex(match[2]) != data[file_offset:file_offset + 4]:
                    raise ValueError('objdump bytes disagree with source')
                line += f'  # file {file_offset:#x}'
                count += 1
            annotated.append(line.rstrip())
        (out / f'{label}-{name}.dis.txt').write_text('\n'.join(annotated) + '\n')
    return dict(path=str(path), sha256=digest(data), size=len(data),
                loads=loads, checked_disassembly_rows=count)


def public_anchor(path):
    """Resolve the legacy verifier's static public DER through its PPC GOT slot."""
    data = path.read_bytes()
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        delta = struct.unpack_from('>i', data, 0x6b5c)[0]
        slot = 0x6b6c + delta - 32768
        relocations = [r for s in elf.iter_sections() if s['sh_type'] == 'SHT_RELA'
                       for r in s.iter_relocations() if r['r_offset'] == slot]
        if len(relocations) != 1 or relocations[0]['r_info_type'] != 22:
            raise ValueError('expected one R_PPC_RELATIVE public-key GOT relocation')
        va = relocations[0]['r_addend']
        loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        offset = next(s['p_offset'] + va - s['p_vaddr'] for s in loads
                      if s['p_vaddr'] <= va < s['p_vaddr'] + s['p_filesz'])
    der = data[offset:offset + 162]
    key = load_der_public_key(der)
    return dict(loader_va='0x6b60', anchor_va='0x6b6c', delta=hex(delta),
                got_slot_va=hex(slot), der_va=hex(va), der_file=hex(offset),
                der_size=len(der), sha256=digest(der), bits=key.key_size,
                exponent=key.public_numbers().e,
                scope='Legacy UPD signature key only; envelope trust selection remains separate.')


def embedded_tree(data):
    """Read selected properties from the previously located embedded FDT."""
    base = 0x2e3000
    header = struct.unpack_from('>10I', data, base)
    magic, size, struct_offset, strings_offset = header[:4]
    if magic != 0xd00dfeed or size > len(data) - base:
        raise ValueError('invalid captured FDT')
    strings = base + strings_offset
    position, stack, properties = base + struct_offset, [], []
    while position + 4 <= base + size:
        token = struct.unpack_from('>I', data, position)[0]
        position += 4
        if token == 1:
            end = data.index(b'\0', position, base + size)
            stack.append(data[position:end].decode('ascii'))
            position = (end + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name_offset = struct.unpack_from('>2I', data, position)
            position += 8
            if length > base + size - position:
                raise ValueError('out-of-bounds FDT property')
            name_start = strings + name_offset
            end = data.index(b'\0', name_start, base + size)
            name = data[name_start:end].decode('ascii')
            node = '/'.join(stack) or '/'
            if name in ('compatible', 'bootargs'):
                properties.append(dict(node=node, name=name, offset=hex(position),
                    value=data[position:position + length].rstrip(b'\0').decode('ascii').split('\0')))
            position = (position + length + 3) & ~3
        elif token == 9:
            return dict(offset=hex(base), size=size, properties=properties)
        elif token != 4:
            raise ValueError('unexpected FDT token')
    raise ValueError('unterminated FDT')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--older-package', type=Path, required=True)
    parser.add_argument('--current-package', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--objdump', type=Path, required=True)
    parser.add_argument('--capture-dir', type=Path, required=True)
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
    kernel = (cache / 'kernel.section').read_bytes()
    gzip_offset = kernel.find(b'\x1f\x8b\x08')
    if gzip_offset != 0x5779:
        parser.error('unexpected older kernel compression offset')
    decompressor = zlib.decompressobj(31)
    raw = decompressor.decompress(kernel[gzip_offset:], 32 * 1024 * 1024)
    if not decompressor.eof or len(raw) != 1511544:
        parser.error('unexpected/truncated decompressed kernel')
    (cache / 'kernel.decompressed').write_bytes(raw)
    kernel_evidence = dict(gzip_offset=hex(gzip_offset), size=len(raw), sha256=digest(raw), literals=[])
    for source, data, needles in (
        ('older-wrapper', kernel, (b'root=/dev/nfs rw ip=on',)),
        ('older-decompressed', raw, (b'Linux version ', b'bootsection', b'bootgeneration', b'squashfs: version')),
        ('captured-vmlinux', (args.capture_dir / 'vmlinux.raw').read_bytes(),
         (b'Linux version ', b'fsl,mpc8315erdb', b'mtdparts=')),
    ):
        source_path = {'older-wrapper': cache / 'kernel.section',
                       'older-decompressed': cache / 'kernel.decompressed',
                       'captured-vmlinux': args.capture_dir / 'vmlinux.raw'}[source]
        kernel_evidence.setdefault('sources', {})[source] = dict(
            path=str(source_path), size=len(data), sha256=digest(data))
        for needle in needles:
            pos = data.find(needle)
            if pos < 0:
                raise ValueError(f'missing expected kernel literal: {needle!r}')
            end = data.index(b'\0', pos)
            kernel_evidence['literals'].append(dict(source=source, offset=hex(pos),
                value=data[pos:end].decode('ascii')))
    write_json(out / 'kernels.json', kernel_evidence)
    write_json(out / 'captured-device-tree.json', embedded_tree(
        (args.capture_dir / 'vmlinux.raw').read_bytes()))
    logs = {}
    for name in ('boot-to-linux.log', 'uboot-prompt-2.log', 'uboot-test-suite.log'):
        path = args.capture_dir / 'logs' / name
        data = path.read_bytes()
        logs[name] = dict(path=str(path), size=len(data), sha256=digest(data))
        (out / name.replace('.log', '.txt')).write_text(
            '# Display escapes non-ASCII; original bytes hashed in logs.json\n'
            + ''.join(f'{n:4}: {line}'.rstrip() + '\n' for n, line in
                      enumerate(data.decode('ascii', 'backslashreplace').splitlines(), 1)))
    write_json(out / 'logs.json', logs)
    images = {}
    images['upgrade'] = elf_evidence(cache / 'rootfs/bin/upgrade', args.objdump, (
        ('key-setup', 0x10004890, 0x10004b30),
        ('constructor', 0x10002dbc, 0x10002e14),
        ('install', 0x100057b0, 0x10005dec),
        ('hash-stream', 0x100055d0, 0x10005668),
        ('payload-reader', 0x10006028, 0x100061d4),
        ('legacy-signature', 0x100074e4, 0x100075c8),
        ('envelope-signature', 0x100075c8, 0x1000778c),
        ('record-dispatch', 0x1000778c, 0x10007c80),
        ('validate-install', 0x10007c68, 0x10007d38),
        ('slot-destination', 0x10009878, 0x10009a28),
    ), out, 'upgrade')
    images['crypto'] = elf_evidence(cache / 'rootfs/lib/libsonoscrypto.so.1', args.objdump, (
        ('hash', 0x5368, 0x55e0), ('rsa', 0x6a0c, 0x6b5c),
        ('legacy-key', 0x6b60, 0x6ba0),
        ('envelope-serialization', 0x7b04, 0x7f7c),
        ('envelope-parser', 0x8448, 0x87d4),
        ('envelope-entry', 0x8b10, 0x8b48),
        ('envelope-verifier', 0x8870, 0x8b10),
    ), out, 'crypto')
    write_json(out / 'images.json', images)
    write_json(out / 'legacy-public-anchor.json', public_anchor(
        cache / 'rootfs/lib/libsonoscrypto.so.1'))
    upgrade_data = (cache / 'rootfs/bin/upgrade').read_bytes()
    write_json(out / 'record-handlers.json', [dict(file=hex(offset),
        words=[hex(value) for value in struct.unpack_from('>4I', upgrade_data, offset)])
        for offset in range(0x1a2c0, 0x1a3a0, 16)])
    write_json(out / 'toolchain.json', dict(objdump=str(args.objdump),
        version=run([str(args.objdump), '--version']).decode().splitlines()[0]))
    write_json(out / 'packages.json', packages)
    write_json(out / 'extraction.json', dict(files=manifest, missing=missing,
               allowlist=list(TEXTS + BINARIES), rootfs_record=root_record,
               script_record=r, kernel_record=r_kernel,
               boundary='Host unsquashfs only; no account data, devices or target execution.'))
    print(f'PASS: 2 pinned UPDs parsed to exact EOF; {len(manifest)} allowlisted files extracted; {len(missing)} absent')
    print(f'PASS: {sum(im["checked_disassembly_rows"] for im in images.values())} disassembly rows match ELF file bytes')


if __name__ == '__main__':
    main()
