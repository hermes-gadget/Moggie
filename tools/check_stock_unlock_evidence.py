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
import struct

from stock_unlock_evidence import Image


def require(condition, message):
    if not condition:
        raise SystemExit('FAIL: ' + message)


def check_metadata(packet, images):
    """Check saved symbolic metadata independently against ELF source records."""
    relocations = json.loads((packet / 'relocations.json').read_text())
    for saved in relocations:
        source = images[saved['image']]
        entry = int(saved['entry_file'], 16)
        actual = None
        for section in source.elf.iter_sections():
            if section['sh_type'] != 'SHT_RELA' or section.name != saved['section']:
                continue
            delta = entry - section['sh_offset']
            if 0 <= delta < section['sh_size'] and delta % section['sh_entsize'] == 0:
                actual = section.get_relocation(delta // section['sh_entsize'])
                symbols = source.elf.get_section(section['sh_link'])
                break
        require(actual is not None, 'relocation entry outside ELF table')
        require(actual['r_offset'] == int(saved['target_va'], 16), 'relocation target')
        require(actual['r_info_type'] == saved['type'], 'relocation type')
        require(actual['r_addend'] == int(saved['addend'], 16), 'relocation addend')
        offset = source.offset(actual['r_offset'])
        require(saved['target_file'] == (hex(offset) if offset is not None else None),
                'relocation target VA/file mapping')
        if 'symbol' in saved:
            require(symbols.get_symbol(actual['r_info_sym']).name == saved['symbol'],
                    'relocation symbol')

    routes = json.loads((packet / 'routes.json').read_text())
    source = images['A'] if 'A' in images else images['Z']
    previous = None
    for row in [routes['module_root'], routes['module'], *routes['routes']]:
        va = int(row['va'], 16)
        require(source.offset(va) == int(row['file'], 16), 'route VA/file mapping')
        require(source.words(va, len(row['words'])) == row['words'], 'route words')
    for row in routes['routes']:
        va = int(row['va'], 16)
        require(len(row['words']) * 4 == routes['stride'], 'route stride/word count')
        if previous is not None:
            require(va - previous == routes['stride'], 'route table continuity')
        previous = va
        pointer = int(row['words'][3], 16)
        require(row['path'] == (source.string(pointer) if pointer else None), 'route path')
    require(all(int(w, 16) == 0 for w in routes['routes'][-1]['words']),
            'missing route-table terminator')

    constants = 0
    for key, rows in json.loads((packet / 'constants.json').read_text()).items():
        for row in rows:
            source = images[row.get('image', key.split('_')[0])]
            va, offset = int(row['va'], 16), int(row['file'], 16)
            require(source.offset(va) == offset, 'constant VA/file mapping')
            if 'words' in row:
                require(source.words(va, len(row['words'])) == row['words'], 'constant words')
            elif 'bytes' in row:
                raw = bytes.fromhex(row['bytes'])
                require(source.data[offset:offset + len(raw)] == raw, 'constant bytes')
                fmt = ('<' if source.elf.little_endian else '>') + ('f' if len(raw) == 4 else 'd')
                require(struct.unpack(fmt, raw)[0] == row['value'], 'floating constant value')
            else:
                require(source.string(va) == row['value'], 'constant string')
            constants += 1

    option_count = 0
    options_path = packet / 'ssh-options.json'
    if options_path.is_file():
        source = images['D']
        options = json.loads(options_path.read_text())
        table = int(options['table_va'], 16)
        require(source.offset(table) == int(options['table_file'], 16), 'option table mapping')
        for row in options['options']:
            option = row['option']
            require(len(option) == 2 and option[0] == '-', 'invalid option label')
            slot = table + 4 * (ord(option[1]) - ord('B'))
            offset = source.offset(slot)
            require(offset == int(row['entry_file'], 16), 'option jump slot')
            delta = struct.unpack_from('>i', source.data, offset)[0]
            require(delta == row['relative_delta'], 'option jump delta')
            target = table + delta
            require(target == int(row['target_va'], 16), 'option jump target')
            require(source.offset(target) == int(row['target_file'], 16), 'option target mapping')
            option_count += 1
    return len(relocations), len(routes['routes']), constants, option_count


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
    metadata = check_metadata(packet, images)
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
    print('METADATA: %d relocations; %d routes; %d constants; %d CLI option rows match source' % metadata)
    print(f'DOCUMENT: {offsets} explicit source-offset citations in file-backed LOAD bytes; all local links resolve')
    print('LIMIT: byte/offset/link checks do not establish semantic or hardware correctness')


if __name__ == '__main__':
    main()
