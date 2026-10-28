#!/usr/bin/env python3
"""Read captured firmware and emit reproducible, offline maintenance evidence.

Never runs a target executable, opens a socket, or writes to the capture tree.
Requires pyelftools and an existing PowerPC GNU objdump.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess

from elftools.elf.elffile import ELFFile


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.elf = ELFFile(io.BytesIO(self.data))

    def offset(self, va):
        for seg in self.elf.iter_segments():
            if seg['p_type'] == 'PT_LOAD':
                delta = va - seg['p_vaddr']
                if 0 <= delta < seg['p_filesz']:
                    return seg['p_offset'] + delta
        return None  # BSS has a virtual address, but no file bytes.

    def string(self, va):
        off = self.offset(va)
        if off is None:
            return None
        end = self.data.find(b'\0', off)
        return self.data[off:end].decode('utf-8', errors='replace')

    def manifest(self):
        return {
            'path': str(self.path), 'size': len(self.data),
            'sha256': hashlib.sha256(self.data).hexdigest(),
            'elf_class': self.elf.elfclass,
            'little_endian': self.elf.little_endian,
            'machine': self.elf['e_machine'], 'type': self.elf['e_type'],
            'load_segments': [
                {key: seg[key] for key in
                 ('p_offset', 'p_vaddr', 'p_filesz', 'p_memsz', 'p_flags')}
                for seg in self.elf.iter_segments() if seg['p_type'] == 'PT_LOAD'
            ],
        }


def disassemble(image, objdump, start, end):
    cmd = [str(objdump), '-d', f'--start-address={start:#x}',
           f'--stop-address={end:#x}', str(image.path)]
    result = subprocess.run(cmd, check=True, capture_output=True, text=True)
    lines = ['$ ' + ' '.join(cmd), '# Each instruction: VA [file offset] bytes mnemonic']
    for line in result.stdout.splitlines():
        match = re.match(r'\s*([0-9a-f]+):\s', line)
        if match:
            off = image.offset(int(match[1], 16))
            label = f'{off:#010x}' if off is not None else 'NO FILE BYTES'
            line = line[:match.end(1)] + f' [file {label}]' + line[match.end(1):]
        lines.append(line)
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--objdump', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = args.rootfs.resolve()
    out = args.out.resolve()
    workspace = Path(__file__).resolve().parents[1]
    if not out.is_relative_to(workspace) or out.is_relative_to(root):
        parser.error('Output must be inside this worktree and outside captured artifacts')
    out.mkdir(parents=True, exist_ok=True)
    targets = {
        'A': 'opt/bin/anacapad', 'C': 'lib/libsonoscrypto.so.1',
        'U': 'lib/libsonosutils.so.1', 'D': 'usr/bin/dropbear',
        'F': 'lib/libflash.so.1',
    }
    images = {name: Image(root / path) for name, path in targets.items()}
    manifest = {name: image.manifest() for name, image in images.items()}
    manifest['tool'] = {
        'objdump': str(args.objdump),
        'version': subprocess.run([str(args.objdump), '--version'], check=True,
                                  capture_output=True, text=True).stdout.splitlines()[0],
    }
    for rel in ('etc/Configure', 'etc/inittab', 'etc/runanacapa',
                'etc/scripts/run_sshd.sh', 'opt/conf/anacapa.conf'):
        path = root / rel
        data = path.read_bytes()
        manifest[rel] = {'path': str(path), 'size': len(data),
                         'sha256': hashlib.sha256(data).hexdigest()}
        # No account database or credential material is read or copied.
        (out / (rel.replace('/', '-') + '.txt')).write_text(
            f'# Source: {path}\n# Display only: trailing whitespace stripped; hash covers original bytes.\n' + ''.join(
                f'{n:4}: {line}'.rstrip() + '\n'
                for n, line in enumerate(data.decode().splitlines(), 1)))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for label, key, start, end in (
        ('unlock', 'A', 0x1029ff30, 0x102a0c04),
        ('spawn', 'A', 0x1041c09c, 0x1041c44c),
        ('manufacturing-object', 'A', 0x10355d34, 0x10355dc0),
        ('unlock-key-slot', 'A', 0x103763f0, 0x103764a4),
        ('hash', 'C', 0x51a8, 0x5360),
        ('rsa', 'C', 0x5eac, 0x6790),
        ('rng', 'C', 0x71f8, 0x725c),
        ('serialization', 'U', 0x2ed8, 0x3190),
        ('force-reboot', 'U', 0x6820, 0x6868),
    ):
        (out / f'{label}.dis.txt').write_text(
            disassemble(images[key], args.objdump, start, end))
    print(f'EVIDENCE: {len(images)} ELF manifests; 5 numbered text sources; 9 disassembly ranges')


if __name__ == '__main__':
    main()
