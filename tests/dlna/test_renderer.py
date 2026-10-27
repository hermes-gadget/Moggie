#!/usr/bin/env python3
"""Protocol-level tests for the Moggie UPnP AV MediaRenderer.

Standard library only. Starts the real binary on 127.0.0.1 and talks to it the
way a control point does:

  * a real SSDP M-SEARCH sent to 239.255.255.250:1900 (unicast fallback is used
    and reported if the environment has no multicast),
  * GET /description.xml and the three SCPD documents,
  * real SOAP POSTs, including the UPnPError faults,
  * GENA SUBSCRIBE/RENEW/UNSUBSCRIBE with a real callback server, asserting the
    NOTIFY bodies and sequence numbers,
  * SSDP alive re-advertisement and ssdp:byebye on shutdown.

Run it from the repository root or through `make -C firmware/dlna test`:

    python3 tests/dlna/test_renderer.py --binary firmware/dlna/build/host/dlna-renderer

Exit code 0 means every check passed. Failures print the daemon's log tail.
"""

from __future__ import annotations

import argparse
import html
import http.server
import os
import re
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

SSDP_GROUP = "239.255.255.250"
SSDP_PORT = 1900
TEST_UUID = "12345678-1234-4234-8234-123456789abc"
FRIENDLY = "Moggie Test Renderer"
CACHE_SECONDS = 6
ANNOUNCE_SECONDS = 2

SERVICE_DIRS = ("AVTransport", "RenderingControl", "ConnectionManager")
SERVICE_TYPES = {
    "AVTransport": "urn:schemas-upnp-org:service:AVTransport:1",
    "RenderingControl": "urn:schemas-upnp-org:service:RenderingControl:1",
    "ConnectionManager": "urn:schemas-upnp-org:service:ConnectionManager:1",
}
EXPECTED_ACTIONS = {
    "AVTransport": {
        "SetAVTransportURI", "Play", "Pause", "Stop", "Seek",
        "GetTransportInfo", "GetPositionInfo", "GetMediaInfo",
        "GetTransportSettings", "GetCurrentTransportActions",
        "GetDeviceCapabilities",
    },
    "RenderingControl": {
        "GetVolume", "SetVolume", "GetMute", "SetMute", "GetVolumeDB",
    },
    "ConnectionManager": {
        "GetProtocolInfo", "GetCurrentConnectionIDs",
        "GetCurrentConnectionInfo",
    },
}
SINK_PROTOCOLS = (
    "http-get:*:audio/mpeg:*",
    "http-get:*:audio/flac:*",
    "http-get:*:audio/wav:*",
    "http-get:*:audio/x-wav:*",
    "http-get:*:audio/aac:*",
    "http-get:*:audio/ogg:*",
)
DIDL = (
    '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
    'xmlns:dc="http://purl.org/dc/elements/1.1/" '
    'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
    '<item id="1" parentID="0" restricted="1">'
    '<dc:title>Test &amp; Tone</dc:title>'
    '<upnp:class>object.item.audioItem.musicTrack</upnp:class>'
    '<res protocolInfo="http-get:*:audio/mpeg:*" duration="0:03:00.000">'
    "http://127.0.0.1:9/tone.mp3?x=1&amp;y=2</res>"
    "</item></DIDL-Lite>"
)
TEST_URI = "http://127.0.0.1:9/tone.mp3?x=1&y=2"


# --------------------------------------------------------------------------
# small helpers


def local(tag):
    return tag.rsplit("}", 1)[-1]


def xml_root(body):
    try:
        return ET.fromstring(body)
    except ET.ParseError as exc:
        raise AssertionError("not valid XML: %s" % exc)


def child_text(elem, name):
    for child in elem:
        if local(child.tag) == name:
            return (child.text or "").strip()
    return None


def all_local(root, name):
    return [e for e in root.iter() if local(e.tag) == name]


def header_text(header):
    if header is None:
        return None
    return header.strip()


def escape(text):
    return html.escape(str(text), quote=False)


def arg(name, value):
    return "<%s>%s</%s>" % (name, escape(value), name)


def hms_to_seconds(text):
    m = re.fullmatch(r"(\d+):(\d{2}):(\d{2})", (text or "").strip())
    if not m:
        raise AssertionError("bad H:MM:SS value %r" % (text,))
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))


class Suite:
    def __init__(self):
        self.total = 0
        self.passed = 0
        self.skipped = 0
        self.failed = 0

    def check(self, condition, name, detail=""):
        self.total += 1
        if condition:
            self.passed += 1
            print("ok %d - %s" % (self.total, name))
            return True
        self.failed += 1
        print("not ok %d - %s" % (self.total, name))
        if detail:
            print("  # %s" % detail)
        return False

    def eq(self, name, got, want):
        return self.check(got == want, name, "got %r, want %r" % (got, want))

    def contains(self, name, haystack, needle):
        return self.check(needle in haystack, name,
                          "%r not in %r" % (needle, haystack[:400]))

    def skip(self, name, why):
        self.total += 1
        self.skipped += 1
        print("ok %d - %s # SKIP %s" % (self.total, name, why))

    def summary(self):
        print()
        print("%d checks: %d passed, %d skipped, %d failed"
              % (self.total, self.passed, self.skipped, self.failed))
        return self.failed == 0


class Resp:
    def __init__(self, status, headers, body):
        self.status = status
        self.headers = {str(k).lower(): str(v) for k, v in headers.items()}
        self.body = body

    @property
    def text(self):
        return self.body.decode("utf-8", "replace")

    def header(self, name):
        return self.headers.get(name.lower())


class Client:
    def __init__(self, base):
        self.base = base.rstrip("/")
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}))

    def request(self, method, path, body=None, headers=None, timeout=4.0):
        req = urllib.request.Request(
            self.base + path, data=body, headers=headers or {}, method=method)
        try:
            with self.opener.open(req, timeout=timeout) as r:
                return Resp(r.status, dict(r.headers.items()), r.read())
        except urllib.error.HTTPError as exc:
            return Resp(exc.code, dict(exc.headers.items()), exc.read())
        except Exception as exc:  # connection refused, timeout, ...
            return Resp(0, {}, ("transport error: %s" % exc).encode())


