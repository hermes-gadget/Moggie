#!/usr/bin/env python3
"""Independently check the saved execution evidence against original source bytes.

This checks transcription/provenance and one captured legacy signature, not
reverse-engineering conclusions or device behavior. No target code is executed.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from elftools.elf.elffile import ELFFile
from cryptography.hazmat.primitives.serialization import load_der_public_key
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives import hashes


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--document', type=Path, required=True)
    args = parser.parse_args()
    root = args.evidence
    read = lambda name: json.loads((root / name).read_text())
    packages = read('packages.json')
    record_count = 0
    for package in packages.values():
        data = Path(package['path']).read_bytes()
        assert len(data) == package['size'] and sha(data) == package['sha256']
        end = 0
        for record in package['records']:
            offset = int(record['offset'], 16)
            assert offset == end and offset + 16 <= len(data)
            magic, kind, size, flags = struct.unpack_from('<4I', data, offset)
            assert magic == 0x35167f49 and size >= 16
            assert kind == int(record['type'], 16) and flags == int(record['flags'], 16)
            assert size == record['size'] and offset + size <= len(data)
            assert int(record['payload_offset'], 16) == offset + 16
            assert record['payload_size'] == size - 16
            payload = data[offset + 16:offset + size]
            assert sha(payload) == record['sha256']
            assert payload[:16].hex() == record['prefix']
            end = offset + size
            record_count += 1
        assert end == len(data)
    extraction = read('extraction.json')
    for relative, entry in extraction['files'].items():
        data = (args.cache / 'rootfs' / relative).read_bytes()
        assert len(data) == entry['size'] and sha(data) == entry['sha256']
        numbered = root / (relative.replace('/', '-') + '.txt')
        if numbered.exists():
            expected = [f'{n:4}: {line}'.rstrip() for n, line in
                        enumerate(data.decode().splitlines(), 1)]
            assert numbered.read_text().splitlines()[1:] == expected
    for name, entry in read('logs.json').items():
        data = Path(entry['path']).read_bytes()
        assert len(data) == entry['size'] and sha(data) == entry['sha256']
        numbered = root / name.replace('.log', '.txt')
        expected = [f'{n:4}: {line}'.rstrip() for n, line in
                    enumerate(data.decode('ascii', 'backslashreplace').splitlines(), 1)]
        assert numbered.read_text().splitlines()[1:] == expected
    kernels = read('kernels.json')
    kernel_sources = {}
    for label, entry in kernels['sources'].items():
        data = Path(entry['path']).read_bytes()
        assert len(data) == entry['size'] and sha(data) == entry['sha256']
        kernel_sources[label] = data
    for literal in kernels['literals']:
        data = kernel_sources[literal['source']]
        start = int(literal['offset'], 16)
        assert data[start:data.index(b'\0', start)].decode('ascii') == literal['value']
    tree = read('captured-device-tree.json')
    data = kernel_sources['captured-vmlinux']
    assert data[int(tree['offset'], 16):int(tree['offset'], 16) + 4] == b'\xd0\x0d\xfe\xed'
    for prop in tree['properties']:
        start = int(prop['offset'], 16)
        value = b'\0'.join(v.encode('ascii') for v in prop['value']) + b'\0'
        assert data[start:start + len(value)] == value
    images = read('images.json')
    rows = 0
    sources = {}
    for label, entry in images.items():
        path = Path(entry['path'])
        data = path.read_bytes()
        sources[label] = data
        assert len(data) == entry['size'] and sha(data) == entry['sha256']
        if entry.get('format') == 'raw':
            mapping = read('kernel-map.json')
            base = int(mapping['base'], 16)
            for anchor in mapping['anchors']:
                pair = struct.unpack_from('>II', data, int(anchor['pair_file'], 16))
                assert pair == (int(anchor['function_va'], 16), int(anchor['name_va'], 16))
                name_offset = int(anchor['name_file'], 16)
                assert data[name_offset:data.index(b'\0', name_offset)].decode() == anchor['name']
                assert pair[1] - name_offset == base
                assert base <= pair[0] < base + len(data)
            for sysctl in mapping['sysctls']:
                fields = struct.unpack_from('>5I', data, int(sysctl['entry_file'], 16))
                assert fields == (sysctl['number'], int(sysctl['name_va'], 16),
                    int(sysctl['value_va'], 16), sysctl['size'], int(sysctl['mode'], 8))
                name_offset = int(sysctl['name_file'], 16)
                assert data[name_offset:data.index(b'\0', name_offset)].decode() == sysctl['name']
                assert fields[1] - name_offset == base
            loads = [(base, 0, len(data))]
        else:
            with path.open('rb') as stream:
                elf = ELFFile(stream)
                if entry.get('format') == 'relocatable':
                    assert elf['e_type'] == 'ET_REL'
                    section = elf.get_section_by_name(entry['load_section'])
                    loads = [(section['sh_addr'], section['sh_offset'], section['sh_size'])]
                    expected = []
                    for section in elf.iter_sections():
                        if section['sh_type'] == 'SHT_RELA':
                            symbols = elf.get_section(section['sh_link'])
                            for relocation in section.iter_relocations():
                                symbol = symbols.get_symbol(relocation['r_info_sym'])
                                expected.append(dict(section=section.name, offset=hex(relocation['r_offset']),
                                    type=relocation['r_info_type'], addend=relocation['r_addend'],
                                    symbol=symbol.name, symbol_section=symbol['st_shndx'],
                                    symbol_value=hex(symbol['st_value']), symbol_size=symbol['st_size']))
                    assert read(label + '-relocations.json') == expected
                else:
                    loads = [(s['p_vaddr'], s['p_offset'], s['p_filesz'])
                             for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        count = 0
        for excerpt in sorted(root.glob(label + '-*.dis.txt')):
            for line in excerpt.read_text().splitlines():
                match = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ){3}[0-9a-f]{2}).*# file (0x[0-9a-f]+)$', line)
                if not match:
                    continue
                va, offset = int(match[1], 16), int(match[3], 16)
                mapped = [start + va - base for base, start, size in loads
                          if base <= va and va + 4 <= base + size]
                assert mapped == [offset]
                assert bytes.fromhex(match[2]) == data[offset:offset + 4]
                count += 1
        assert count == entry['checked_disassembly_rows'] and count > 0
        rows += count
    for literal in read('supervision-literals.json'):
        data = sources[literal['source']]
        start = int(literal['file'], 16)
        assert data[start:data.index(b'\0', start)].decode() == literal['value']
    for record in read('record-handlers.json'):
        offset = int(record['file'], 16)
        assert tuple(int(word, 16) for word in record['words']) == struct.unpack_from(
            '>4I', sources['upgrade'], offset)
    for filename, label in (('direct-callers.json', 'upgrade'),
                            ('watchdog-direct-callers.json', 'anacapa')):
        for call in read(filename)['calls']:
            start = int(call['caller_file'], 16)
            word = struct.unpack_from('>I', sources[label], start)[0]
            assert f'{word:08x}' == call['bytes'] and word & 0xfc000003 == 0x48000001
            displacement = word & 0x03fffffc
            if displacement & 0x02000000:
                displacement -= 0x04000000
            assert (int(call['caller_va'], 16) + displacement) & 0xffffffff == int(call['target_va'], 16)
    anchor = read('legacy-public-anchor.json')
    start = int(anchor['der_file'], 16)
    assert sha(sources['crypto'][start:start + anchor['der_size']]) == anchor['sha256']
    signature_check = read('legacy-signature-validation.json')
    older = Path(packages['34.16-37101-1-16']['path']).read_bytes()
    prefix_end = int(signature_check['prefix_end'], 16)
    signature_start = int(signature_check['signature_file'], 16)
    assert prefix_end == int(packages['34.16-37101-1-16']['records'][-1]['offset'], 16)
    assert signature_start == prefix_end + 16
    assert sha(older[:prefix_end]) == signature_check['prefix_sha256']
    assert hashlib.md5(older[:prefix_end]).hexdigest() == signature_check['prefix_md5']
    key = load_der_public_key(sources['crypto'][start:start + anchor['der_size']])
    key.verify(older[signature_start:], older[:prefix_end], padding.PKCS1v15(), hashes.MD5())
    for anchor in read('envelope-public-anchors.json')['anchors']:
        start = int(anchor['der_file'], 16)
        assert sha(sources['crypto'][start:start + anchor['der_size']]) == anchor['sha256']
        row = int(anchor['row_file'], 16)
        assert struct.unpack_from('>I', sources['crypto'], row)[0] == anchor['deny_mask']
        assert sources['crypto'][row + 4:row + 24].hex() == anchor['identifier']
    current = Path(packages['57.23-74170-1-16']['path']).read_bytes()
    for header in read('encrypted-headers.json'):
        start, length = int(header['payload_file'], 16), header['header_size']
        fields = struct.unpack_from('>IIHHI', current, start)
        assert fields == (int(header['magic'], 16), length, header['version'], header['flags'], header['attributes'])
        assert int(header['data_file'], 16) == start + length
        assert current[start + 16] == len(header['recipients'])
        position = start + 17
        for recipient in header['recipients']:
            assert struct.unpack_from('>I', current, position)[0] == recipient['identifier_size']
            assert current[position + 4] == recipient['identifier_type']
            position += 5 + recipient['identifier_size']
            assert struct.unpack_from('>II', current, position) == (recipient['algorithm'], recipient['wrapped_size'])
            position += 8 + recipient['wrapped_size']
        assert position + 8 == start + length
        assert struct.unpack_from('>II', current, position) == (header['cipher_id'], header['trailing_scalar'])
        record = next(r for r in packages['57.23-74170-1-16']['records'] if r['payload_offset'] == header['payload_file'])
        assert header['data_size'] == record['payload_size'] - length
    document = args.document.read_text()
    assert '## CONFIDENCE' in document and '## UNKNOWN-OFFLINE' in document
    links = 0
    for target in re.findall(r'\]\(([^)]+)\)', document):
        assert not target.startswith(('http:', 'https:'))
        assert (args.document.parent / target).is_file(), target
        links += 1
    offsets = 0
    for label, value in re.findall(r'\b([UCKA])@(?:`)?(0x[0-9a-f]+)', document):
        key = {'U': 'upgrade', 'C': 'crypto', 'K': 'kernel', 'A': 'anacapa'}[label]
        assert int(value, 16) < len(sources[key])
        offsets += 1
    compared = 0
    if args.compare:
        files = lambda path: {p.relative_to(path) for p in path.rglob('*') if p.is_file()}
        paths = files(root)
        assert paths == files(args.compare), 'packet file sets differ'
        for path in paths:
            assert (root / path).read_bytes() == (args.compare / path).read_bytes(), path
            compared += 1
    print(f'PASS: {len(packages)} source packages; {record_count} records; {len(extraction["files"])} extracted hashes; {rows} disassembly rows')
    print(f'PASS: {links} document links; {offsets} cited offset starts in bounds; {compared} regenerated files identical')
    print('PASS: offline legacy RSA/MD5 signature validates the older package prefix')
    print('LIMIT: byte checks and one legacy signature do not prove later signatures, RE semantics, complete control flow or hardware behavior')


if __name__ == '__main__':
    main()
