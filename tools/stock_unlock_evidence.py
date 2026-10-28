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
import struct
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

    def address(self, off):
        for seg in self.elf.iter_segments():
            delta = off - seg['p_offset']
            if seg['p_type'] == 'PT_LOAD' and 0 <= delta < seg['p_filesz']:
                return seg['p_vaddr'] + delta
        return None

    def words(self, va, count):
        off = self.offset(va)
        endian = '<' if self.elf.little_endian else '>'
        return [hex(v) for v in struct.unpack_from(endian + 'I' * count, self.data, off)]

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
    parser.add_argument('--target-package', type=Path)
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
        'F': 'lib/libflash.so.1', 'B': 'bin/busybox',
    }
    images = {name: Image(root / path) for name, path in targets.items()}
    manifest = {name: image.manifest() for name, image in images.items()}
    manifest['tool'] = {
        'objdump': str(args.objdump),
        'version': subprocess.run([str(args.objdump), '--version'], check=True,
                                  capture_output=True, text=True).stdout.splitlines()[0],
    }
    manifest['symlinks'] = {
        rel: str((root / rel).readlink())
        for rel in ('tmp', 'var', 'bin/login', 'usr/sbin/telnetd')
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
    ranges = (
        ('unlock', 'A', 0x1029ff30, 0x102a0c04),
        ('spawn', 'A', 0x1041c09c, 0x1041c44c),
        ('manufacturing-object', 'A', 0x10355d34, 0x10355dc0),
        ('unlock-key-slot', 'A', 0x103763f0, 0x103764a4),
        ('unlock-key-initialization', 'A', 0x10376224, 0x103763f0),
        ('unlock-key-startup', 'A', 0x1027ac70, 0x1027ac7c),
        ('manufacturing-second-read', 'F', 0x23b8, 0x2410),
        ('unlocked-flag-reader', 'A', 0x1037d048, 0x1037d08c),
        ('hash', 'C', 0x51a8, 0x5360),
        ('rsa', 'C', 0x5eac, 0x6790),
        ('rng', 'C', 0x71f8, 0x725c),
        ('serialization', 'U', 0x2ed8, 0x3190),
        ('force-reboot', 'U', 0x6820, 0x6868),
        ('http-module-registration', 'A', 0x10430424, 0x104306bc),
        ('http-route-dispatch', 'A', 0x10430804, 0x10430e34),
        ('http-policy-install', 'A', 0x1000d060, 0x1000d0b8),
        ('http-policy-setter', 'A', 0x10426a88, 0x10426a98),
        ('http-policy', 'A', 0x1027d438, 0x1027d4f4),
        ('http-policy-forward', 'A', 0x10287678, 0x10287720),
        ('http-policy-flags', 'A', 0x10286bb0, 0x10286f30),
        ('http-module-root', 'A', 0x10429660, 0x104296bc),
        ('socket-bind', 'A', 0x1041ec30, 0x1041ed60),
        ('peer-address-predicate', 'A', 0x10428244, 0x10428320),
        ('peer-accept-addresses', 'A', 0x1041f19c, 0x1041f32c),
        ('peer-filter-caller', 'A', 0x10428b64, 0x10428cf4),
        ('http-listeners', 'A', 0x10425494, 0x104257ac),
        ('accepted-connection', 'A', 0x10428890, 0x10428b28),
        ('tls-context', 'A', 0x1042b488, 0x1042b558),
        ('tls-predicate', 'A', 0x1041dcf8, 0x1041dd18),
        ('host-check', 'A', 0x10423dec, 0x10423f78),
        ('host-check-caller', 'A', 0x10425328, 0x104253a0),
        ('stat-helper', 'A', 0x1041e3e8, 0x1041e428),
        ('static-directory-check', 'A', 0x10427f2c, 0x10428064),
        ('devmode-entry', 'A', 0x1027bdf0, 0x1027bdf4),
        ('devmode', 'A', 0x1037d890, 0x1037eb24),
        ('devmode-startup', 'A', 0x10346b40, 0x10346bac),
        ('debug-property-gates', 'A', 0x1035ab48, 0x1035afb0),
        ('testenv-entry', 'A', 0x1027d4f4, 0x1027d58c),
        ('testenv', 'A', 0x1036d4e4, 0x1036dca0),
        ('sysprops-transaction', 'A', 0x1028baa8, 0x1028c410),
        ('sysprops-setter', 'A', 0x1028c924, 0x1028cb04),
        ('sysprops-constructor', 'A', 0x1028cf44, 0x1028d2b8),
        ('sysprops-commit', 'A', 0x1028dc44, 0x1028e0d8),
        ('settings-wiring', 'A', 0x100ac574, 0x100ac634),
        ('settings-forward', 'A', 0x101da494, 0x101da4e0),
        ('config-defaults', 'A', 0x10006c10, 0x100075bc),
        ('config-overrides', 'A', 0x100093d0, 0x100094bc),
        ('devmode-verifier', 'C', 0xa050, 0xa634),
        ('mfg-envelope', 'C', 0xc170, 0xc2f8),
        ('jffs-root-setter', 'U', 0x3c40, 0x3ca4),
        ('ssh-account-loader', 'D', 0x9738, 0x9848),
        ('ssh-account-policy', 'D', 0x12f4c, 0x133e8),
        ('ssh-password', 'D', 0x133ec, 0x1358c),
        ('telnet-default-login', 'B', 0x10011f84, 0x10012010),
        ('telnet-login-exec', 'B', 0x10011c80, 0x10011eb8),
        ('busybox-applet-dispatch', 'B', 0x10003d60, 0x10004008),
        ('telnet-login-policy', 'B', 0x10006a58, 0x10006f48),
        ('telnet-password', 'B', 0x1004df98, 0x1004e098),
        ('telnet-password-hasher', 'B', 0x100570cc, 0x10057568),
        ('telnet-login-identity', 'B', 0x1004d71c, 0x1004d7a8),
        ('telnet-login-setuid', 'B', 0x100058ac, 0x1000591c),
        ('telnet-login-shell', 'B', 0x100583bc, 0x100584dc),
    )
    for label, key, start, end in ranges:
        (out / f'{label}.dis.txt').write_text(
            disassemble(images[key], args.objdump, start, end))

    # Record complete exact-match table, including its terminator. These are
    # static route names and pointers, not requests or runtime user data.
    a = images['A']
    routes = []
    for va in range(0x1056cae4, 0x1056d52c + 28, 28):
        words = a.words(va, 7)
        path_va = int(words[3], 16)
        routes.append({'va': hex(va), 'file': hex(a.offset(va)), 'words': words,
                       'path': a.string(path_va) if path_va else None})
    (out / 'routes.json').write_text(json.dumps({
        'source': str(a.path), 'stride': 28,
        'module_root': {'va': '0x1056f410', 'file': '0x55f410',
                        'words': a.words(0x1056f410, 2)},
        'module': {'va': '0x1056c9ac', 'file': '0x55c9ac',
                   'words': a.words(0x1056c9ac, 15)},
        'routes': routes}, indent=2) + '\n')

    string_vas = {
        'A': (0x104b9f2c, 0x104b9f48, 0x104b9f5c, 0x104b9f70,
              0x104b9f8c, 0x104ba03c, 0x104b9ffc, 0x1051df6c,
              0x1051df90, 0x1051e164, 0x1045ad0c, 0x1051a234,
              0x1045d610, 0x104b7468, 0x104b7478, 0x104b5508,
              0x1046d8f4, 0x1045b0e8, 0x1051d298, 0x1045b390,
              0x1051d8e4, 0x1046641c, 0x104b5694, 0x10475e3c),
        'U': (0x87b8,),
        'D': (0x2add9, 0x2ae4f, 0x2aef0, 0x2afb0, 0x2b3c6),
        'B': (0x1005e280, 0x1005e3a9, 0x10063c39, 0x10061990,
              0x100619bf, 0x10068dde),
    }
    strings = {key: [{'va': hex(va), 'file': hex(images[key].offset(va)),
                      'value': images[key].string(va)} for va in vas]
               for key, vas in string_vas.items()}
    strings['A_float32'] = [
        {'va': hex(va), 'file': hex(a.offset(va)),
         'bytes': a.data[a.offset(va):a.offset(va) + 4].hex(),
         'value': struct.unpack_from('>f', a.data, a.offset(va))[0]}
        for va in (0x104979c0, 0x104983e0)]
    strings['A_vtables'] = [
        {'va': hex(va), 'file': hex(a.offset(va)), 'words': a.words(va, n)}
        for va, n in ((0x104b7250, 22), (0x104b7294, 4))]
    (out / 'constants.json').write_text(json.dumps(strings, indent=2) + '\n')
    b = images['B']
    applets = []
    name_start = b.data.index(b'[\0[[\0')
    for name in ('login', 'telnetd'):
        off = b.data.index(name.encode() + b'\0', name_start)
        index = b.data[name_start:off].count(b'\0')
        slot = 0x1005ba90 + 4 * index
        applets.append(dict(name=name, name_file=hex(off), index=index,
                            function_slot_file=hex(b.offset(slot)),
                            function_va=b.words(slot, 1)[0]))
    (out / 'busybox-applets.json').write_text(json.dumps(dict(
        names_file=hex(name_start), function_table_file='0x5ba90', applets=applets),
        indent=2) + '\n')

    relocations = []
    selected = {'A': {0x1057200c}, 'U': {0x190b0},
                'D': {0x3f9bc, 0x3f9c8, 0x3f9e4, 0x3fa08}}
    for key, offsets in selected.items():
        image = images[key]
        for section in image.elf.iter_sections():
            if section['sh_type'] != 'SHT_RELA':
                continue
            symbols = image.elf.get_section(section['sh_link'])
            for index, reloc in enumerate(section.iter_relocations()):
                if reloc['r_offset'] not in offsets:
                    continue
                symbol = symbols.get_symbol(reloc['r_info_sym'])
                relocations.append({
                    'image': key, 'section': section.name,
                    'entry_file': hex(section['sh_offset'] + index * section['sh_entsize']),
                    'target_va': hex(reloc['r_offset']),
                    'target_file': (hex(image.offset(reloc['r_offset']))
                                    if image.offset(reloc['r_offset']) is not None else None),
                    'type': reloc['r_info_type'], 'symbol': symbol.name,
                    'addend': hex(reloc['r_addend']),
                })
    (out / 'relocations.json').write_text(json.dumps(relocations, indent=2) + '\n')

    versions = []
    for path in sorted(root.parent.glob('*/opt/bin/anacapad')):
        image = Image(path)
        entry = image.manifest()
        entry['run_sshd_exists'] = (path.parents[2] / 'etc/scripts/run_sshd.sh').is_file()
        entry['strings'] = []
        endian = '<' if image.elf.little_endian else '>'
        for name in ('/unlock', '/unlock.htm', '/devunlock', '/mfgunlock',
                     '/devmode', '/testenv'):
            needle = name.encode() + b'\0'
            for match in re.finditer(re.escape(needle), image.data):
                off = match.start()
                va = image.address(off)
                candidates = []
                if va is not None:
                    pointer = struct.pack(endian + 'I', va)
                    for ref in re.finditer(re.escape(pointer), image.data):
                        pos = ref.start()
                        if pos % 4 == 0 and image.address(pos) is not None:
                            candidates.append({'file': hex(pos),
                                               'va': hex(image.address(pos)),
                                               'following_words': list(map(hex, struct.unpack_from(
                                                   endian + '2I', image.data, pos + 4)))})
                entry['strings'].append({'value': name, 'file': hex(off),
                                         'va': hex(va) if va is not None else None,
                                         'aligned_pointer_candidates': candidates})
        versions.append(entry)
    (out / 'versions.json').write_text(json.dumps(versions, indent=2) + '\n')
    if args.target_package:
        package = args.target_package.resolve()
        data = package.read_bytes()
        signatures = {
            'ELF': b'\x7fELF', 'gzip': b'\x1f\x8b\x08',
            'squashfs_LE': b'hsqs', 'squashfs_BE': b'sqsh',
            'cramfs_LE': b'E=\xcd(', 'cramfs_BE': b'(\xcd=E',
            'uImage': b'\x27\x05\x19\x56',
        }
        scan = {
            'path': str(package), 'size': len(data),
            'sha256': hashlib.sha256(data).hexdigest(),
            'signature_scan': {
                name: [hex(m.start()) for m in re.finditer(re.escape(magic), data)]
                for name, magic in signatures.items()
            },
            'adjacent_files': sorted(p.name for p in package.parent.iterdir()
                                     if p.is_file()),
            'limitation': 'Magic search does not prove encryption or impossibility of extraction.',
        }
        (out / 'target-package.json').write_text(json.dumps(scan, indent=2) + '\n')
    print(f'EVIDENCE: {len(images)} ELF manifests; 5 numbered text sources; '
          f'{len(ranges)} disassembly ranges; {len(versions)} version inventories')


if __name__ == '__main__':
    main()
