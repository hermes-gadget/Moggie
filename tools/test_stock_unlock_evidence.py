#!/usr/bin/env python3
"""Reject tampered offline evidence in isolated worktree scratch copies.

Requires the captured sources named in the packet manifest. No target code runs.
The committed packet and captured sources are never modified.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--document', type=Path, required=True)
    args = parser.parse_args()
    workspace = Path(__file__).resolve().parents[1]
    scratch = workspace / 'build' / 'unlock-analysis'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='evidence-negative-', dir=scratch) as temporary:
        packet = Path(temporary) / 'packet'
        shutil.copytree(args.evidence, packet)
        command = [sys.executable, str(workspace / 'tools/check_stock_unlock_evidence.py'),
                   '--evidence', str(packet), '--document', str(args.document.resolve())]

        def run():
            return subprocess.run(command, text=True, capture_output=True)

        baseline = run()
        if baseline.returncode:
            raise SystemExit('Baseline failed:\n' + baseline.stdout + baseline.stderr)

        def wrong_symbol(data):
            row = next(row for row in data if row.get('symbol'))
            row['symbol'] += '_tampered'

        def wrong_addend(data):
            data[0]['addend'] = hex(int(data[0]['addend'], 16) + 1)

        def wrong_route(data):
            row = next(row for row in data['routes'] if row['path'] == '/unlock')
            row['words'][5] = hex(int(row['words'][5], 16) ^ 1)

        def wrong_constant(data):
            key = 'A' if 'A' in data else 'Z'
            data[key][0]['value'] += '_tampered'

        def wrong_option(data):
            data['options'][0]['relative_delta'] += 4

        cases = [
            ('relocations.json', wrong_symbol, 'relocation symbol'),
            ('relocations.json', wrong_addend, 'relocation addend'),
            ('routes.json', wrong_route, 'route words'),
            ('constants.json', wrong_constant, 'constant string'),
        ]
        if (packet / 'ssh-options.json').is_file():
            cases.append(('ssh-options.json', wrong_option, 'option jump delta'))
        for filename, mutate, diagnostic in cases:
            target = packet / filename
            original = target.read_bytes()
            try:
                data = json.loads(original)
                mutate(data)
                target.write_text(json.dumps(data))
                result = run()
                expected = 'FAIL: ' + diagnostic
                if result.returncode == 0 or expected not in result.stdout + result.stderr:
                    raise SystemExit('Tamper test failed: ' + diagnostic + '\n'
                                     + result.stdout + result.stderr)
                print('REJECTED: ' + diagnostic)
            finally:
                target.write_bytes(original)
        print(f'NEGATIVE: {len(cases)} metadata mutations rejected; unmodified baseline passed')


if __name__ == '__main__':
    main()
