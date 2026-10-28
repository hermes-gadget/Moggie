#!/usr/bin/env python3
"""Audit saved offline evidence against captured bytes; never execute target code.

This checks provenance and transcription, not correctness of RE interpretations.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex

from stock_unlock_evidence import Image


def require(condition, message):
    if not condition:
        raise SystemExit('FAIL: ' + message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--document', type=Path, required=True)
    args = parser.parse_args()
    packet = args.evidence
    manifest = json.loads((packet / 'manifest.json').read_text())
    images = {key: Image(value['path']) for key, value in manifest.items()
              if isinstance(value, dict) and 'elf_class' in value}
    require(bool(images), 'no ELF sources in manifest')
    records = [v for v in manifest.values() if isinstance(v, dict) and 'sha256' in v]
    versions_path = packet / 'versions.json'
    if versions_path.is_file():
        records += json.loads(versions_path.read_text())
    for name in ('target-package.json', 'current-package.json'):
        package_path = packet / name
        if package_path.is_file():
            records.append(json.loads(package_path.read_text()))
    for record in records:
        data = Path(record['path']).read_bytes()
        require(len(data) == record['size'], 'source size: ' + record['path'])
        require(hashlib.sha256(data).hexdigest() == record['sha256'],
                'source hash: ' + record['path'])
    sources = {str(image.path): image for image in images.values()}
    instruction = re.compile(
        r'^\s*([0-9a-f]+) \[file (0x[0-9a-f]+)\]:\s*'
        r'((?:[0-9a-f]{2} ){3}[0-9a-f]{2})(?:\s|$)')
    count = 0
    excerpts = sorted(packet.glob('*.dis.txt'))
    for excerpt in excerpts:
        lines = excerpt.read_text().splitlines()
        command = shlex.split(lines[0][2:])
        source = sources[command[-1]]
        for line in lines[1:]:
            if line.startswith('#') or '[file ' not in line:
                continue
            match = instruction.match(line)
            require(match is not None, 'unparsed instruction: ' + excerpt.name + ': ' + line)
            va, off = int(match[1], 16), int(match[2], 16)
            require(source.offset(va) == off, 'VA/file mapping: ' + excerpt.name)
            require(source.data[off:off + 4] == bytes.fromhex(match[3]),
                    'instruction bytes: ' + excerpt.name + ' ' + hex(off))
            count += 1
    document = args.document.read_text()
    offsets = 0
    for match in re.finditer(r'\b([A-Z]+)@`?(0x[0-9a-f]+)(?:[–-](0x[0-9a-f]+))?', document):
        if match[1] not in images:
            continue  # A document can cite several independently audited packets.
        image = images[match[1]]
        first, last = int(match[2], 16), int(match[3] or match[2], 16)
        require(0 <= first <= last < len(image.data), 'document offset outside source')
        require(image.address(first) is not None and image.address(last) is not None,
                'document offset outside file-backed LOAD bytes')
        offsets += 1
    for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', document):
        if target.startswith(('#', 'http:', 'https:')):
            continue
        require((args.document.parent / target.split('#')[0]).exists(),
                'missing document link: ' + target)
    if args.compare:
        original = {p.relative_to(packet): p.read_bytes() for p in packet.rglob('*') if p.is_file()}
        repeated = {p.relative_to(args.compare): p.read_bytes()
                    for p in args.compare.rglob('*') if p.is_file()}
        require(original == repeated, 'regenerated packet differs')
        print(f'REPRODUCIBLE: {len(original)} evidence files byte-identical')
    print(f'PROVENANCE: {len(records)} source records match original size and SHA-256')
    print(f'BYTES: {len(excerpts)} excerpts; {count} instructions match original bytes and LOAD mappings')
    print(f'DOCUMENT: {offsets} explicit source-offset citations in file-backed LOAD bytes; all local links resolve')
    print('LIMIT: byte/offset/link checks do not establish semantic or hardware correctness')


if __name__ == '__main__':
    main()
