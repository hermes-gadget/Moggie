#!/usr/bin/env python3
"""Publish Moggie's Home Assistant discovery documents to an MQTT broker.

Implements just enough MQTT 3.1.1 (CONNECT / PUBLISH qos 1 / SUBSCRIBE) to do
the job with the standard library, so the test rig needs nothing but python3.

Examples::

    # generate + publish to the docker rig
    python3 tools/ha/publish.py --host 127.0.0.1 --port 18830 --seed-state

    # publish the checked-in payloads to a broker with auth
    python3 tools/ha/publish.py --host broker.lan --username moggie \\
        --password "$MOGGIE_MQTT_PASSWORD"

    # publish for a second speaker and prove the broker kept it
    python3 tools/ha/publish.py --device-id kitchen --seed-state --verify

Stdlib only, Python 3.8+.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_discovery  # noqa: E402  (sibling module, not a package)

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 18830
DEFAULT_TIMEOUT = 10.0

CONNECT = 1
CONNACK = 2
PUBLISH = 3
PUBACK = 4
SUBSCRIBE = 8
SUBACK = 9
PINGREQ = 12
PINGRESP = 13
DISCONNECT = 14


class MqttError(RuntimeError):
    """Raised when the broker refuses or the wire protocol goes sideways."""


def encode_remaining_length(length: int) -> bytes:
    if length < 0 or length > 268_435_455:
        raise MqttError(f"remaining length out of range: {length}")
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length:
            byte |= 0x80
        out.append(byte)
        if not length:
            return bytes(out)


def encode_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > 65535:
        raise MqttError("MQTT string longer than 65535 bytes")
    return struct.pack("!H", len(encoded)) + encoded


class MqttClient:
    """A very small, blocking MQTT 3.1.1 client."""

    def __init__(
        self,
        host: str = DEFAULT_HOST,
        port: int = DEFAULT_PORT,
        *,
        timeout: float = DEFAULT_TIMEOUT,
        client_id: str = "",
    ) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.client_id = client_id or f"moggie-ha-{os.getpid()}-{int(time.time())}"
        self._sock: Optional[socket.socket] = None
        self._packet_id = 0

    # -- wire helpers ---------------------------------------------------------

    def _next_packet_id(self) -> int:
        self._packet_id = self._packet_id % 65535 + 1
        return self._packet_id

    def _recv_exact(self, count: int) -> bytes:
        assert self._sock is not None
        chunks = bytearray()
        while len(chunks) < count:
            chunk = self._sock.recv(count - len(chunks))
            if not chunk:
                raise MqttError("broker closed the connection")
            chunks.extend(chunk)
        return bytes(chunks)

    def _send(self, packet: bytes) -> None:
        assert self._sock is not None
        self._sock.sendall(packet)

    def _read_packet(self) -> Tuple[int, int, bytes]:
        header = self._recv_exact(1)[0]
        multiplier = 1
        length = 0
        for _ in range(4):
            byte = self._recv_exact(1)[0]
            length += (byte & 0x7F) * multiplier
            if not byte & 0x80:
                break
            multiplier *= 128
        else:
            raise MqttError("malformed remaining length")
        body = self._recv_exact(length) if length else b""
        return header >> 4, header & 0x0F, body

    def _packet(self, packet_type: int, flags: int, body: bytes) -> bytes:
        return bytes([(packet_type << 4) | flags]) + encode_remaining_length(len(body)) + body

    # -- protocol -------------------------------------------------------------

    def connect(self, username: Optional[str] = None, password: Optional[str] = None) -> None:
        flags = 0x02  # clean session
        payload = encode_string(self.client_id)
        if username is not None:
            flags |= 0x80
            payload += encode_string(username)
        if password is not None:
            flags |= 0x40
            payload += encode_string(password)

        body = encode_string("MQTT") + bytes([0x04, flags]) + struct.pack("!H", 60) + payload
        try:
            self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as error:
            raise MqttError(f"cannot reach {self.host}:{self.port}: {error}") from error
        self._sock.settimeout(self.timeout)
        self._send(self._packet(CONNECT, 0, body))

        packet_type, _, ack = self._read_packet()
        if packet_type != CONNACK or len(ack) < 2:
            raise MqttError("broker did not send CONNACK")
        if ack[1] != 0:
            reasons = {
                1: "unacceptable protocol version",
                2: "client identifier rejected",
                3: "server unavailable",
                4: "bad username or password",
                5: "not authorised",
            }
            raise MqttError(f"broker refused connection: {reasons.get(ack[1], ack[1])}")

    def publish(
        self, topic: str, payload: str | bytes, *, qos: int = 1, retain: bool = True
    ) -> None:
        if not topic or any(char in topic for char in ("+", "#")):
            raise MqttError(f"refusing to publish to invalid topic: {topic!r}")
        data = payload.encode("utf-8") if isinstance(payload, str) else payload
        flags = (qos << 1) | (1 if retain else 0)
        body = encode_string(topic)
        packet_id = None
        if qos:
            packet_id = self._next_packet_id()
            body += struct.pack("!H", packet_id)
        body += data
        self._send(self._packet(PUBLISH, flags, body))
        if packet_id is None:
            return
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            packet_type, _, ack = self._read_packet()
            if packet_type == PUBACK and len(ack) >= 2 and struct.unpack("!H", ack[:2])[0] == packet_id:
                return
            if packet_type == PINGRESP:
                continue
        raise MqttError(f"no PUBACK for {topic}")

    def subscribe(self, topics: Sequence[str], qos: int = 1) -> None:
        body = struct.pack("!H", self._next_packet_id())
        for topic in topics:
            body += encode_string(topic) + bytes([qos])
        self._send(self._packet(SUBSCRIBE, 0x02, body))
        while True:
            packet_type, _, _ = self._read_packet()
            if packet_type == SUBACK:
                return

    def read_publish(self) -> Tuple[str, bytes, bool]:
        while True:
            packet_type, flags, body = self._read_packet()
            if packet_type != PUBLISH:
                continue
            topic_length = struct.unpack("!H", body[:2])[0]
            topic = body[2 : 2 + topic_length].decode("utf-8", "replace")
            cursor = 2 + topic_length
            qos = (flags >> 1) & 0x03
            if qos:
                packet_id = struct.unpack("!H", body[cursor : cursor + 2])[0]
                cursor += 2
                self._send(self._packet(PUBACK, 0, struct.pack("!H", packet_id)))
            return topic, body[cursor:], bool(flags & 0x01)

    def disconnect(self) -> None:
        if self._sock is None:
            return
        try:
            self._send(self._packet(DISCONNECT, 0, b""))
        except OSError:
            pass
        finally:
            self._sock.close()
            self._sock = None


# --- payload loading ----------------------------------------------------------


def load_from_dir(path: Path) -> List[Dict[str, Any]]:
    envelopes = []
    for file in sorted(path.glob("*.json")):
        if file.name.startswith("_"):
            continue
        data = json.loads(file.read_text(encoding="utf-8"))
        envelopes.append(
            {
                "topic": data["topic"],
                "payload": data["payload"],
                "qos": data.get("qos", 1),
                "retain": data.get("retain", True),
            }
        )
    return envelopes


def build_in_memory(args: argparse.Namespace) -> List[Dict[str, Any]]:
    entities = gen_discovery.build_entities(
        device_id=args.device_id or gen_discovery.DEFAULT_DEVICE_ID,
        base_topic=args.base_topic or gen_discovery.DEFAULT_BASE_TOPIC,
        firmware_version=args.firmware or gen_discovery.DEFAULT_FIRMWARE_VERSION,
        discovery_prefix=args.discovery_prefix or gen_discovery.DEFAULT_DISCOVERY_PREFIX,
        entity_prefix=args.entity_prefix or gen_discovery.DEFAULT_ENTITY_PREFIX,
        include_media_player_shim=not args.no_media_player_shim,
    )
    return [
        {
            "topic": entity.topic,
            "payload": entity.payload,
            "qos": 1,
            "retain": True,
        }
        for entity in entities
    ]


def serialise(envelope: Dict[str, Any]) -> str:
    payload = envelope["payload"]
    if isinstance(payload, (dict, list)):
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
    return str(payload)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Publish Moggie MQTT discovery payloads to a broker.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--username", default=None)
    parser.add_argument(
        "--password",
        default=None,
        help="broker password; prefer MOGGIE_MQTT_PASSWORD to keep it out of shell history",
    )
    parser.add_argument("--client-id", default="", help="MQTT client id (default: generated)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument(
        "--dir",
        default=None,
        help="payload directory; default tools/ha/payloads, generated in memory if absent",
    )
    parser.add_argument("--device-id", default=None, help="force generation for this device id")
    parser.add_argument("--base-topic", default=None, help="force generation under this topic root")
    parser.add_argument("--discovery-prefix", default=None)
    parser.add_argument("--entity-prefix", default=None, help="entity id prefix for a second speaker")
    parser.add_argument("--firmware", default=None, help="override sw_version in generated payloads")
    parser.add_argument("--no-media-player-shim", action="store_true")
    parser.add_argument(
        "--seed-state",
        action="store_true",
        help="also publish retained state + availability so entities show values",
    )
    parser.add_argument("--no-retain", action="store_true", help="publish without the retain flag")
    parser.add_argument("--dry-run", action="store_true", help="print topics without connecting")
    parser.add_argument(
        "--verify",
        action="store_true",
        help="subscribe back to every published topic and compare (proves delivery)",
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    if args.password is None:
        args.password = os.environ.get("MOGGIE_MQTT_PASSWORD")
    if args.username is None and args.password is not None:
        args.username = os.environ.get("MOGGIE_MQTT_USER", "moggie")

    payload_dir = Path(args.dir) if args.dir else Path(__file__).resolve().parent / "payloads"
    explicit_model = any(
        value is not None
        for value in (
            args.device_id,
            args.base_topic,
            args.discovery_prefix,
            args.entity_prefix,
            args.firmware,
        )
    )
    if payload_dir.is_dir() and not explicit_model:
        envelopes = load_from_dir(payload_dir)
        source = str(payload_dir)
    else:
        envelopes = build_in_memory(args)
        source = "generated in memory"

    if not envelopes:
        print(f"no discovery payloads found in {payload_dir}", file=sys.stderr)
        return 2
    if args.no_retain:
        for envelope in envelopes:
            envelope["retain"] = False

    device_id = args.device_id or gen_discovery.DEFAULT_DEVICE_ID
    base_topic = args.base_topic or gen_discovery.DEFAULT_BASE_TOPIC
    firmware = args.firmware or gen_discovery.DEFAULT_FIRMWARE_VERSION

    messages: List[Dict[str, Any]] = list(envelopes)
    if args.seed_state:
        for message in gen_discovery.state_messages(
            device_id=device_id, base_topic=base_topic, firmware_version=firmware
        ):
            messages.append({"topic": message["topic"], "payload": message["payload"],
                             "qos": 1, "retain": message.get("retain", True)})

    if args.dry_run:
        for message in messages:
            print(f"{message['topic']}  {serialise(message)}")
        print(f"\n{len(envelopes)} discovery documents + {len(messages) - len(envelopes)} "
              f"state messages from {source} (dry run)")
        return 0

    client = MqttClient(args.host, args.port, timeout=args.timeout, client_id=args.client_id)
    try:
        client.connect(args.username, args.password)
        for message in messages:
            client.publish(
                message["topic"],
                serialise(message),
                qos=int(message.get("qos", 1)),
                retain=bool(message.get("retain", True)),
            )
        if args.verify:
            expected = {m["topic"]: serialise(m) for m in messages}
            client.subscribe(sorted(expected), qos=1)
            seen: Dict[str, str] = {}
            deadline = time.monotonic() + min(args.timeout, 5.0)
            while len(seen) < len(expected) and time.monotonic() < deadline:
                try:
                    topic, payload, _retained = client.read_publish()
                except (socket.timeout, TimeoutError):
                    break
                if topic in expected:
                    seen[topic] = payload.decode("utf-8", "replace")
            missing = sorted(set(expected) - set(seen))
            mismatched = sorted(t for t, v in seen.items() if v != expected[t])
            if missing or mismatched:
                print(f"verification failed: {len(missing)} missing, {len(mismatched)} mismatched",
                      file=sys.stderr)
                for topic in missing[:10]:
                    print(f"  missing: {topic}", file=sys.stderr)
                for topic in mismatched[:10]:
                    print(f"  mismatch: {topic}", file=sys.stderr)
                return 1
            if not args.quiet:
                print(f"verified {len(seen)} retained topics on {args.host}:{args.port}")
    finally:
        client.disconnect()

    if not args.quiet:
        print(
            f"published {len(envelopes)} discovery documents + "
            f"{len(messages) - len(envelopes)} state messages to "
            f"{args.host}:{args.port} from {source}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
