#!/usr/bin/env python3
"""Preserve a bounded comparison from an already-captured plaintext component.

Host tools only. No UPD decryption, target execution, account files or key data.
The archive's package association is recorded separately from byte verification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from stock_unlock_evidence import Image, disassemble


def record(path):
    data = path.read_bytes()
    return dict(path=str(path), size=len(data), sha256=hashlib.sha256(data).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--objdump', type=Path, required=True)
    p.add_argument('--cache', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    workspace = Path(__file__).resolve().parents[1]
    capture, cache, out = a.capture.resolve(), a.cache.resolve(), a.out.resolve()
    for target in (cache, out):
        if target == workspace or not target.is_relative_to(workspace):
            p.error('cache and output must be subdirectories of this worktree')
        if target.is_relative_to(capture):
            p.error('capture must remain read-only')
    component = capture / 'fw/57.23-74170-1-16-rootfs.bin'
    package = capture / 'fw/pkg.upd'
    receipt = capture / 'raw/fw-m16-m17.json'
    expected = {
        component: '48fc2172e56cbd2a6304cd67a627ff50aaf4bb37fd6480dd2b245d71e8a2e98d',
        package: 'e19a7bb28f2d940346c605ccfcff81f8d9a7dd1771ee991841ac1f9e07223ad4',
    }
    for path, digest in expected.items():
        if record(path)['sha256'] != digest:
            p.error('unexpected captured input: ' + str(path))
    out.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    files = ('opt/bin/anacapad', 'etc/Configure', 'etc/inittab',
             'etc/runanacapa', 'etc/arch', 'opt/conf/anacapa.conf',
             'etc/scripts/run_sshd.sh')
    commands, comparisons = [], []
    manifest = {name: record(path) for name, path in
                [('plaintext-component', component), ('package', package),
                 ('archive-run-receipt', receipt)]}
    for rel in files:
        cmd = ['unsquashfs', '-cat', str(component), rel]
        data = subprocess.run(cmd, check=True, capture_output=True).stdout
        commands.append(cmd)
        dest = cache / 'rootfs' / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        prior = capture / 'fw/rootfs16' / rel
        same = prior.read_bytes() == data
        if not same:
            p.error('plaintext component differs from prior extraction: ' + rel)
        comparisons.append(dict(**record(prior), byte_identical=True))
        if rel == 'opt/bin/anacapad':
            im = Image(dest)
            if im.manifest()['sha256'] != 'bea55150c43eaadeec72b03a9494c32d96ec8f0b4406b1b84e3f30ff652e04ba':
                p.error('unexpected anacapad')
            manifest['W'] = im.manifest()
        else:
            manifest[rel] = record(dest)
            (out / (rel.replace('/', '-') + '.txt')).write_text(
                f'# Source: {dest}\n# Numbered display; hash covers original bytes.\n'
                + ''.join(f'{i:4}: {line}'.rstrip() + '\n' for i, line in
                          enumerate(data.decode().splitlines(), 1)))
    archive = json.loads(receipt.read_text())
    selected = [r for r in archive['packages'] if r['package_id'] == '57.23-74170-1-16']
    if len(selected) != 1 or selected[0]['package_sha256'] != manifest['package']['sha256']:
        p.error('archive run receipt package identity mismatch')
    # Deliberately omit recipient/key-source fields from the upstream receipt.
    archive_claim = {key: selected[0][key] for key in
                     ('package_id', 'release_tag', 'package_sha256', 'receipt',
                      'component_count', 'component_bytes')}
    (out / 'provenance.json').write_text(json.dumps(dict(
        extraction_commands=commands, previous_extraction_comparison=comparisons,
        archive_claim=archive_claim,
        limitation='The per-package receipt is not present at the referenced local path. '
                   'No local decryption binds plaintext component to encrypted package; '
                   'archive association is a capture claim, not cryptographic verification.'
    ), indent=2) + '\n')
    manifest['tool'] = dict(objdump=str(a.objdump), version=subprocess.run(
        [str(a.objdump), '--version'], check=True, capture_output=True,
        text=True).stdout.splitlines()[0])
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    rows = []
    for va in range(0x1053cd0c, 0x1053d604 + 28, 28):
        words = im.words(va, 7)
        ptr = int(words[3], 16)
        rows.append(dict(va=hex(va), file=hex(im.offset(va)), words=words,
                         path=im.string(ptr) if ptr else None))
    (out / 'routes.json').write_text(json.dumps(dict(image='W', stride=28,
        module_root=dict(va='0x105459b0', file=hex(im.offset(0x105459b0)),
                         words=im.words(0x105459b0, 2)),
        module=dict(va='0x1053cbd4', file=hex(im.offset(0x1053cbd4)),
                    words=im.words(0x1053cbd4, 15)), routes=rows), indent=2) + '\n')
    strings = (0x1048b808, 0x10497040, 0x1049704c, 0x10497068,
               0x1049707c, 0x10497090, 0x1043dbe4, 0x1043dbf8, 0x1043dc04)
    (out / 'constants.json').write_text(json.dumps(dict(W=[dict(
        va=hex(va), file=hex(im.offset(va)), value=im.string(va))
        for va in strings]), indent=2) + '\n')
    (out / 'relocations.json').write_text('[]\n')
    for label, start, end in (
        ('unlock-challenge', 0x102a8760, 0x102a89d0),
        ('unlock-confirmation', 0x102a89d0, 0x102a8d24),
        ('unlock-wrapper', 0x102a8e14, 0x102a9030),
        ('devmode-entry', 0x1028acf4, 0x1028b500),
        ('testenv-entry', 0x1028c258, 0x1028c700),
        ('process-helper', 0x10404730, 0x10404b00),
        ('process-leaf', 0x104044b4, 0x10404730),
    ):
        (out / (label + '.dis.txt')).write_text(disassemble(im, a.objdump, start, end))
    print(f'EVIDENCE: 57.23 plaintext component; {len(files)} allowlisted sources; '
          f'{len(rows)} exact route rows; prior extraction byte-identical')


if __name__ == '__main__':
    main()
