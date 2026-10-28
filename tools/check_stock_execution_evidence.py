#!/usr/bin/env python3
"""Independently check the saved execution evidence against original source bytes.

This checks transcription/provenance, not reverse-engineering conclusions,
vendor signatures or device behavior. No target code is executed.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from elftools.elf.elffile import ELFFile


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
        with path.open('rb') as stream:
            elf = ELFFile(stream)
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
    for record in read('record-handlers.json'):
        offset = int(record['file'], 16)
        assert tuple(int(word, 16) for word in record['words']) == struct.unpack_from(
            '>4I', sources['upgrade'], offset)
    anchor = read('legacy-public-anchor.json')
    start = int(anchor['der_file'], 16)
    assert sha(sources['crypto'][start:start + anchor['der_size']]) == anchor['sha256']
    document = args.document.read_text()
    assert '## CONFIDENCE' in document and '## UNKNOWN-OFFLINE' in document
    links = 0
    for target in re.findall(r'\]\(([^)]+)\)', document):
        assert not target.startswith(('http:', 'https:'))
        assert (args.document.parent / target).is_file(), target
        links += 1
    offsets = 0
    for label, value in re.findall(r'\b([UC])@(?:`)?(0x[0-9a-f]+)', document):
        assert int(value, 16) < len(sources['upgrade' if label == 'U' else 'crypto'])
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
    print('LIMIT: integrity checks do not validate signatures, RE semantics, complete control-flow coverage or hardware behavior')


if __name__ == '__main__':
    main()
