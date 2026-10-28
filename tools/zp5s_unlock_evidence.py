#!/usr/bin/env python3
"""Extract a fixed, non-credential allowlist from a captured 34.16 UPD.

Only host unsquashfs/objdump run. Target programs and install scripts never run.
All output must be inside this worktree; the package is opened read-only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from stock_unlock_evidence import Image, disassemble


def record(path, data):
    return dict(path=str(path), size=len(data), sha256=hashlib.sha256(data).hexdigest())


def container(path):
    data = path.read_bytes()
    records, pos = [], 0
    while pos < len(data):
        if pos + 16 > len(data):
            raise ValueError('truncated UPD header')
        magic, kind, size, flags = struct.unpack_from('<4I', data, pos)
        if magic != 0x35167f49 or size < 16 or pos + size > len(data):
            raise ValueError(f'invalid UPD record at {pos:#x}')
        payload = data[pos + 16:pos + size]
        records.append(dict(record_file=hex(pos), type=hex(kind), size=size,
                            flags=hex(flags), payload_file=hex(pos + 16),
                            payload_size=len(payload), prefix=payload[:32].hex(),
                            sha256=hashlib.sha256(payload).hexdigest()))
        pos += size
    return data, dict(**record(path, data), exact_eof=pos == len(data), records=records)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--current-package', type=Path, required=True)
    p.add_argument('--cache', type=Path, required=True)
    p.add_argument('--objdump', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    workspace = Path(__file__).resolve().parents[1]
    cache, out = a.cache.resolve(), a.out.resolve()
    for target in (cache, out):
        if target == workspace or not target.is_relative_to(workspace):
            p.error('cache and output must be subdirectories of this worktree')
    package = a.package.resolve()
    data, parsed = container(package)
    # Pin the reviewed input rather than treating arbitrary payloads as this build.
    if parsed['sha256'] != '84632e02bdce626bf10ae846839e90d66e510a627320fff8690c2bdb998733f2':
        p.error('unexpected 34.16 package bytes')
    root_record = [r for r in parsed['records'] if r['type'] == '0x4']
    if len(root_record) != 1 or root_record[0]['flags'] != '0x0':
        p.error('expected one plain type-4 rootfs record')
    r = root_record[0]
    start, length = int(r['payload_file'], 16), r['payload_size']
    payload = data[start:start + length]
    if payload[:4] != b'sqsh':
        p.error('expected big-endian SquashFS magic')
    cache.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    section = cache / 'rootfs.section'
    section.write_bytes(payload)
    root = cache / 'rootfs'
    files = {
        'Z': 'opt/bin/anacapad', 'ZC': 'lib/libsonoscrypto.so.1',
        'ZU': 'lib/libsonosutils.so.1', 'ZB': 'bin/busybox',
    }
    texts = ('etc/Configure', 'etc/inittab', 'etc/runanacapa',
             'etc/init.d/inetd', 'etc/inetd.conf', 'opt/conf/anacapa.conf',
             'opt/bin/anacapactl')
    commands = []
    for rel in (*files.values(), *texts):
        cmd = ['unsquashfs', '-cat', str(section), rel]
        commands.append(cmd)
        extracted = subprocess.run(cmd, check=True, capture_output=True).stdout
        dest = root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(extracted)
    images = {key: Image(root / rel) for key, rel in files.items()}
    manifest = {key: im.manifest() for key, im in images.items()}
    manifest['package'] = record(package, data)
    manifest['rootfs-section'] = record(section, payload)
    manifest['tool'] = dict(objdump=str(a.objdump), version=subprocess.run(
        [str(a.objdump), '--version'], check=True, capture_output=True,
        text=True).stdout.splitlines()[0])
    for rel in texts:
        path = root / rel
        original = path.read_bytes()
        manifest[rel] = record(path, original)
        (out / (rel.replace('/', '-') + '.txt')).write_text(
            f'# Source: {path}\n# Display only: trailing whitespace stripped; hash covers original bytes.\n'
            + ''.join(f'{n:4}: {line}'.rstrip() + '\n' for n, line in
                      enumerate(original.decode().splitlines(), 1)))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (out / 'package.json').write_text(json.dumps(parsed, indent=2) + '\n')
    _, current = container(a.current_package.resolve())
    (out / 'current-package.json').write_text(json.dumps(current, indent=2) + '\n')
    inspection = [['unsquashfs', '-s', str(section)],
                  ['unsquashfs', '-ll', str(section), 'usr/sbin/inetd',
                   'usr/sbin/telnetd', 'bin/login', 'tmp', 'var']]
    result = []
    for cmd in inspection:
        run = subprocess.run(cmd, check=True, capture_output=True, text=True)
        result.append('$ ' + ' '.join(cmd) + '\n' + run.stdout + run.stderr)
    (out / 'filesystem.txt').write_text('\n'.join(result))
    (out / 'extraction.json').write_text(json.dumps(dict(
        source=str(package), rootfs_record=r,
        unsquashfs_commands=commands,
        boundary='Fixed allowlist: no account database, target execution, socket or device access.'
    ), indent=2) + '\n')
    ranges = (
        ('unlock', 'Z', 0x10261778, 0x10262204),
        ('spawn', 'Z', 0x102d7118, 0x102d7558),
        ('manufacturing-object', 'Z', 0x102487b8, 0x10248878),
        ('manufacturing-read', 'Z', 0x102cbc70, 0x102cbdb4),
        ('unlock-key-slot', 'Z', 0x1025d9f4, 0x1025da80),
        ('unlock-key-initialization', 'Z', 0x1025d870, 0x1025d9f4),
        ('unlock-key-startup', 'Z', 0x101c5574, 0x101c5584),
        ('manufacturing-second-read', 'Z', 0x102cbdbc, 0x102cbeb4),
        ('hash', 'ZC', 0x5368, 0x55e0),
        ('serialization', 'ZU', 0x1b8c, 0x1de8),
        ('rsa', 'ZC', 0x5f60, 0x6b3c),
        ('rng', 'ZC', 0x7408, 0x7468),
        ('http-module-root', 'Z', 0x102e363c, 0x102e3680),
        ('http-registration', 'Z', 0x102e8ff8, 0x102e9230),
        ('http-dispatch', 'Z', 0x102e96b4, 0x102e9ac0),
        ('http-policy-install', 'Z', 0x10006e48, 0x10006e78),
        ('http-policy-setter', 'Z', 0x102e0610, 0x102e0618),
        ('http-policy', 'Z', 0x101c86ac, 0x101c88f0),
        ('tls-predicate', 'Z', 0x102d9190, 0x102d9198),
        ('socket-bind', 'Z', 0x102d9cc0, 0x102d9d34),
        ('http-listeners', 'Z', 0x102e08c0, 0x102e0b0c),
        ('accepted-connection', 'Z', 0x102e16e4, 0x102e1758),
        ('accept-addresses', 'Z', 0x102da34c, 0x102da534),
        ('accepted-address-storage', 'Z', 0x102e1614, 0x102e1758),
        ('http-connection-worker', 'Z', 0x102e0618, 0x102e08c0),
        ('request-target-parser', 'Z', 0x102de760, 0x102de920),
        ('tls-server-config', 'Z', 0x102e2d70, 0x102e2fc0),
        ('tls-connection', 'Z', 0x102e2fd8, 0x102e310c),
        ('tls-handshake', 'Z', 0x102e323c, 0x102e331c),
        ('ssl-port-enabled', 'Z', 0x102e10fc, 0x102e111c),
        ('devmode', 'Z', 0x10262204, 0x10262f38),
        ('devmode-startup', 'Z', 0x1023bab8, 0x1023bb44),
        ('devmode-verifier', 'ZC', 0x8e24, 0x901c),
        ('devmode-trust', 'ZC', 0x8b48, 0x8e20),
        ('devmode-envelope-verify', 'ZC', 0x8870, 0x8b10),
        ('devmode-hash-callback', 'ZC', 0x5698, 0x5770),
        ('devmode-signature-callback', 'ZC', 0x6b3c, 0x6b5c),
        ('devmode-key-release', 'ZC', 0x59ac, 0x59cc),
        ('debug-properties', 'Z', 0x1024cdf0, 0x1024da40),
        ('testenv-entry', 'Z', 0x101c8924, 0x101c8984),
        ('testenv', 'Z', 0x10259254, 0x10259834),
        ('config-defaults', 'Z', 0x101c8984, 0x101c8a44),
        ('config-override', 'Z', 0x100060bc, 0x10006114),
        ('settings-wiring', 'Z', 0x10050f30, 0x10050fc8),
        ('settings-wiring-boot', 'Z', 0x100516b8, 0x10051764),
        ('settings-wrapper-constructor', 'Z', 0x100513b4, 0x100513f0),
        ('settings-global', 'Z', 0x10007078, 0x100070a4),
        ('settings-getter', 'Z', 0x10058ba4, 0x10058bb0),
        ('settings-forward', 'Z', 0x1014c2b0, 0x1014c2f4),
        ('settings-app-constructor', 'Z', 0x101d0a54, 0x101d0b00),
        ('settings-constructor', 'Z', 0x101d04d0, 0x101d0540),
        ('settings-base-constructor', 'Z', 0x101cef34, 0x101cefe8),
        ('settings-setter', 'Z', 0x101cf654, 0x101cf8e4),
        ('settings-thunk', 'Z', 0x101d13c8, 0x101d13d0),
        ('settings-transaction', 'Z', 0x101ce274, 0x101ce780),
        ('inetd-exec', 'ZB', 0x1001a594, 0x1001a714),
        ('telnet-login', 'ZB', 0x1001d884, 0x1001db48),
        ('login', 'ZB', 0x10021cb4, 0x100222d0),
        ('login-password', 'ZB', 0x1003557c, 0x10035628),
        ('login-identity', 'ZB', 0x10034c38, 0x10034cec),
        ('login-shell', 'ZB', 0x10039070, 0x100391a0),
    )
    for label, key, start, end in ranges:
        (out / f'{label}.dis.txt').write_text(disassemble(images[key], a.objdump, start, end))
    z = images['Z']
    routes = []
    for va in range(0x103eb708, 0x103ec100, 24):
        words = z.words(va, 6)
        path_va = int(words[3], 16)
        routes.append(dict(va=hex(va), file=hex(z.offset(va)), words=words,
                           path=z.string(path_va) if path_va else None))
        if all(int(word, 16) == 0 for word in words):
            break
    else:
        raise ValueError('missing exact-route terminator')
    (out / 'routes.json').write_text(json.dumps(dict(stride=24,
        module_root=dict(va='0x103f56f8', file=hex(z.offset(0x103f56f8)), words=z.words(0x103f56f8, 2)),
        module=dict(va='0x103eb5d4', file=hex(z.offset(0x103eb5d4)), words=z.words(0x103eb5d4, 15)),
        routes=routes), indent=2) + '\n')
    strings = {'Z': (0x10314fec, 0x10314ff4, 0x10315000, 0x1031500c,
        0x10315018, 0x10315024, 0x1035a924, 0x1035a940, 0x1035a95c,
        0x1035a968, 0x1035a97c, 0x1035a990, 0x1035aa88, 0x1035aa90,
        0x1035aaa0, 0x1035aaac, 0x1035aac4, 0x10358204, 0x10313ec4,
        0x103474d0, 0x103474d8, 0x10348344, 0x10319d54, 0x1031f330,
        0x1033e408, 0x10385de0, 0x1035a22c, 0x1035a238,
        0x1034b548, 0x1035a240),
        'ZU': (0x2428,),
        'ZB': (0x1003ba28, 0x1003bb10, 0x1003b9f4, 0x10045758,
               0x1004631c, 0x10046350, 0x10048164)}
    constants = {key: [dict(va=hex(va), file=hex(images[key].offset(va)),
                     value=images[key].string(va)) for va in vas] for key, vas in strings.items()}
    constants['Z_float64'] = [dict(va=hex(va), file=hex(z.offset(va)),
        bytes=z.data[z.offset(va):z.offset(va)+8].hex(),
        value=struct.unpack_from('>d', z.data, z.offset(va))[0])
        for va in (0x10336f60, 0x1035a9e8)]
    constants['vtables'] = [dict(image=key, va=hex(va), file=hex(images[key].offset(va)),
        words=images[key].words(va, count)) for key, va, count in
        [('Z', 0x103480d0, 22), ('Z', 0x1031db34, 1), ('ZB', 0x1003bd70, 3),
         ('ZB', 0x1003bdc4, 3), ('ZB', 0x1003bf2c, 3), ('ZB', 0x100590e8, 2)]]
    (out / 'constants.json').write_text(json.dumps(constants, indent=2) + '\n')
    relocations = []
    selected = {'ZU': {0x12454},
                'ZC': {0x1c0ec, 0x1c0f0, 0x1c0f4, 0x1c0f8, 0x1c0fc}}
    for key, offsets in selected.items():
        im = images[key]
        for section in im.elf.iter_sections():
            if section['sh_type'] != 'SHT_RELA':
                continue
            symbols = im.elf.get_section(section['sh_link'])
            for index, reloc in enumerate(section.iter_relocations()):
                if reloc['r_offset'] in offsets:
                    relocations.append(dict(image=key, section=section.name,
                        entry_file=hex(section['sh_offset'] + index * section['sh_entsize']),
                        target_va=hex(reloc['r_offset']), target_file=hex(im.offset(reloc['r_offset'])),
                        type=reloc['r_info_type'],
                        symbol=symbols.get_symbol(reloc['r_info_sym']).name,
                        addend=hex(reloc['r_addend'])))
    (out / 'relocations.json').write_text(json.dumps(relocations, indent=2) + '\n')
    print(f'EVIDENCE: 2 UPD containers; {len(images)} ELF manifests; '
          f'{len(texts)} numbered text sources; {len(ranges)} disassembly ranges; {len(routes)} exact route rows')


if __name__ == '__main__':
    main()