class Daemon:
    def __init__(self, binary, http_port, ssdp_port=SSDP_PORT,
                 uuid=TEST_UUID, backend="stub", mock_deny=False,
                 cache=CACHE_SECONDS, announce=ANNOUNCE_SECONDS,
                 log_path=None, log_level=3):
        self.binary = binary
        self.http_port = http_port
        self.ssdp_port = ssdp_port
        self.uuid = uuid
        self.backend = backend
        self.mock_deny = mock_deny
        self.cache = cache
        self.announce = announce
        self.log_level = log_level
        self.log_path = log_path or tempfile.mktemp(prefix="moggie-dlna-",
                                                    suffix=".log")
        self.proc = None
        self.log_file = None
        self.base = "http://127.0.0.1:%d" % http_port

    def start(self):
        args = [
            self.binary,
            "--http-port", str(self.http_port),
            "--http-addr", "127.0.0.1",
            "--ssdp-port", str(self.ssdp_port),
            "--ip", "127.0.0.1",
            "--uuid", self.uuid,
            "--friendly-name", FRIENDLY,
            "--cache-control", str(self.cache),
            "--announce-interval", str(self.announce),
            "--backend", self.backend,
            "--log-level", str(self.log_level),
        ]
        if self.mock_deny:
            args.append("--mock-deny")
        self.log_file = open(self.log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen(args, stdout=self.log_file,
                                     stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 5.0
        client = Client(self.base)
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("daemon exited during startup:\n%s"
                                   % self.log_tail())
            r = client.request("GET", "/description.xml", timeout=0.4)
            if r.status == 200:
                return
            time.sleep(0.05)
        raise RuntimeError("daemon did not answer HTTP within 5s:\n%s"
                           % self.log_tail())

    def rss_kb(self):
        if not self.proc or self.proc.poll() is not None:
            return None
        text = Path("/proc/%d/status" % self.proc.pid).read_text()
        values = {}
        for line in text.splitlines():
            if line.startswith(("VmRSS:", "VmHWM:")):
                key, value = line.split(":", 1)
                values[key] = int(value.strip().split()[0])
        return values

    def log_tail(self, lines=80):
        try:
            data = Path(self.log_path).read_text(errors="replace").splitlines()
        except OSError:
            return "(no log)"
        return "\n".join(data[-lines:])

    def log_text(self):
        try:
            return Path(self.log_path).read_text(errors="replace")
        except OSError:
            return ""

    def stop(self):
        if self.proc is None:
            return None
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        rc = self.proc.returncode
        if self.log_file:
            self.log_file.close()
            self.log_file = None
        return rc


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# --------------------------------------------------------------------------
# SSDP


class SSDPListener:
    """Listens on 1900 for NOTIFY. Multicast on loopback; no privileges."""

    def __init__(self, port=SSDP_PORT):
        self.events = []
        self.lock = threading.Lock()
        self.stop_flag = threading.Event()
        self.sock = None
        self.thread = None
        self.ok = False
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,
                          socket.IPPROTO_UDP)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", port))
            s.setsockopt(
                socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                socket.inet_aton(SSDP_GROUP) + socket.inet_aton("127.0.0.1"))
            s.settimeout(0.2)
        except OSError:
            s.close()
            return
        self.sock = s
        self.ok = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while not self.stop_flag.is_set():
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            text = data.decode("utf-8", "replace")
            if not text.startswith("NOTIFY"):
                continue
            headers = parse_headers(text)
            event = {
                "at": time.monotonic(),
                "addr": addr,
                "nt": headers.get("nt", ""),
                "nts": headers.get("nts", ""),
                "usn": headers.get("usn", ""),
                "location": headers.get("location", ""),
                "cache": headers.get("cache-control", ""),
                "raw": text,
            }
            with self.lock:
                self.events.append(event)

    def snapshot(self):
        with self.lock:
            return list(self.events)

    def clear(self):
        with self.lock:
            self.events = []

    def stop(self):
        self.stop_flag.set()
        if self.thread:
            self.thread.join(timeout=1)
        if self.sock:
            self.sock.close()


def parse_headers(text):
    """Parse an HTTP-like request/response/NOTIFY into a lower-case dict."""
    lines = text.replace("\r\n", "\n").split("\n")
    headers = {}
    for line in lines[1:]:
        if not line:
            break
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    return headers


def ssdp_search(st, port=SSDP_PORT, mx=1, mode="multicast", wait_extra=0.5,
                man='"ssdp:discover"'):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.settimeout(0.2)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                 socket.inet_aton("127.0.0.1"))
    msg = (
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: %s:%d\r\n"
        "MAN: %s\r\n"
        "MX: %d\r\n"
        "ST: %s\r\n"
        "\r\n" % (SSDP_GROUP, port, man, mx, st)
    ).encode("ascii")
    dest = (SSDP_GROUP, port) if mode == "multicast" else ("127.0.0.1", port)
    try:
        s.sendto(msg, dest)
    except OSError:
        s.close()
        return []
    out = []
    deadline = time.monotonic() + mx + wait_extra
    while time.monotonic() < deadline:
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        text = data.decode("utf-8", "replace")
        out.append({
            "status": text.split("\r\n", 1)[0],
            "headers": parse_headers(text),
            "raw": text,
            "addr": addr,
        })
    s.close()
    return out


# --------------------------------------------------------------------------
# GENA callback server


