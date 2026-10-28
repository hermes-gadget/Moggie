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
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives import hashes
from cryptography.exceptions import InvalidSignature


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
            'bin/busybox', 'bin/formatjffs', 'modules/chk_driver.o',
            'opt/bin/anacapad')


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
    extra = {}
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        if elf['e_machine'] != 'EM_PPC' or elf.elfclass != 32 or elf.little_endian:
            raise ValueError('expected PPC32 big-endian ELF')
        if elf['e_type'] == 'ET_REL':
            section = elf.get_section_by_name('.text')
            loads = [dict(va=section['sh_addr'], offset=section['sh_offset'], size=section['sh_size'])]
            extra = dict(format='relocatable', load_section='.text')
            relocations = []
            for section in elf.iter_sections():
                if section['sh_type'] != 'SHT_RELA':
                    continue
                symbols = elf.get_section(section['sh_link'])
                for relocation in section.iter_relocations():
                    symbol = symbols.get_symbol(relocation['r_info_sym'])
                    relocations.append(dict(section=section.name, offset=hex(relocation['r_offset']),
                        type=relocation['r_info_type'], addend=relocation['r_addend'],
                        symbol=symbol.name, symbol_section=symbol['st_shndx'],
                        symbol_value=hex(symbol['st_value']), symbol_size=symbol['st_size']))
            write_json(out / f'{label}-relocations.json', relocations)
        else:
            loads = [dict(va=s['p_vaddr'], offset=s['p_offset'], size=s['p_filesz'])
                     for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    count = disassembly_evidence(path, objdump, ranges, loads, out, label,
                                section=extra.get('load_section'))
    return dict(path=str(path), sha256=digest(data), size=len(data),
                loads=loads, checked_disassembly_rows=count, **extra)


def disassembly_evidence(path, objdump, ranges, loads, out, label, raw_base=None, section=None):
    data = path.read_bytes()
    def offset(va):
        for segment in loads:
            if segment['va'] <= va and va + 4 <= segment['va'] + segment['size']:
                return segment['offset'] + va - segment['va']
        raise ValueError(f'VA {va:#x} has no four-byte file backing')
    count = 0
    for name, start, end in ranges:
        options = ['-d'] if raw_base is None else [
            '-D', '-b', 'binary', '-m', 'powerpc:common', '-EB', f'--adjust-vma={raw_base:#x}']
        if section is not None:
            options = ['-dr', '-j', section]
        command = [str(objdump), *options, f'--start-address={start:#x}',
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
    return count


def raw_kernel_evidence(path, objdump, out):
    data = path.read_bytes()
    anchors, bases = [], set()
    for pair_offset, name in ((0x15ca48, 'default_mtd_readv'),
                              (0x15ca50, 'default_mtd_writev'),
                              (0x15ca70, 'get_mtd_device'),
                              (0x15ca80, 'add_mtd_device'),
                              (0x15cab8, 'add_mtd_partitions'),
                              (0x15cb18, 'nand_scan')):
        function_va, name_va = struct.unpack_from('>II', data, pair_offset)
        name_offset = data.index(name.encode() + b'\0')
        base = name_va - name_offset
        if base != 0xc0000000 or not base <= function_va < base + len(data):
            raise ValueError('inconsistent raw-kernel export anchor')
        bases.add(base)
        anchors.append(dict(pair_file=hex(pair_offset), function_va=hex(function_va),
                            name_va=hex(name_va), name_file=hex(name_offset), name=name))
    if len(bases) != 1:
        raise ValueError('raw-kernel anchors disagree')
    base = bases.pop()
    loads = [dict(va=base, offset=0, size=len(data))]
    rows = disassembly_evidence(path, objdump, (
        ('ioctl-dispatch', base + 0xb7318, base + 0xb748c),
        ('oob-write', base + 0xb7728, base + 0xb78dc),
        ('oob-read', base + 0xb78dc, base + 0xb7aa8),
        ('vendor-ioctls', base + 0xb7bf0, base + 0xb7cb4),
        ('nand-callbacks', base + 0xbe490, base + 0xbe598),
        ('nand-defaults', base + 0xbdb88, base + 0xbdd48),
        ('char-write', base + 0xb7070, base + 0xb7244),
        ('nand-write-wrapper', base + 0xbcd0c, base + 0xbcd38),
        ('nand-data-write', base + 0xbc9c0, base + 0xbcd0c),
        ('nand-page-program', base + 0xbb860, base + 0xbbafc),
        ('nand-data-verify', base + 0xbbafc, base + 0xbbd4c),
        ('nand-verify-buffer', base + 0xbaa18, base + 0xbab64),
        ('partition-callbacks', base + 0xb6660, base + 0xb68d8),
        ('partition-free-adjust', base + 0xb5608, base + 0xb56c4),
        ('partition-read-cache', base + 0xb56c4, base + 0xb58d8),
        ('partition-snapshot', base + 0xb58d8, base + 0xb5a8c),
        ('nand-oob-program', base + 0xbb268, base + 0xbb4a0),
        ('rincon-partition', base + 0xc0178, base + 0xc036c),
        ('board-nand-init', base + 0x16c1d4, base + 0x16c308),
        ('partition-discovery', base + 0xc036c, base + 0xc06f8),
        ('boot-section-choice', base + 0xc06f8, base + 0xc0998),
    ), loads, out, 'kernel', raw_base=base)
    sysctls = []
    for entry_file, name in ((0x14db70, 'bootsection'), (0x14db9c, 'bootgeneration')):
        number, name_va, value_va, size, mode = struct.unpack_from('>5I', data, entry_file)
        name_file = name_va - base
        if data[name_file:data.index(b'\0', name_file)].decode() != name:
            raise ValueError('unexpected boot sysctl name')
        sysctls.append(dict(entry_file=hex(entry_file), number=number,
            name=name, name_va=hex(name_va), name_file=hex(name_file),
            value_va=hex(value_va), size=size, mode=oct(mode)))
    write_json(out / 'kernel-map.json', dict(base=hex(base), anchors=anchors, sysctls=sysctls,
        boundary='Consistent link labels recovered from raw export pointers; no ELF headers or live load address asserted.'))
    return dict(path=str(path), sha256=digest(data), size=len(data), loads=loads,
                format='raw', checked_disassembly_rows=rows)


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


def encrypted_headers(data, records):
    """Inspect framing only; never decrypt recipient blobs or payloads."""
    headers = []
    for record in records:
        if record['flags'] != '0x4':
            continue
        base, size = int(record['payload_offset'], 16), record['payload_size']
        payload = data[base:base + size]
        magic, length, version, flags, attributes = struct.unpack_from('>IIHHI', payload)
        if magic != 0x886499ca or not 17 <= length <= len(payload) or version != 1 or attributes:
            raise ValueError('unexpected pinned encrypted-envelope header')
        count = payload[16]
        if not 1 <= count <= 2:
            raise ValueError('unexpected recipient count')
        position, recipients = 17, []
        for _ in range(count):
            if position + 5 > length:
                raise ValueError('truncated recipient')
            id_size = struct.unpack_from('>I', payload, position)[0]
            id_type = payload[position + 4]
            position += 5
            if id_size > 2047 or position + id_size + 8 > length:
                raise ValueError('invalid recipient identifier size')
            position += id_size
            algorithm, wrapped_size = struct.unpack_from('>II', payload, position)
            position += 8
            if wrapped_size > 512 or position + wrapped_size > length:
                raise ValueError('invalid wrapped-data size')
            recipients.append(dict(identifier_size=id_size, identifier_type=id_type,
                                   algorithm=algorithm, wrapped_size=wrapped_size))
            position += wrapped_size
        if position + 8 != length:
            raise ValueError('unexpected envelope trailer size')
        cipher, scalar = struct.unpack_from('>II', payload, position)
        headers.append(dict(type=record['type'], payload_file=hex(base), magic=hex(magic),
            header_size=length, version=version, flags=flags, attributes=attributes,
            recipients=recipients, cipher_id=cipher, trailing_scalar=scalar,
            data_file=hex(base + length), data_size=len(payload) - length,
            boundary='Structural metadata only; no key/ciphertext contents or decryption.'))
    return headers


def envelope_anchors(path):
    data = path.read_bytes()
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        def offset(va):
            return next(s['p_offset'] + va - s['p_vaddr'] for s in loads
                        if s['p_vaddr'] <= va < s['p_vaddr'] + s['p_filesz'])
        relocations = {r['r_offset']: r['r_addend'] for s in elf.iter_sections()
                       if s['sh_type'] == 'SHT_RELA' for r in s.iter_relocations()
                       if r['r_info_type'] == 22}
        delta = struct.unpack_from('>i', data, offset(0x70e4))[0]
        slot = 0x70f4 + delta - 32768
        table = relocations[slot]
        anchors = []
        for index in range(2):
            row = table + index * 32
            position = offset(row)
            mask = struct.unpack_from('>I', data, position)[0]
            der_va = relocations[row + 24]
            length = struct.unpack_from('>I', data, position + 28)[0]
            der = data[offset(der_va):offset(der_va) + length]
            # Public-key parsing deliberately rejects private-key encodings.
            key = load_der_public_key(der)
            anchors.append(dict(table_index=index, row_va=hex(row), row_file=hex(position),
                deny_mask=mask, identifier=data[position + 4:position + 24].hex(),
                der_va=hex(der_va), der_file=hex(offset(der_va)), der_size=length,
                sha256=digest(der), bits=key.key_size, exponent=key.public_numbers().e))
    return dict(selector_va='0x70e8', got_slot_va=hex(slot), table_va=hex(table),
                table_file=hex(offset(table)), anchors=anchors)


def legacy_signature(data, records, crypto, anchor):
    footer = records[-1]
    if footer['type'] != '0xffffffff' or footer['payload_size'] != 128:
        raise ValueError('unexpected legacy signature record')
    end, signature_start = int(footer['offset'], 16), int(footer['payload_offset'], 16)
    offset = int(anchor['der_file'], 16)
    key = load_der_public_key(crypto[offset:offset + anchor['der_size']])
    signature = data[signature_start:]
    key.verify(signature, data[:end], padding.PKCS1v15(), hashes.MD5())
    altered = bytes([data[0] ^ 1]) + data[1:end]
    try:
        key.verify(signature, altered, padding.PKCS1v15(), hashes.MD5())
    except InvalidSignature:
        pass
    else:
        raise ValueError('altered signed prefix accepted')
    return dict(package_sha256=digest(data), prefix_start=0, prefix_end=hex(end),
                prefix_sha256=digest(data[:end]), prefix_md5=hashlib.md5(data[:end]).hexdigest(),
                signature_file=hex(signature_start), signature_size=len(signature),
                scheme='RSASSA-PKCS1-v1_5 / MD5', public_der_sha256=anchor['sha256'],
                verified=True, altered_prefix_rejected=True,
                boundary='Offline cryptographic validity of these bytes; no device acceptance, model compatibility or complete caller proof.')


def direct_calls(path, targets=None):
    if targets is None:
        targets = {0x1000778c: 'parser', 0x100057b0: 'install'}
    calls = []
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        for section in elf.iter_sections():
            if not section['sh_flags'] & 4:
                continue
            data = section.data()
            for position in range(0, len(data) - 3, 4):
                word = struct.unpack_from('>I', data, position)[0]
                if word & 0xfc000003 != 0x48000001:
                    continue
                delta = word & 0x03fffffc
                if delta & 0x02000000:
                    delta -= 0x04000000
                va = section['sh_addr'] + position
                target = (va + delta) & 0xffffffff
                if target in targets:
                    calls.append(dict(caller_va=hex(va), caller_file=hex(section['sh_offset'] + position),
                                      target_va=hex(target), target=targets[target], bytes=f'{word:08x}'))
    return dict(calls=calls, boundary='Direct relative BL encodings in executable ELF sections only; indirect and virtual orchestration excluded.')


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
        else:
            current_data = data
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
        ('phase-state', 0x10002eb8, 0x10003038),
        ('network-phases', 0x10003038, 0x100031e4),
        ('phase-dispatch', 0x100031e4, 0x100032b0),
        ('network-phase-gates', 0x10004248, 0x100042c0),
        ('install', 0x100057b0, 0x10005dec),
        ('hash-stream', 0x100055d0, 0x10005668),
        ('payload-reader', 0x10006028, 0x100061d4),
        ('header-reader', 0x1000626c, 0x100063fc),
        ('unknown-record', 0x10006a64, 0x10006b2c),
        ('encrypted-payload', 0x100063fc, 0x100069b0),
        ('legacy-signature', 0x100074e4, 0x100075c8),
        ('envelope-signature', 0x100075c8, 0x1000778c),
        ('record-dispatch', 0x1000778c, 0x10007c80),
        ('validate-install', 0x10007c68, 0x10007d38),
        ('slot-destination', 0x10009878, 0x10009a28),
        ('slot-program', 0x100085c8, 0x10008aa4),
        ('logical-block-map', 0x10008178, 0x100081e0),
        ('partition-table-write', 0x10008aa4, 0x10008ba4),
        ('partition-table-compare', 0x10009634, 0x100096fc),
        ('partition-adjust', 0x10009748, 0x10009864),
        ('range-invalidate', 0x10008c88, 0x10008f10),
        ('commit-sequence', 0x10009bec, 0x1000a048),
        ('nand-writer', 0x10008f10, 0x10009530),
    ), out, 'upgrade')
    images['crypto'] = elf_evidence(cache / 'rootfs/lib/libsonoscrypto.so.1', args.objdump, (
        ('hash', 0x5368, 0x55e0), ('rsa', 0x6a0c, 0x6b5c),
        ('legacy-key', 0x6b60, 0x6ba0),
        ('envelope-serialization', 0x7b04, 0x7f7c),
        ('envelope-parser', 0x8448, 0x87d4),
        ('envelope-entry', 0x8b10, 0x8b48),
        ('envelope-verifier', 0x8870, 0x8b10),
        ('envelope-key-selector', 0x70e8, 0x7274),
        ('encrypted-parser', 0xa7bc, 0xaae0),
        ('encrypted-setup', 0xad5c, 0xaeec),
        ('rsa-decrypt', 0x5ac4, 0x5c18),
    ), out, 'crypto')
    images['kernel'] = raw_kernel_evidence(cache / 'kernel.decompressed', args.objdump, out)
    images['driver'] = elf_evidence(cache / 'rootfs/modules/chk_driver.o', args.objdump,
        (('text', 0, 0x298),), out, 'driver')
    images['anacapa'] = elf_evidence(cache / 'rootfs/opt/bin/anacapad', args.objdump, (
        ('chk-startup', 0x10005c68, 0x10005d7c),
        ('rdm-reset-handler', 0x1000b06c, 0x1000b0d0),
        ('rdm-watchdog', 0x10056c5c, 0x10056ca4),
        ('reboot', 0x101c6510, 0x101c655c),
        ('watchdog-log-copy', 0x102e3bcc, 0x102e3cb0),
        ('watchdog-action', 0x102e3d64, 0x102e3fe4),
        ('watchdog-supervision', 0x102e4258, 0x102e4360),
        ('watchdog-setup', 0x102e4808, 0x102e4950),
        ('watchdog-register', 0x102e4ab4, 0x102e4bcc),
        ('watchdog-chsnk-site', 0x100aa210, 0x100aa254),
        ('watchdog-chsrc-site', 0x100bd050, 0x100bd090),
        ('watchdog-ddt-site', 0x101ba17c, 0x101ba1b0),
        ('watchdog-upnpeventing-site', 0x101deb9c, 0x101debd0),
        ('watchdog-dynamic-site', 0x10259d98, 0x10259dd4),
        ('watchdog-anacapa-site', 0x102e3734, 0x102e3778),
        ('watchdog-chsnk', 0x100a72f4, 0x100a73b4),
        ('watchdog-chsrc', 0x100ac8a4, 0x100ac964),
        ('watchdog-ddt', 0x101b9fc0, 0x101ba060),
        ('watchdog-upnpeventing', 0x101deaf8, 0x101deb9c),
        ('watchdog-dynamic', 0x1025a084, 0x1025a110),
        ('watchdog-anacapa', 0x102e38f8, 0x102e3a6c),
    ), out, 'anacapa')
    write_json(out / 'watchdog-direct-callers.json', direct_calls(
        cache / 'rootfs/opt/bin/anacapad', {
            0x102e4ab4: 'watchdog-register', 0x102e4808: 'watchdog-setup',
            0x102e3d64: 'watchdog-action'}))
    literals = []
    app = (cache / 'rootfs/opt/bin/anacapad').read_bytes()
    for needle in (b'/dev/chk', b'RDM watchdog handler triggering reset',
                   b'RDM watchdog triggering reset', b'%s/watchdog.log',
                   b'%s/watchdog.dmesg', b'/sbin/reboot', b'WATCHDOG: %s unresponsive! (UTC %s)',
                   b'chsnk', b'chsrc', b'ddt', b'upnpeventing', b'anacapa'):
        position = app.index(needle + b'\0')
        literals.append(dict(source='anacapa', file=hex(position), value=needle.decode()))
    write_json(out / 'supervision-literals.json', literals)
    write_json(out / 'images.json', images)
    anchor = public_anchor(cache / 'rootfs/lib/libsonoscrypto.so.1')
    write_json(out / 'legacy-public-anchor.json', anchor)
    write_json(out / 'envelope-public-anchors.json', envelope_anchors(
        cache / 'rootfs/lib/libsonoscrypto.so.1'))
    write_json(out / 'encrypted-headers.json', encrypted_headers(current_data,
        packages['57.23-74170-1-16']['records']))
    write_json(out / 'legacy-signature-validation.json', legacy_signature(older_data,
        packages['34.16-37101-1-16']['records'],
        (cache / 'rootfs/lib/libsonoscrypto.so.1').read_bytes(), anchor))
    write_json(out / 'direct-callers.json', direct_calls(cache / 'rootfs/bin/upgrade'))
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
    print(f'PASS: {sum(im["checked_disassembly_rows"] for im in images.values())} disassembly rows match source image bytes')
    print('PASS: older UPD legacy RSA/MD5 signature verifies exact prefix; altered prefix rejected')


if __name__ == '__main__':
    main()
