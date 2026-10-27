#!/usr/bin/env python3
"""Probe a Sonos speaker over the LAN and record what it exposes.

The point is to walk up to a powered Play:5 and capture everything that is
reachable *before* any jailbreak, so we know exactly what the stock surface
looks like and whether the network-only route is viable.

Every request is read-only except ``--unlock``, which POSTs to the ``/unlock``
handler found in ``anacapad``'s route table. That is an active jailbreak attempt
and is off unless asked for. Only run this against hardware you own.

Usage:
    speaker-probe.py probe --host 192.168.1.50 --out captures/play5
    speaker-probe.py probe --host 192.168.1.50 --unlock
    speaker-probe.py selftest
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.request

PORT = 1400

# Endpoints worth asking about. None of these are guesses at a password: they are
# the paths present in the stock firmware image.
ENDPOINTS = [
    ("/xml/device_description.xml", "UPnP device description"),
    ("/status/zp", "zone player status"),
    ("/info", "build and hardware info"),
    ("/getDSP", "DSP block graph (see docs/AUDIO.md)"),
    ("/dsp/configDSP.htm", "DSP configuration UI"),
    ("/dsp/eqdata.txt", "EQ presets"),
    ("/support/diagnostics", "diagnostics index"),
    ("/tools", "tool index"),
]

SSDP_ADDR = ("239.255.255.250", 1900)


def http_get(host: str, path: str, timeout: float = 5.0) -> tuple[int, bytes, dict]:
    url = f"http://{host}:{PORT}{path}"
    req = urllib.request.Request(url, headers={"User-Agent": "moggie-probe/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read(), dict(resp.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read()[:4096], dict(e.headers)
    except (urllib.error.URLError, socket.timeout, OSError) as e:
        return 0, str(e).encode(), {}


def http_post(host: str, path: str, body: bytes, timeout: float = 5.0) -> tuple[int, bytes]:
    url = f"http://{host}:{PORT}{path}"
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()[:4096]
    except urllib.error.HTTPError as e:
        return e.code, e.read()[:4096]
    except (urllib.error.URLError, socket.timeout, OSError) as e:
        return 0, str(e).encode()


def ssdp_search(timeout: float = 3.0) -> list[str]:
    msg = "\r\n".join([
        "M-SEARCH * HTTP/1.1",
        f"HOST: {SSDP_ADDR[0]}:{SSDP_ADDR[1]}",
        'MAN: "ssdp:discover"',
        "MX: 2",
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1",
        "", "",
    ]).encode()

    out: list[str] = []
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    try:
        s.sendto(msg, SSDP_ADDR)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                continue
            text = data.decode("utf-8", "replace")
            if addr[0] not in [a for a, _ in (o.split(" ", 1) for o in out)]:
                out.append(f"{addr[0]} {text.splitlines()[0] if text else ''}")
    finally:
        s.close()
    return out


def cmd_probe(args: argparse.Namespace) -> int:
    os.makedirs(args.out, exist_ok=True)
    print(f"probing {args.host}:{PORT}\n")

    reachable = 0
    results = []

    for path, label in ENDPOINTS:
        status, body, headers = http_get(args.host, path)
        name = path.strip("/").replace("/", "_") or "root"
        if 200 <= status < 300:
            reachable += 1
            with open(os.path.join(args.out, name), "wb") as fh:
                fh.write(body)
            results.append((path, status, len(body), label))
            print(f"  {status:>3}  {len(body):>8} B  {path:<32} {label}")
        elif status:
            # A 404 is a real answer: the endpoint is not on this firmware.
            results.append((path, status, 0, label))
            print(f"  {status:>3}  {'':>8}    {path:<32} {label}  (not present)")
        else:
            results.append((path, 0, 0, label))
            print(f"    -  {'':>8}    {path:<32} {label}  ({body.decode(errors='replace')[:40]})")

    print()

    if args.unlock:
        print("  POST /unlock  (active jailbreak attempt)")
        status, body = http_post(args.host, "/unlock", b"")
        print(f"  {status or 'no response'}  {body[:120]!r}")
        with open(os.path.join(args.out, "unlock-response.txt"), "wb") as fh:
            fh.write(body)

    ssdp = ssdp_search()
    if ssdp:
        print("\n  SSDP responders:")
        for line in ssdp:
            print(f"    {line}")
    else:
        print("\n  SSDP: no MediaRenderer responded")

    summary = {
        "host": args.host,
        "reachable_endpoints": reachable,
        "results": [
            {"path": p, "status": s, "bytes": n, "label": l} for p, s, n, l in results
        ],
        "ssdp": ssdp,
        "unlock_attempted": bool(args.unlock),
    }
    with open(os.path.join(args.out, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=2)

    print(f"\n{reachable} of {len(ENDPOINTS)} endpoints served content; saved to {args.out}/")
    return 0 if reachable else 1


def cmd_selftest(_: argparse.Namespace) -> int:
    """Exercise the parsers without a speaker."""
    checks = 0
    failures = 0

    def check(cond: bool, label: str) -> None:
        nonlocal checks, failures
        checks += 1
        if cond:
            print(f"  ok   {label}")
        else:
            print(f"  FAIL {label}")
            failures += 1

    # A closed port must produce status 0, not an exception.
    status, body, _ = http_get("127.0.0.1", "/anything", timeout=0.3)
    check(status == 0, "unreachable host yields status 0 instead of raising")

    # SSDP must return a list even when nothing answers.
    check(isinstance(ssdp_search(timeout=0.2), list), "ssdp_search returns a list")

    check(len(ENDPOINTS) >= 5, "endpoint table is populated")

    print(f"\nselftest: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Probe a Sonos speaker over the LAN.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="query a speaker")
    p.add_argument("--host", required=True)
    p.add_argument("--out", default="captures/speaker")
    p.add_argument("--unlock", action="store_true",
                   help="also POST /unlock (active jailbreak attempt)")
    p.set_defaults(func=cmd_probe)

    t = sub.add_parser("selftest", help="check the probe without hardware")
    t.set_defaults(func=cmd_selftest)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