class Callback:
    def __init__(self):
        self.events = []
        self.lock = threading.Lock()
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_NOTIFY(self):
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length)
                event = {
                    "path": self.path,
                    "headers": {k.lower(): v for k, v in self.headers.items()},
                    "body": raw.decode("utf-8", "replace"),
                    "raw_len": len(raw),
                    "at": time.monotonic(),
                }
                with outer.lock:
                    outer.events.append(event)
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       daemon=True)
        self.thread.start()

    @property
    def url(self):
        return "http://127.0.0.1:%d/cb" % self.port

    def clear(self):
        with self.lock:
            self.events = []

    def snapshot(self):
        with self.lock:
            return list(self.events)

    def wait(self, predicate, timeout=4.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                hits = [e for e in self.events if predicate(e)]
            if hits:
                return hits[-1]
            time.sleep(0.05)
        return None

    def wait_count(self, count, timeout=4.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                if len(self.events) >= count:
                    return True
            time.sleep(0.05)
        return False

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=1)


def propertyset_values(body):
    root = xml_root(body)
    values = {}
    for prop in all_local(root, "property"):
        for child in prop:
            values[local(child.tag)] = (child.text or "")
    return values


def lastchange_values(body):
    props = propertyset_values(body)
    text = props.get("LastChange")
    if not text:
        return {}
    root = xml_root(text)
    values = {}
    for elem in root.iter():
        if local(elem.tag) in ("InstanceID", "Event"):
            continue
        if "val" in elem.attrib:
            values[local(elem.tag)] = elem.attrib["val"]
    return values


def wait_lastchange(callback, sid, predicate, timeout=4.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for event in callback.snapshot():
            if event["headers"].get("sid") != sid:
                continue
            try:
                values = lastchange_values(event["body"])
            except AssertionError:
                continue
            if predicate(values):
                return event, values
        time.sleep(0.05)
    return None, None


# --------------------------------------------------------------------------
# SOAP / HTTP helpers


class Renderer:
    def __init__(self, client):
        self.client = client

    def soap(self, service, action, args="", service_type=None,
             soapaction=None):
        stype = service_type or SERVICE_TYPES[service]
        envelope = (
            '<?xml version="1.0" encoding="utf-8"?>'
            '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
            "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>"
            % (action, stype, args, action)
        ).encode("utf-8")
        headers = {
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPACTION": ('"%s#%s"' % (stype, action)
                           if soapaction is None else soapaction),
        }
        return self.client.request("POST", "/%s/control" % service,
                                   envelope, headers)

    def fault_code(self, resp):
        try:
            root = xml_root(resp.body)
        except AssertionError:
            return None
        for elem in root.iter():
            if local(elem.tag) == "errorCode":
                try:
                    return int((elem.text or "").strip())
                except ValueError:
                    return None
        return None

    def value(self, resp, tag):
        try:
            root = xml_root(resp.body)
        except AssertionError:
            return None
        for elem in root.iter():
            if local(elem.tag) == tag:
                return (elem.text or "").strip()
        return None

    def subscribe(self, service, callback=None, nt="upnp:event",
                  timeout="Second-300", sid=None):
        headers = {}
        if callback is not None:
            headers["CALLBACK"] = "<%s>" % callback
        if nt is not None:
            headers["NT"] = nt
        if timeout is not None:
            headers["TIMEOUT"] = timeout
        if sid is not None:
            headers["SID"] = sid
        return self.client.request("SUBSCRIBE", "/%s/event" % service,
                                   headers=headers)

    def unsubscribe(self, service, sid):
        return self.client.request("UNSUBSCRIBE", "/%s/event" % service,
                                   headers={"SID": sid} if sid else {})


# --------------------------------------------------------------------------
# test sections


def test_cli(suite, binary):
    def run(*args):
        return subprocess.run([binary, *args], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=3)

    r = run("--version")
    suite.eq("--version exits 0", r.returncode, 0)
    suite.contains("--version string", r.stdout.decode(), "moggie-dlna")
    r = run("--help")
    suite.eq("--help exits 0", r.returncode, 0)
    r = run("--ip", "not-an-ip", "--http-port", str(free_port()))
    suite.eq("bad --ip exits 1", r.returncode, 1)
    r = run("--uuid", "not-a-uuid", "--http-port", str(free_port()))
    suite.eq("bad --uuid exits 1", r.returncode, 1)
    r = run("--backend", "nope", "--http-port", str(free_port()))
    suite.eq("bad --backend exits 1", r.returncode, 1)


def test_description(suite, client):
    resp = client.request("GET", "/description.xml")
    suite.eq("GET /description.xml status", resp.status, 200)
    suite.contains("description content-type", resp.header("content-type") or "",
                   "text/xml")
    root = xml_root(resp.body)
    suite.eq("description root element", local(root.tag), "root")

    devices = all_local(root, "device")
    suite.check(len(devices) == 1, "description has one device",
                "found %d" % len(devices))
    if not devices:
        return
    device = devices[0]
    suite.eq("deviceType", child_text(device, "deviceType"),
             "urn:schemas-upnp-org:device:MediaRenderer:1")
    suite.eq("friendlyName", child_text(device, "friendlyName"), FRIENDLY)
    suite.eq("UDN", child_text(device, "UDN"), "uuid:" + TEST_UUID)

    services = all_local(device, "service")
    suite.eq("service count", len(services), 3)
    seen = {}
    for svc in services:
        stype = child_text(svc, "serviceType")
        seen[stype] = {
            "serviceId": child_text(svc, "serviceId"),
            "scpd": child_text(svc, "SCPDURL"),
            "control": child_text(svc, "controlURL"),
            "event": child_text(svc, "eventSubURL"),
        }
    for service in SERVICE_DIRS:
        stype = SERVICE_TYPES[service]
        suite.check(stype in seen, "service present: %s" % service)
        if stype not in seen:
            continue
        suite.eq("%s SCPDURL" % service, seen[stype]["scpd"],
                 "/%s/scpd.xml" % service)
        suite.eq("%s controlURL" % service, seen[stype]["control"],
                 "/%s/control" % service)
        suite.eq("%s eventSubURL" % service, seen[stype]["event"],
                 "/%s/event" % service)
        suite.check(bool(seen[stype]["serviceId"]),
                    "%s serviceId present" % service)


def test_scpds(suite, client):
    for service in SERVICE_DIRS:
        resp = client.request("GET", "/%s/scpd.xml" % service)
        suite.eq("%s SCPD status" % service, resp.status, 200)
        try:
            root = xml_root(resp.body)
        except AssertionError as exc:
            suite.check(False, "%s SCPD parses" % service, str(exc))
            continue
        suite.eq("%s SCPD root" % service, local(root.tag), "scpd")

        state_vars = {}
        for sv in all_local(root, "stateVariable"):
            name = child_text(sv, "name")
            state_vars[name] = {
                "sendEvents": (sv.attrib.get("sendEvents") or "no"),
                "dataType": child_text(sv, "dataType"),
            }

        actions = {}
        for action in all_local(root, "action"):
            name = child_text(action, "name")
            args = []
            for argument in all_local(action, "argument"):
                args.append((
                    child_text(argument, "name"),
                    child_text(argument, "relatedStateVariable"),
                ))
            actions[name] = args

        suite.eq("%s action set" % service, set(actions),
                 EXPECTED_ACTIONS[service])
        for name, args in actions.items():
            for arg_name, related in args:
                suite.check(
                    related in state_vars,
                    "%s.%s argument %s has a declared state variable"
                    % (service, name, arg_name),
                    "%r missing" % (related,))
        suite.check(bool(state_vars), "%s declares state variables" % service)

    avt = client.request("GET", "/AVTransport/scpd.xml")
    avt_root = xml_root(avt.body)
    avt_vars = {child_text(sv, "name"): sv
                for sv in all_local(avt_root, "stateVariable")}
    suite.check("TransportState" in avt_vars,
                "AVTransport declares TransportState")
    allowed = [e.text for e in avt_root.iter()
               if local(e.tag) == "allowedValue"]
    for value in ("STOPPED", "PLAYING", "PAUSED_PLAYBACK"):
        suite.check(value in allowed,
                    "TransportState allows %s" % value)

    rcs = client.request("GET", "/RenderingControl/scpd.xml")
    rcs_root = xml_root(rcs.body)
    ranges = [e for e in rcs_root.iter()
              if local(e.tag) == "allowedValueRange"]
    suite.check(any(child_text(r, "minimum") == "0" and
                    child_text(r, "maximum") == "100" for r in ranges),
                "RenderingControl volume range is 0..100")

    for service in ("AVTransport", "RenderingControl"):
        body = client.request("GET", "/%s/scpd.xml" % service).body
        root = xml_root(body)
        last = [sv for sv in all_local(root, "stateVariable")
                if child_text(sv, "name") == "LastChange"]
        suite.check(len(last) == 1 and last[0].attrib.get("sendEvents") == "yes",
                    "%s LastChange is evented" % service)

    # Regression: the RenderingControl SCPD used to declare only A_ARG_TYPE_*
    # variables, with action arguments relating to those instead of to the
    # service's own state variables. UPnP requires relatedStateVariable to name
    # a state variable of the service, and control points read its range.
    #
    # async_upnp_client's DmrDevice._supports() is literally
    #   state_variable("RC", name) is not None and action("RC", "Set"+name)
    # so a missing Volume variable made Home Assistant expose a media_player
    # with no volume control at all -- 306 passing protocol checks missed it,
    # a real HA instance found it immediately.
    decl = {child_text(sv, "name"): sv
            for sv in all_local(rcs_root, "stateVariable")}
    for var in ("Volume", "Mute", "VolumeDB"):
        suite.check(var in decl,
                    "RenderingControl declares the %s state variable" % var,
                    "declared: %s" % sorted(decl))

    vol = decl.get("Volume")
    if vol is not None:
        rng = [e for e in vol.iter() if local(e.tag) == "allowedValueRange"]
        suite.check(bool(rng) and child_text(rng[0], "minimum") == "0" and
                    child_text(rng[0], "maximum") == "100",
                    "RenderingControl Volume carries its 0..100 range")
        suite.check(vol.attrib.get("sendEvents") == "yes",
                    "RenderingControl Volume is evented")

    # Every argument must relate to a state variable the service declares.
    missing = []
    for action in all_local(rcs_root, "action"):
        for arg in all_local(action, "argument"):
            rel = child_text(arg, "relatedStateVariable")
            if rel and rel not in decl:
                missing.append("%s/%s" % (child_text(action, "name"), rel))
    suite.check(not missing,
                "every RenderingControl argument relates to a declared variable",
                ", ".join(missing))


def test_http_edges(suite, client, port):
    suite.eq("GET / is 404", client.request("GET", "/").status, 404)
    suite.eq("GET unknown SCPD is 404",
             client.request("GET", "/Nope/scpd.xml").status, 404)
    suite.eq("PUT is 405",
             client.request("PUT", "/description.xml").status, 405)

    def raw(data, timeout=3):
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        try:
            s.sendall(data)
            chunks = []
            while True:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    break
                if not chunk:
                    break
                chunks.append(chunk)
            return b"".join(chunks)
        finally:
            s.close()

    bad = raw(b"BOGUS\r\n\r\n")
    suite.check(bad.startswith(b"HTTP/1.1 400"), "malformed request is 400",
                bad[:200].decode("latin-1", "replace"))
    big = raw(b"GET /description.xml HTTP/1.1\r\nHost: x\r\n"
              b"Content-Length: 999999\r\n\r\n")
    suite.check(big.startswith(b"HTTP/1.1 413"),
                "oversized Content-Length is 413",
                big[:200].decode("latin-1", "replace"))

    # A length near SIZE_MAX used to wrap the size_t body_total addition to a
    # small value, which then passed the 413 check and satisfied
    # `req_len >= body_total`, dispatching the request with a truncated body.
    # The bound must be applied to the raw length, before the add.
    wrap = raw(b"GET /description.xml HTTP/1.1\r\nHost: x\r\n"
               b"Content-Length: 18446744073709551615\r\n\r\n")
    suite.check(wrap.startswith(b"HTTP/1.1 413"),
                "Content-Length that would wrap size_t is 413",
                wrap[:200].decode("latin-1", "replace"))

    # Just past the limit on the other side: parses fine, still too large.
    edge = raw(b"GET /description.xml HTTP/1.1\r\nHost: x\r\n"
               b"Content-Length: 8193\r\n\r\n")
    suite.check(edge.startswith(b"HTTP/1.1 413"),
                "Content-Length one byte over DLNA_REQ_MAX (8192) is 413",
                edge[:200].decode("latin-1", "replace"))

    # RFC 7230 3.3.3: conflicting Content-Length values must be rejected, not
    # resolved last-one-wins. Previously the second value won and the connection
    # then hung until the accept deadline closed it with no response at all.
    dup = raw(b"POST /AVTransport/control HTTP/1.1\r\nHost: x\r\n"
              b"Content-Length: 0\r\nContent-Length: 400\r\n\r\n")
    suite.check(dup.startswith(b"HTTP/1.1 400"),
                "conflicting Content-Length is 400",
                dup[:200].decode("latin-1", "replace"))

    # Two identical values are legal and must still work.
    same = raw(b"POST /AVTransport/control HTTP/1.1\r\nHost: x\r\n"
               b"Content-Length: 0\r\nContent-Length: 0\r\n\r\n")
    suite.check(same.startswith(b"HTTP/1.1 "),
                "duplicate but identical Content-Length still answers",
                same[:120].decode("latin-1", "replace"))


def test_connection_manager(suite, renderer, client):
    resp = renderer.soap("ConnectionManager", "GetProtocolInfo")
    suite.eq("GetProtocolInfo status", resp.status, 200)
    sink = renderer.value(resp, "Sink") or ""
    for protocol in SINK_PROTOCOLS:
        suite.contains("sink protocol %s" % protocol, sink, protocol)
    suite.eq("GetProtocolInfo Source", renderer.value(resp, "Source"), "")

    resp = renderer.soap("ConnectionManager", "GetCurrentConnectionIDs")
    suite.eq("GetCurrentConnectionIDs status", resp.status, 200)
    suite.eq("connection IDs", renderer.value(resp, "ConnectionIDs"), "0")

    resp = renderer.soap("ConnectionManager", "GetCurrentConnectionInfo",
                         arg("ConnectionID", 0))
    suite.eq("GetCurrentConnectionInfo status", resp.status, 200)
    suite.eq("connection direction", renderer.value(resp, "Direction"), "Input")
    suite.eq("connection status", renderer.value(resp, "Status"), "OK")
    suite.eq("connection AVTransportID",
             renderer.value(resp, "AVTransportID"), "0")
    suite.eq("connection RcsID", renderer.value(resp, "RcsID"), "0")
    suite.eq("connection PeerConnectionID",
             renderer.value(resp, "PeerConnectionID"), "-1")

    resp = renderer.soap("ConnectionManager", "GetCurrentConnectionInfo",
                         arg("ConnectionID", 5))
    suite.eq("bad connection id status", resp.status, 500)
    suite.eq("bad connection id fault", renderer.fault_code(resp), 402)


def test_avtransport(suite, renderer):
    # Fault codes below are from UPnP AVTransport:1, not from this renderer's
    # behaviour. 718 = Invalid InstanceID, 717 = Play speed not supported,
    # 710 = Seek mode not supported, 701 = Transition not available. They were
    # all 402/701 once, and the tests asserted whatever the code did -- which
    # made the suite unable to detect the conformance bug.
    # Before any URI: Play/Pause/Seek must be refused, Stop is legal.
    resp = renderer.soap("AVTransport", "Play",
                         arg("InstanceID", 0) + arg("Speed", "1"))
    suite.eq("Play before URI fault", renderer.fault_code(resp), 701)

    resp = renderer.soap("AVTransport", "Pause", arg("InstanceID", 0))
    suite.eq("Pause before URI fault", renderer.fault_code(resp), 701)

    resp = renderer.soap("AVTransport", "Seek",
                         arg("InstanceID", 0) + arg("Unit", "REL_TIME") +
                         arg("Target", "00:00:10"))
    suite.eq("Seek before URI fault", renderer.fault_code(resp), 701)

    resp = renderer.soap("AVTransport", "Stop", arg("InstanceID", 0))
    suite.eq("Stop before URI is accepted", resp.status, 200)

    resp = renderer.soap("AVTransport", "GetTransportInfo",
                         arg("InstanceID", 0))
    suite.eq("initial transport state", renderer.value(resp,
             "CurrentTransportState"), "STOPPED")
    suite.eq("initial transport status", renderer.value(resp,
             "CurrentTransportStatus"), "OK")
    suite.eq("initial speed", renderer.value(resp, "CurrentSpeed"), "1")

    resp = renderer.soap("AVTransport", "GetMediaInfo", arg("InstanceID", 0))
    suite.eq("initial NrTracks", renderer.value(resp, "NrTracks"), "0")
    suite.eq("initial CurrentURI", renderer.value(resp, "CurrentURI"), "")

    resp = renderer.soap("AVTransport", "SetAVTransportURI",
                         arg("InstanceID", 0) + arg("CurrentURI", TEST_URI) +
                         arg("CurrentURIMetaData", DIDL))
    suite.eq("SetAVTransportURI status", resp.status, 200)
    suite.check(b"SetAVTransportURIResponse" in resp.body,
                "SetAVTransportURI response element")

    # The URI and metadata are published XML-escaped inside a fixed 4 KiB
    # LastChange document. A URI that fits its own 511-byte cap can still
    # expand past the document (every '&' becomes '&amp;'), and the old code
    # answered 200 and then silently failed to build the event -- so the
    # control point was never told what it had just set. It must be refused.
    explode = "&" * 500
    resp = renderer.soap("AVTransport", "SetAVTransportURI",
                         arg("InstanceID", 0) + arg("CurrentURI", explode) +
                         arg("CurrentURIMetaData", DIDL))
    suite.check(resp.status != 200,
                "SetAVTransportURI that would overflow LastChange is refused",
                "status=%d" % resp.status)

    # The good URI must still work afterwards, i.e. the refusal left no state.
    resp = renderer.soap("AVTransport", "GetMediaInfo", arg("InstanceID", 0))
    suite.eq("CurrentURI unchanged after refusal",
             renderer.value(resp, "CurrentURI"), TEST_URI)

    resp = renderer.soap("AVTransport", "GetMediaInfo", arg("InstanceID", 0))
    suite.eq("NrTracks after URI", renderer.value(resp, "NrTracks"), "1")
    suite.eq("CurrentURI round-trip", renderer.value(resp, "CurrentURI"),
             TEST_URI)
    suite.eq("CurrentURIMetaData round-trip",
             renderer.value(resp, "CurrentURIMetaData"), DIDL)
    suite.eq("MediaDuration from DIDL",
             renderer.value(resp, "MediaDuration"), "0:03:00")

    resp = renderer.soap("AVTransport", "GetPositionInfo",
                         arg("InstanceID", 0))
    suite.eq("Track after URI", renderer.value(resp, "Track"), "1")
    suite.eq("TrackDuration", renderer.value(resp, "TrackDuration"), "0:03:00")
    suite.eq("TrackURI", renderer.value(resp, "TrackURI"), TEST_URI)
    suite.eq("RelTime initial", renderer.value(resp, "RelTime"), "0:00:00")

    resp = renderer.soap("AVTransport", "GetTransportSettings",
                         arg("InstanceID", 0))
    suite.eq("GetTransportSettings status", resp.status, 200)
    suite.eq("PlayMode", renderer.value(resp, "PlayMode"), "NORMAL")
    resp = renderer.soap("AVTransport", "GetDeviceCapabilities",
                         arg("InstanceID", 0))
    suite.eq("GetDeviceCapabilities status", resp.status, 200)
    suite.eq("PlayMedia", renderer.value(resp, "PlayMedia"), "NETWORK")
    resp = renderer.soap("AVTransport", "GetCurrentTransportActions",
                         arg("InstanceID", 0))
    suite.contains("transport actions while stopped",
                   renderer.value(resp, "Actions") or "", "Play")

    resp = renderer.soap("AVTransport", "Play",
                         arg("InstanceID", 0) + arg("Speed", "1"))
    suite.eq("Play status", resp.status, 200)
    resp = renderer.soap("AVTransport", "GetTransportInfo",
                         arg("InstanceID", 0))
    suite.eq("state after Play", renderer.value(resp,
             "CurrentTransportState"), "PLAYING")
    time.sleep(1.1)
    resp = renderer.soap("AVTransport", "GetPositionInfo",
                         arg("InstanceID", 0))
    position = hms_to_seconds(renderer.value(resp, "RelTime"))
    suite.check(1 <= position <= 5, "position advances while playing",
                "RelTime=%r" % (renderer.value(resp, "RelTime"),))

    resp = renderer.soap("AVTransport", "Pause", arg("InstanceID", 0))
    suite.eq("Pause status", resp.status, 200)
    resp = renderer.soap("AVTransport", "GetTransportInfo",
                         arg("InstanceID", 0))
    suite.eq("state after Pause", renderer.value(resp,
             "CurrentTransportState"), "PAUSED_PLAYBACK")
    before = hms_to_seconds(renderer.value(
        renderer.soap("AVTransport", "GetPositionInfo",
                      arg("InstanceID", 0)), "RelTime"))
    time.sleep(1.0)
    after = hms_to_seconds(renderer.value(
        renderer.soap("AVTransport", "GetPositionInfo",
                      arg("InstanceID", 0)), "RelTime"))
    suite.eq("position frozen while paused", after, before)

    resp = renderer.soap("AVTransport", "Seek",
                         arg("InstanceID", 0) + arg("Unit", "REL_TIME") +
                         arg("Target", "00:00:30"))
    suite.eq("Seek status", resp.status, 200)
    resp = renderer.soap("AVTransport", "GetPositionInfo",
                         arg("InstanceID", 0))
    suite.eq("position after Seek", hms_to_seconds(
        renderer.value(resp, "RelTime")), 30)

    resp = renderer.soap("AVTransport", "Stop", arg("InstanceID", 0))
    suite.eq("Stop status", resp.status, 200)
    resp = renderer.soap("AVTransport", "GetTransportInfo",
                         arg("InstanceID", 0))
    suite.eq("state after Stop", renderer.value(resp,
             "CurrentTransportState"), "STOPPED")
    resp = renderer.soap("AVTransport", "GetPositionInfo",
                         arg("InstanceID", 0))
    suite.eq("position after Stop", renderer.value(resp, "RelTime"), "0:00:00")

    # Stop released the audio path; Play must be able to request it again.
    resp = renderer.soap("AVTransport", "Play",
                         arg("InstanceID", 0) + arg("Speed", "1"))
    suite.eq("Play after Stop reacquires audio", resp.status, 200)
    renderer.soap("AVTransport", "Stop", arg("InstanceID", 0))

    # Negative arguments and transitions.
    resp = renderer.soap("AVTransport", "Play",
                         arg("InstanceID", 0) + arg("Speed", "2"))
    suite.eq("non-1 Speed fault", renderer.fault_code(resp), 717)
    resp = renderer.soap("AVTransport", "Pause", arg("InstanceID", 0))
    suite.eq("Pause while stopped fault", renderer.fault_code(resp), 701)
    resp = renderer.soap("AVTransport", "Seek",
                         arg("InstanceID", 0) + arg("Unit", "REL_TIME") +
                         arg("Target", "00:00:05"))
    suite.eq("Seek while stopped fault", renderer.fault_code(resp), 701)
    resp = renderer.soap("AVTransport", "Seek",
                         arg("InstanceID", 0) + arg("Unit", "ABS_TIME") +
                         arg("Target", "00:00:05"))
    suite.eq("unsupported Seek unit fault", renderer.fault_code(resp), 710)
    resp = renderer.soap("AVTransport", "Seek",
                         arg("InstanceID", 0) + arg("Unit", "REL_TIME") +
                         arg("Target", "not-a-time"))
    suite.eq("bad Seek target fault", renderer.fault_code(resp), 402)
    resp = renderer.soap("AVTransport", "SetAVTransportURI",
                         arg("InstanceID", 0) + arg("CurrentURI", ""))
    suite.eq("empty URI fault", renderer.fault_code(resp), 402)
    resp = renderer.soap("AVTransport", "GetMediaInfo",
                         arg("InstanceID", 1))
    suite.eq("wrong InstanceID fault", renderer.fault_code(resp), 718)
    resp = renderer.soap("AVTransport", "GetMediaInfo")
    suite.eq("missing InstanceID fault", renderer.fault_code(resp), 402)


def test_rendering_control(suite, renderer):
    renderer.soap("RenderingControl", "SetVolume",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredVolume", 30))

    resp = renderer.soap("RenderingControl", "GetVolume",
                         arg("InstanceID", 0) + arg("Channel", "Master"))
    suite.eq("GetVolume status", resp.status, 200)
    suite.eq("GetVolume value", renderer.value(resp, "CurrentVolume"), "30")

    resp = renderer.soap("RenderingControl", "SetVolume",
                         arg("InstanceID", 0) + arg("Channel", "Master") +
                         arg("DesiredVolume", 42))
    suite.eq("SetVolume status", resp.status, 200)
    resp = renderer.soap("RenderingControl", "GetVolume",
                         arg("InstanceID", 0) + arg("Channel", "Master"))
    suite.eq("volume stored", renderer.value(resp, "CurrentVolume"), "42")

    resp = renderer.soap("RenderingControl", "SetVolume",
                         arg("InstanceID", 0) + arg("Channel", "Master") +
                         arg("DesiredVolume", 101))
    suite.eq("volume 101 fault", renderer.fault_code(resp), 402)
    resp = renderer.soap("RenderingControl", "SetVolume",
                         arg("InstanceID", 0) + arg("Channel", "Master") +
                         arg("DesiredVolume", -1))
    suite.eq("volume -1 fault", renderer.fault_code(resp), 402)
    resp = renderer.soap("RenderingControl", "SetVolume",
                         arg("InstanceID", 0) + arg("Channel", "Master") +
                         arg("DesiredVolume", "loud"))
    suite.eq("volume non-numeric fault", renderer.fault_code(resp), 402)
    resp = renderer.soap("RenderingControl", "SetVolume",
                         arg("InstanceID", 0) + arg("Channel", "Master"))
    suite.eq("missing DesiredVolume fault", renderer.fault_code(resp), 402)
    resp = renderer.soap("RenderingControl", "GetVolume",
                         arg("InstanceID", 0) + arg("Channel", "LF"))
    suite.eq("bad channel fault", renderer.fault_code(resp), 402)

    resp = renderer.soap("RenderingControl", "SetMute",
                         arg("InstanceID", 0) + arg("Channel", "Master") +
                         arg("DesiredMute", 1))
    suite.eq("SetMute status", resp.status, 200)
    resp = renderer.soap("RenderingControl", "GetMute",
                         arg("InstanceID", 0) + arg("Channel", "Master"))
    suite.eq("mute stored", renderer.value(resp, "CurrentMute"), "1")
    renderer.soap("RenderingControl", "SetMute",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredMute", "false"))
    resp = renderer.soap("RenderingControl", "GetMute",
                         arg("InstanceID", 0) + arg("Channel", "Master"))
    suite.eq("mute false accepted", renderer.value(resp, "CurrentMute"), "0")
    resp = renderer.soap("RenderingControl", "SetMute",
                         arg("InstanceID", 0) + arg("Channel", "Master") +
                         arg("DesiredMute", "maybe"))
    suite.eq("bad mute fault", renderer.fault_code(resp), 402)

    resp = renderer.soap("RenderingControl", "GetVolumeDB",
                         arg("InstanceID", 0) + arg("Channel", "Master"))
    suite.eq("GetVolumeDB status", resp.status, 200)
    db = int(renderer.value(resp, "CurrentVolume"))
    suite.check(-10240 <= db <= 0, "GetVolumeDB in SCPD range",
                "CurrentVolume=%d" % db)

    # The full 101-step range must map to distinct 1/256 dB values. Truncating
    # to whole dB first collapsed it to 41 values and made 98, 99 and 100 all
    # report 0 (full scale) although 99 is attenuated by 0.4 dB.
    seen = {}
    for vol in (0, 1, 50, 98, 99, 100):
        renderer.soap("RenderingControl", "SetVolume",
                      arg("InstanceID", 0) + arg("Channel", "Master") +
                      arg("DesiredVolume", vol))
        r = renderer.soap("RenderingControl", "GetVolumeDB",
                          arg("InstanceID", 0) + arg("Channel", "Master"))
        seen[vol] = int(renderer.value(r, "CurrentVolume"))
    suite.eq("volume 0 dB", seen[0], -10240)
    suite.eq("volume 100 dB", seen[100], 0)
    suite.check(seen[99] != 0, "volume 99 is not full scale",
                "CurrentVolume=%d" % seen[99])
    suite.check(len(set(seen.values())) == len(seen),
                "volume steps map to distinct dB values", str(seen))
    renderer.soap("RenderingControl", "SetVolume",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredVolume", 50))


def test_soap_errors(suite, renderer):
    resp = renderer.soap("AVTransport", "Frobnicate", arg("InstanceID", 0))
    suite.eq("unknown action status", resp.status, 500)
    suite.eq("unknown action fault", renderer.fault_code(resp), 401)
    suite.contains("fault says invalid action", resp.text, "Invalid Action")

    resp = renderer.soap(
        "AVTransport", "Play",
        arg("InstanceID", 0) + arg("Speed", "1"),
        soapaction='"urn:schemas-upnp-org:service:RenderingControl:1#Play"')
    suite.eq("service mismatch fault", renderer.fault_code(resp), 401)

    resp = renderer.soap(
        "AVTransport", "Play", arg("InstanceID", 0) + arg("Speed", "1"),
        soapaction='"urn:schemas-upnp-org:service:AVTransport:1"')
    suite.eq("SOAPACTION without action fault", renderer.fault_code(resp), 401)

    envelope = (b'<?xml version="1.0"?><s:Envelope '
                b'xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                b'<s:Body><u:Play xmlns:u="urn:schemas-upnp-org:service:'
                b'AVTransport:1"><InstanceID>0</InstanceID></u:Play>'
                b"</s:Body></s:Envelope>")
    resp = renderer.client.request(
        "POST", "/AVTransport/control", envelope,
        {"Content-Type": 'text/xml; charset="utf-8"'})
    suite.eq("missing SOAPACTION fault", renderer.fault_code(resp), 401)

    resp = renderer.client.request(
        "POST", "/AVTransport/control", b"",
        {"Content-Type": 'text/xml; charset="utf-8"',
         "SOAPACTION": '"urn:schemas-upnp-org:service:AVTransport:1#Play"'})
    suite.eq("empty SOAP body fault", renderer.fault_code(resp), 402)


def test_gena_avt(suite, renderer, callback):
    callback.clear()
    resp = renderer.subscribe("AVTransport", callback.url,
                              timeout="Second-300")
    suite.eq("AVT SUBSCRIBE status", resp.status, 200)
    sid = resp.header("sid")
    suite.check(bool(sid and re.fullmatch(
        r"uuid:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",
        sid)), "AVT SID is a uuid", repr(sid))
    suite.eq("AVT SUBSCRIBE timeout", resp.header("timeout"), "Second-300")
    suite.eq("AVT SUBSCRIBE body length", resp.header("content-length"), "0")

    event = callback.wait(lambda e: e["headers"].get("sid") == sid, timeout=3)
    suite.check(event is not None, "initial AVT NOTIFY arrives")
    if not event:
        return
    # UPnP GENA: the first event of a subscription carries SEQ 1. 0 is reserved.
    suite.eq("initial NOTIFY SEQ", event["headers"].get("seq"), "1")
    suite.eq("initial NOTIFY NT", event["headers"].get("nt"), "upnp:event")
    suite.eq("initial NOTIFY NTS", event["headers"].get("nts"),
             "upnp:propchange")
    suite.contains("initial NOTIFY content-type",
                   event["headers"].get("content-type") or "", "text/xml")
    suite.eq("initial NOTIFY path", event["path"], "/cb")
    suite.eq("NOTIFY Content-Length matches body",
             int(event["headers"].get("content-length", "-1")),
             event["raw_len"])
    values = lastchange_values(event["body"])
    suite.eq("initial TransportState", values.get("TransportState"), "STOPPED")
    suite.eq("initial AVTransportURI", values.get("AVTransportURI"), TEST_URI)
    suite.eq("initial duration", values.get("CurrentTrackDuration"), "0:03:00")

    resp = renderer.soap("AVTransport", "Play",
                         arg("InstanceID", 0) + arg("Speed", "1"))
    suite.eq("GENA Play status", resp.status, 200)
    event, values = wait_lastchange(
        callback, sid, lambda v: v.get("TransportState") == "PLAYING")
    suite.check(event is not None, "NOTIFY reports PLAYING after Play")
    if event:
        suite.eq("PLAYING SEQ", event["headers"].get("seq"), "2")
        suite.eq("PLAYING URI", values.get("AVTransportURI"), TEST_URI)

    resp = renderer.soap("AVTransport", "Pause", arg("InstanceID", 0))
    event, values = wait_lastchange(
        callback, sid, lambda v: v.get("TransportState") == "PAUSED_PLAYBACK")
    suite.check(event is not None, "NOTIFY reports PAUSED_PLAYBACK")

    resp = renderer.soap("AVTransport", "Seek",
                         arg("InstanceID", 0) + arg("Unit", "REL_TIME") +
                         arg("Target", "00:00:30"))
    renderer.soap("AVTransport", "Stop", arg("InstanceID", 0))
    event, values = wait_lastchange(
        callback, sid, lambda v: v.get("TransportState") == "STOPPED")
    suite.check(event is not None, "NOTIFY reports STOPPED after Stop")

    seqs = []
    for item in callback.snapshot():
        if item["headers"].get("sid") == sid:
            seqs.append(int(item["headers"].get("seq", "-1")))
    # 1-based and contiguous: the first event is 1, so a run of n events is
    # range(1, n + 1). This asserted 0-based until the spec was checked.
    suite.check(seqs and seqs[0] == 1 and seqs == list(range(1, len(seqs) + 1)),
                "AVT SEQ starts at 1 and increments by one", repr(seqs))
    return sid


def test_gena_rcs(suite, renderer, callback):
    renderer.soap("RenderingControl", "SetVolume",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredVolume", 30))
    callback.clear()
    resp = renderer.subscribe("RenderingControl", callback.url,
                              timeout="Second-300")
    suite.eq("RCS SUBSCRIBE status", resp.status, 200)
    sid = resp.header("sid")
    suite.check(bool(sid), "RCS SID present")

    event = callback.wait(lambda e: e["headers"].get("sid") == sid, timeout=3)
    suite.check(event is not None, "initial RCS NOTIFY arrives")

    # The evented VolumeDB must agree with what GetVolumeDB returns. These are
    # two independent code paths (event.c builds the LastChange, soap.c answers
    # the action) and they disagreed: event.c applied the 1/256 scaling twice,
    # so volume 30 evented -1,835,008 while SOAP returned -7,168.
    if event:
        ev = lastchange_values(event["body"])
        soap = renderer.soap("RenderingControl", "GetVolumeDB",
                             arg("InstanceID", 0) + arg("Channel", "Master"))
        soap_db = int(renderer.value(soap, "CurrentVolume"))
        suite.check("VolumeDB" in ev,
                    "LastChange carries VolumeDB",
                    "keys: %s" % sorted(ev))
        if "VolumeDB" in ev:
            suite.eq("evented VolumeDB matches GetVolumeDB",
                     int(ev["VolumeDB"]), soap_db)
    if event:
        values = lastchange_values(event["body"])
        suite.eq("initial Volume", values.get("Volume"), "30")
        suite.eq("initial Mute", values.get("Mute"), "0")
        suite.check("VolumeDB" in values, "initial VolumeDB present")

    renderer.soap("RenderingControl", "SetVolume",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredVolume", 55))
    event, values = wait_lastchange(
        callback, sid, lambda v: v.get("Volume") == "55")
    suite.check(event is not None, "NOTIFY reports volume 55")
    renderer.soap("RenderingControl", "SetMute",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredMute", 1))
    event, values = wait_lastchange(
        callback, sid, lambda v: v.get("Mute") == "1")
    suite.check(event is not None, "NOTIFY reports mute 1")

    # Renewal keeps the same SID and applies the new timeout.
    resp = renderer.subscribe("RenderingControl", sid=sid,
                              timeout="Second-120", nt=None)
    suite.eq("RCS renewal status", resp.status, 200)
    suite.eq("RCS renewal SID", resp.header("sid"), sid)
    suite.eq("RCS renewal timeout", resp.header("timeout"), "Second-120")

    resp = renderer.unsubscribe("RenderingControl", sid)
    suite.eq("RCS UNSUBSCRIBE status", resp.status, 200)
    count = len([e for e in callback.snapshot()
                 if e["headers"].get("sid") == sid])
    renderer.soap("RenderingControl", "SetVolume",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredVolume", 44))
    time.sleep(0.8)
    after = len([e for e in callback.snapshot()
                 if e["headers"].get("sid") == sid])
    suite.eq("no NOTIFY after UNSUBSCRIBE", after, count)


def test_gena_cm(suite, renderer, callback):
    callback.clear()
    resp = renderer.subscribe("ConnectionManager", callback.url,
                              timeout="Second-300")
    suite.eq("CM SUBSCRIBE status", resp.status, 200)
    sid = resp.header("sid")
    event = callback.wait(lambda e: e["headers"].get("sid") == sid, timeout=3)
    suite.check(event is not None, "initial CM NOTIFY arrives")
    if event:
        props = propertyset_values(event["body"])
        suite.contains("CM SinkProtocolInfo has flac",
                       props.get("SinkProtocolInfo", ""), "audio/flac")
        suite.eq("CM CurrentConnectionIDs", props.get("CurrentConnectionIDs"),
                 "0")
        suite.eq("CM SourceProtocolInfo", props.get("SourceProtocolInfo"), "")
        suite.check("LastChange" not in props,
                    "CM event does not pretend to be LastChange")
    renderer.unsubscribe("ConnectionManager", sid)


def test_gena_negatives(suite, renderer, callback):
    resp = renderer.subscribe("AVTransport", None)
    suite.eq("SUBSCRIBE without CALLBACK is 412", resp.status, 412)
    resp = renderer.subscribe("AVTransport", callback.url, nt="upnp:wrong")
    suite.eq("SUBSCRIBE with wrong NT is 412", resp.status, 412)
    resp = renderer.subscribe("AVTransport", "ftp://127.0.0.1/cb")
    suite.eq("SUBSCRIBE with non-http CALLBACK is 412", resp.status, 412)

    # A CALLBACK naming a hostname used to be resolved with getaddrinfo() inside
    # the single poll loop, so any unauthenticated peer could stall SSDP and the
    # control endpoint for the duration of a DNS lookup. Only literal IPv4 is
    # accepted now; the answer must be 412 and it must be immediate.
    import time as _time
    t0 = _time.monotonic()
    resp = renderer.subscribe("AVTransport", "http://not-a-real-host.invalid:1234/cb")
    elapsed = _time.monotonic() - t0
    suite.eq("SUBSCRIBE with a hostname CALLBACK is 412", resp.status, 412)
    suite.check(elapsed < 1.0,
                "hostname CALLBACK is refused without a DNS lookup",
                "took %.3fs" % elapsed)
    resp = renderer.subscribe("AVTransport", sid="uuid:does-not-exist",
                              nt=None, timeout="Second-60")
    suite.eq("renew unknown SID is 412", resp.status, 412)
    resp = renderer.unsubscribe("AVTransport", "uuid:does-not-exist")
    suite.eq("UNSUBSCRIBE unknown SID is 412", resp.status, 412)
    resp = renderer.unsubscribe("AVTransport", None)
    suite.eq("UNSUBSCRIBE without SID is 412", resp.status, 412)

    # An invalid TIMEOUT falls back to the default instead of failing.
    callback.clear()
    resp = renderer.subscribe("AVTransport", callback.url,
                              timeout="Banana")
    suite.eq("invalid TIMEOUT still subscribes", resp.status, 200)
    suite.eq("invalid TIMEOUT uses default", resp.header("timeout"),
             "Second-1800")
    if resp.header("sid"):
        renderer.unsubscribe("AVTransport", resp.header("sid"))

    # A one-second subscription expires; renewal of the stale SID is refused.
    callback.clear()
    resp = renderer.subscribe("RenderingControl", callback.url,
                              timeout="Second-1")
    suite.eq("short SUBSCRIBE status", resp.status, 200)
    sid = resp.header("sid")
    suite.eq("short TIMEOUT echoed", resp.header("timeout"), "Second-1")
    event = callback.wait(lambda e: e["headers"].get("sid") == sid, timeout=2)
    suite.check(event is not None, "short subscription initial event")
    time.sleep(1.6)
    renderer.soap("RenderingControl", "SetVolume",
                  arg("InstanceID", 0) + arg("Channel", "Master") +
                  arg("DesiredVolume", 33))
    time.sleep(0.8)
    suite.eq("expired SID receives no NOTIFY",
             len([e for e in callback.snapshot()
                  if e["headers"].get("sid") == sid]), 1)
    resp = renderer.subscribe("RenderingControl", sid=sid,
                              timeout="Second-60", nt=None)
    suite.eq("renew expired SID is 412", resp.status, 412)


def test_gena_dead_callback(suite, renderer, daemon):
    # A bound but non-listening port refuses connections immediately. Three
    # failed NOTIFYs must drop the subscriber without stalling the HTTP server.
    blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    blocker.bind(("127.0.0.1", 0))
    dead_port = blocker.getsockname()[1]
    try:
        callback_url = "http://127.0.0.1:%d/cb" % dead_port
        resp = renderer.subscribe("RenderingControl", callback_url,
                                  timeout="Second-300")
        suite.eq("dead-callback SUBSCRIBE accepted", resp.status, 200)
        sid = resp.header("sid")
        for volume in (50, 51, 52):
            resp = renderer.soap("RenderingControl", "SetVolume",
                                 arg("InstanceID", 0) +
                                 arg("Channel", "Master") +
                                 arg("DesiredVolume", volume))
            suite.eq("dead-callback SetVolume %d" % volume, resp.status, 200)
        time.sleep(0.3)
        log = daemon.log_text()
        suite.contains("failed NOTIFY logged", log, "NOTIFY failed")
        suite.contains("dead subscriber dropped", log, "dropping subscriber")
        resp = renderer.soap("RenderingControl", "GetVolume",
                             arg("InstanceID", 0) + arg("Channel", "Master"))
        suite.eq("server alive after dead callback", resp.status, 200)
        resp = renderer.unsubscribe("RenderingControl", sid)
        suite.eq("dropped SID unsubscribe is 412", resp.status, 412)
    finally:
        blocker.close()


def test_mock_backend(suite, binary, tmpdir):
    # Denied ownership maps to 701, like a real sonosd with AirPlay active.
    port = free_port()
    daemon = Daemon(binary, port, ssdp_port=1901,
                    uuid="22345678-1234-4234-8234-123456789abc",
                    backend="mock-sonosd", mock_deny=True,
                    log_path=os.path.join(tmpdir, "mock-deny.log"))
    try:
        daemon.start()
        renderer = Renderer(Client(daemon.base))
        resp = renderer.soap("AVTransport", "SetAVTransportURI",
                             arg("InstanceID", 0) +
                             arg("CurrentURI", TEST_URI))
        suite.eq("mock-deny SetURI accepted", resp.status, 200)
        resp = renderer.soap("AVTransport", "Play",
                             arg("InstanceID", 0) + arg("Speed", "1"))
        suite.eq("mock-deny Play status", resp.status, 500)
        suite.eq("mock-deny Play fault", renderer.fault_code(resp), 701)
        resp = renderer.soap("AVTransport", "GetTransportInfo",
                             arg("InstanceID", 0))
        suite.eq("mock-deny stays STOPPED",
                 renderer.value(resp, "CurrentTransportState"), "STOPPED")
    finally:
        daemon.stop()

    # The same backend, without the busy flag, plays normally.
    port = free_port()
    daemon = Daemon(binary, port, ssdp_port=1902,
                    uuid="32345678-1234-4234-8234-123456789abc",
                    backend="mock-sonosd",
                    log_path=os.path.join(tmpdir, "mock-ok.log"))
    try:
        daemon.start()
        renderer = Renderer(Client(daemon.base))
        renderer.soap("AVTransport", "SetAVTransportURI",
                      arg("InstanceID", 0) + arg("CurrentURI", TEST_URI))
        resp = renderer.soap("AVTransport", "Play",
                             arg("InstanceID", 0) + arg("Speed", "1"))
        suite.eq("mock sonosd Play status", resp.status, 200)
        resp = renderer.soap("AVTransport", "GetTransportInfo",
                             arg("InstanceID", 0))
        suite.eq("mock sonosd PLAYING",
                 renderer.value(resp, "CurrentTransportState"), "PLAYING")
    finally:
        daemon.stop()


def test_ssdp(suite, listener, client, ssdp_mode_holder):
    expected_all = {
        "upnp:rootdevice",
        "uuid:" + TEST_UUID,
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        SERVICE_TYPES["AVTransport"],
        SERVICE_TYPES["RenderingControl"],
        SERVICE_TYPES["ConnectionManager"],
    }

    mode = "multicast"
    responses = ssdp_search("urn:schemas-upnp-org:device:MediaRenderer:1",
                            mx=1, mode="multicast")
    if not responses:
        mode = "unicast-fallback"
        responses = ssdp_search(
            "urn:schemas-upnp-org:device:MediaRenderer:1", mx=1,
            mode="unicast")
    ssdp_mode_holder.append(mode)

    suite.check(bool(responses), "M-SEARCH receives a response (%s)" % mode,
                "no SSDP response on multicast or unicast")
    if responses:
        resp = responses[0]
        headers = resp["headers"]
        suite.eq("M-SEARCH status line", resp["status"], "HTTP/1.1 200 OK")
        suite.eq("M-SEARCH CACHE-CONTROL",
                 headers.get("cache-control"), "max-age=%d" % CACHE_SECONDS)
        suite.check("ext" in headers, "M-SEARCH EXT header present")
        suite.eq("M-SEARCH LOCATION", headers.get("location"),
                 "http://127.0.0.1:%d/description.xml"
                 % int(client.base.rsplit(":", 1)[1]))
        suite.eq("M-SEARCH ST", headers.get("st"),
                 "urn:schemas-upnp-org:device:MediaRenderer:1")
        suite.eq("M-SEARCH USN", headers.get("usn"),
                 "uuid:%s::urn:schemas-upnp-org:device:MediaRenderer:1"
                 % TEST_UUID)
        suite.contains("M-SEARCH SERVER", headers.get("server", ""), "Moggie")
        suite.check(headers.get("date", "").endswith("GMT"),
                    "M-SEARCH DATE is GMT")
        suite.check(headers.get("bootid.upnp.org", "").isdigit(),
                    "M-SEARCH BOOTID present")

    responses = ssdp_search("ssdp:all", mx=1, mode=mode)
    suite.eq("ssdp:all returns six targets", len(responses), 6)
    sts = {r["headers"].get("st") for r in responses}
    suite.eq("ssdp:all ST set", sts, expected_all)

    responses = ssdp_search("urn:schemas-upnp-org:device:MediaServer:1",
                            mx=1, mode=mode)
    suite.eq("foreign ST gets no response", len(responses), 0)

    # MAN must be exactly "ssdp:discover". This used to match by substring, so
    # a malformed search was answered as though it were well formed.
    for bad in ('"xssdp:discovery"', '"ssdp:discover-ish"', "ssdp:discoverX"):
        responses = ssdp_search("ssdp:all", mx=1, mode=mode, man=bad,
                                wait_extra=0.3)
        suite.eq("malformed MAN %s ignored" % bad, len(responses), 0)

    if listener.ok:
        alive = [e for e in listener.snapshot()
                 if e["nts"] == "ssdp:alive" and e["nt"] == "upnp:rootdevice"]
        suite.check(len(alive) >= 2,
                    "SSDP alive is re-advertised before max-age",
                    "saw %d rootdevice alive NOTIFYs" % len(alive))
        if len(alive) >= 2:
            gap = alive[1]["at"] - alive[0]["at"]
            suite.check(1.0 <= gap < CACHE_SECONDS,
                        "alive re-advertisement interval", "gap=%.2fs" % gap)
        if alive:
            suite.eq("alive LOCATION", alive[0]["location"],
                     "http://127.0.0.1:%d/description.xml"
                     % int(client.base.rsplit(":", 1)[1]))
            suite.eq("alive CACHE-CONTROL", alive[0]["cache"],
                     "max-age=%d" % CACHE_SECONDS)
            suite.check(alive[0]["usn"].startswith("uuid:"),
                        "alive USN is a uuid")
    else:
        suite.skip("SSDP alive re-advertisement", "cannot join multicast group")


def test_byebye(suite, listener):
    if not listener.ok:
        suite.skip("ssdp:byebye on shutdown", "cannot join multicast group")
        return
    deadline = time.monotonic() + 2.0
    byes = []
    while time.monotonic() < deadline:
        byes = [e for e in listener.snapshot() if e["nts"] == "ssdp:byebye"]
        if len(byes) >= 6:
            break
        time.sleep(0.05)
    suite.check(len(byes) >= 6, "ssdp:byebye sent for all targets",
                "saw %d" % len(byes))
    nts = {e["nt"] for e in byes}
    suite.check("urn:schemas-upnp-org:device:MediaRenderer:1" in nts,
                "byebye includes the device type")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="firmware/dlna/build/host/dlna-renderer")
    parser.add_argument("--keep-logs", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.isfile(binary):
        print("error: binary not found: %s" % binary, file=sys.stderr)
        print("build it with: make -C firmware/dlna", file=sys.stderr)
        return 2

    suite = Suite()
    tmpdir = tempfile.mkdtemp(prefix="moggie-dlna-test-")
    listener = SSDPListener()
    daemon = None
    callback = None
    ssdp_mode = []

    if not listener.ok:
        print("# note: could not bind/join SSDP listener on port %d; "
              "multicast checks will be skipped" % SSDP_PORT)

    try:
        http_port = free_port()
        daemon = Daemon(binary, http_port, uuid=TEST_UUID,
                        log_path=os.path.join(tmpdir, "daemon.log"))
        daemon.start()
        client = Client(daemon.base)
        renderer = Renderer(client)
        callback = Callback()

        test_ssdp(suite, listener, client, ssdp_mode)
        test_cli(suite, binary)
        test_description(suite, client)
        test_scpds(suite, client)
        test_http_edges(suite, client, http_port)
        test_connection_manager(suite, renderer, client)
        test_avtransport(suite, renderer)
        test_rendering_control(suite, renderer)
        test_soap_errors(suite, renderer)
        test_gena_avt(suite, renderer, callback)
        test_gena_rcs(suite, renderer, callback)
        test_gena_cm(suite, renderer, callback)
        test_gena_negatives(suite, renderer, callback)
        test_gena_dead_callback(suite, renderer, daemon)
        test_mock_backend(suite, binary, tmpdir)

        mem = daemon.rss_kb() or {}
        rss = mem.get("VmRSS")
        hwm = mem.get("VmHWM")
        suite.check(rss is not None and rss < 8192,
                    "resident memory while serving is under 8 MiB",
                    "VmRSS=%r kB" % (rss,))
        print("# measured RSS %s kB, peak %s kB (host build)" % (rss, hwm))

        rc = daemon.stop()
        suite.eq("daemon exits 0 on SIGTERM", rc, 0)
        test_byebye(suite, listener)
    except Exception:
        traceback.print_exc()
        suite.check(False, "test harness completed without an exception")
    finally:
        if daemon is not None:
            daemon.stop()
        if callback is not None:
            callback.stop()
        listener.stop()

    ok = suite.summary()
    if not ok:
        if daemon is not None:
            print("\n--- daemon log tail ---")
            print(daemon.log_tail())
        print("\nartifacts: %s" % tmpdir)
    elif not args.keep_logs:
        for name in ("daemon.log", "mock-deny.log", "mock-ok.log"):
            try:
                os.unlink(os.path.join(tmpdir, name))
            except OSError:
                pass
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass
    if ssdp_mode:
        print("# SSDP transport used: %s" % ssdp_mode[0])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
