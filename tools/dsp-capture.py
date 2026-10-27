#!/usr/bin/env python3
"""Capture, inspect and diff a Sonos Play:5's DSP configuration.

The stock firmware ships a DSP configuration UI at
``http://<speaker>:1400/dsp/configDSP.htm`` (htdocs_locked on 57.10, htdocs on
34.16). The page is only a shell: it talks to three endpoints.

    GET  /getDSP             the whole DSP block graph as XML
    POST /putDSP             write it back
    POST /save_eq_presets    persist EQ presets to /jffs/eqdata.txt

The XML is a tree of ``<block name=...>`` elements, each holding
``<parameter name="<kind>_<name>" value="v1,v2,...">`` children. Coefficients
travel as comma-separated text.

Why this matters: the speaker's crossover, shelf filters, notch and limiting
are not hardcoded in anacapad, they are this configuration graph. Capturing it
from a real unit gives us the actual numbers instead of guesses, which is the
difference between "sounds roughly similar" and "the same filter".

``getDSP`` needs no authentication on the models this targets; it is the same
page the diagnostic UI uses. Run it against hardware only with permission.

Usage:
    dsp-capture.py fetch --host 192.168.1.50 --out play5-dsp.xml
    dsp-capture.py show play5-dsp.xml
    dsp-capture.py diff before.xml after.xml
    dsp-capture.py selftest
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
import xml.etree.ElementTree as ET
from typing import Dict, List, Tuple

DEFAULT_PORT = 1400


def fetch(host: str, port: int = DEFAULT_PORT, timeout: float = 10.0) -> bytes:
    url = f"http://{host}:{port}/getDSP"
    req = urllib.request.Request(url, headers={"Accept": "text/xml"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        if resp.status != 200:
            raise RuntimeError(f"{url} returned HTTP {resp.status}")
        return resp.read()


def walk(node: ET.Element, path: str = "") -> List[Tuple[str, str, str]]:
    """Flatten the block tree into (path, parameter name, value) rows."""
    rows: List[Tuple[str, str, str]] = []
    for child in node:
        name = child.get("name") or child.tag
        value = child.get("value")
        here = f"{path}/{name}" if path else name
        if value is not None:
            rows.append((path, name, value))
        else:
            rows.extend(walk(child, here))
    return rows


def blocks(root: ET.Element) -> List[ET.Element]:
    return [el for el in root.iter() if el.tag.lower() == "block"]


def cmd_show(args: argparse.Namespace) -> int:
    root = ET.parse(args.path).getroot()
    bs = blocks(root)
    if not bs:
        print("no <block> elements found — is this really a getDSP dump?", file=sys.stderr)
        return 1

    rows = walk(root)
    print(f"{len(bs)} block(s), {len(rows)} parameter(s)")
    print()
    current = None
    for parent, name, value in rows:
        if parent != current:
            print(f"[{parent or '/'}]")
            current = parent
        shown = value if len(value) <= 72 else value[:69] + "..."
        print(f"  {name:<28} {shown}")
    return 0


def cmd_diff(args: argparse.Namespace) -> int:
    a = {(p, n): v for p, n, v in walk(ET.parse(args.a).getroot())}
    b = {(p, n): v for p, n, v in walk(ET.parse(args.b).getroot())}

    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])

    if not (only_a or only_b or changed):
        print("identical: no parameter differs")
        return 0

    for key in only_a:
        print(f"only in {args.a}: {key[0]}/{key[1]} = {a[key]}")
    for key in only_b:
        print(f"only in {args.b}: {key[0]}/{key[1]} = {b[key]}")
    for key in changed:
        print(f"changed {key[0]}/{key[1]}: {a[key]} -> {b[key]}")

    print()
    print(f"{len(changed)} changed, {len(only_a)} removed, {len(only_b)} added")
    return 0


SAMPLE = """<?xml version="1.0"?>
<DSP>
  <block name="crossover">
    <parameter name="type" value="lowpass2"/>
    <parameter name="freq" value="2500"/>
    <parameter name="q" value="0.707"/>
  </block>
  <block name="eq">
    <block name="band0">
      <parameter name="freq" value="80"/>
      <parameter name="gain" value="-2.5"/>
    </block>
  </block>
</DSP>
"""


def cmd_selftest(_: argparse.Namespace) -> int:
    checks = 0
    failures = 0

    root = ET.fromstring(SAMPLE)
    bs = blocks(root)
    rows = walk(root)

    # Three <block> elements: crossover, eq, and the nested eq/band0.
    print(f"[ ok ] parsed {len(bs)} blocks" if len(bs) == 3 else f"[FAIL] block count {len(bs)}")
    checks += 1
    failures += 0 if len(bs) == 3 else 1

    expect = {
        ("crossover", "type"): "lowpass2",
        ("crossover", "freq"): "2500",
        ("eq/band0", "gain"): "-2.5",
    }
    got = {(p, n): v for p, n, v in rows}
    ok = all(got.get(k) == v for k, v in expect.items())
    print(f"[ {'ok' if ok else 'FAIL'} ] nested parameters flattened with their path")
    checks += 1
    failures += 0 if ok else 1

    # diff: identical, changed, added
    import tempfile
    import os

    with tempfile.TemporaryDirectory() as td:
        pa = os.path.join(td, "a.xml")
        pb = os.path.join(td, "b.xml")
        pc = os.path.join(td, "c.xml")
        open(pa, "w").write(SAMPLE)
        open(pb, "w").write(SAMPLE)
        open(pc, "w").write(SAMPLE.replace('"2500"', '"1800"'))

        same = cmd_diff(argparse.Namespace(a=pa, b=pb))
        checks += 1
        if same == 0:
            print("[ ok ] identical captures diff clean")
        else:
            print("[FAIL] identical captures reported a difference")
            failures += 1

        import io
        import contextlib

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            cmd_diff(argparse.Namespace(a=pa, b=pc))
        out = buf.getvalue()
        ok = "2500 -> 1800" in out and "1 changed" in out
        print(f"[ {'ok' if ok else 'FAIL'} ] changed parameter is reported")
        checks += 1
        failures += 0 if ok else 1

    print()
    print(f"selftest: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


def cmd_fetch(args: argparse.Namespace) -> int:
    data = fetch(args.host, args.port, args.timeout)
    root = ET.fromstring(data)
    n = len(blocks(root))
    with open(args.out, "wb") as fh:
        fh.write(data)
    print(f"wrote {len(data)} bytes to {args.out} ({n} blocks)")
    return 0


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Capture, inspect and diff a Sonos Play:5 DSP configuration."
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("fetch", help="GET /getDSP from a speaker and save it")
    f.add_argument("--host", required=True)
    f.add_argument("--port", type=int, default=DEFAULT_PORT)
    f.add_argument("--timeout", type=float, default=10.0)
    f.add_argument("--out", required=True)
    f.set_defaults(func=cmd_fetch)

    s = sub.add_parser("show", help="print a saved capture")
    s.add_argument("path")
    s.set_defaults(func=cmd_show)

    d = sub.add_parser("diff", help="compare two captures")
    d.add_argument("a")
    d.add_argument("b")
    d.set_defaults(func=cmd_diff)

    t = sub.add_parser("selftest", help="exercise the parser without hardware")
    t.set_defaults(func=cmd_selftest)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
