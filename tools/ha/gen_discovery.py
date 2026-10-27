#!/usr/bin/env python3
"""Generate Home Assistant MQTT discovery payloads for Moggie.

Emits one retained discovery document per entity for the topic layout documented
in ``docs/HA.md``:

    homeassistant/<component>/moggie_<device_id>/<object_id>/config

The payloads are derived entirely from the arguments below, so the same script
generates the rig fixtures, the firmware's retained announcements and the
checked-in reference under ``tools/ha/payloads/``.

Usage::

    python3 tools/ha/gen_discovery.py                     # write tools/ha/payloads/
    python3 tools/ha/gen_discovery.py --json              # same, on stdout
    python3 tools/ha/gen_discovery.py --device-id kitchen --base-topic moggie \\
        --firmware 0.2.0 --out /tmp/moggie-payloads

Stdlib only, Python 3.8+.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence

# --- defaults -----------------------------------------------------------------

DEFAULT_DEVICE_ID = "play5"
DEFAULT_BASE_TOPIC = "moggie"
DEFAULT_DISCOVERY_PREFIX = "homeassistant"
DEFAULT_FIRMWARE_VERSION = "0.1.0"
DEFAULT_HARDWARE_VERSION = "Play:5 Gen 1"
DEFAULT_DEVICE_NAME = "Moggie"
DEFAULT_SUGGESTED_AREA = "Living Room"
DEFAULT_ENTITY_PREFIX = "moggie"

MANUFACTURER = "Sonos"
MODEL = "Play:5 (Gen 1)"
ORIGIN_NAME = "Moggie"
SUPPORT_URL = "https://github.com/hermes-gadget/Moggie"

#: Sources the arbiter can hand the DAC to. Also the media_player source_list.
SOURCES: Sequence[str] = ("AirPlay", "Spotify", "DLNA", "Snapcast")

#: Entities the HA control plane is specified to contain.  The media player is
#: the headline entity; everything else is a first-class MQTT component.
REQUIRED_ENTITY_IDS: Sequence[str] = (
    "media_player.moggie_play5",
    "number.moggie_bass",
    "number.moggie_treble",
    "number.moggie_balance",
    "switch.moggie_loudness",
    "switch.moggie_led",
    "switch.moggie_night_mode",
    "sensor.moggie_temperature",
    "sensor.moggie_uptime",
    "sensor.moggie_buffer",
    "button.moggie_identify",
    "button.moggie_reboot",
    "update.moggie_firmware",
)

#: Helper entities that let a *stock* Home Assistant build a real
#: ``media_player.moggie_play5``.  See docs/HA.md — HA's MQTT integration has no
#: media_player platform, so ``tools/ha/ha-config/packages/moggie_media_player.yaml``
#: wraps these three in a universal media player.
SHIM_ENTITY_IDS: Sequence[str] = (
    "number.moggie_volume",
    "select.moggie_source",
    "sensor.moggie_playback_state",
)


# --- model --------------------------------------------------------------------


@dataclass(frozen=True)
class Entity:
    """One discovery document plus the metadata the publisher and tests need."""

    component: str
    object_id: str
    topic: str
    payload: Dict[str, Any]
    required: bool = True

    @property
    def entity_id(self) -> str:
        return str(self.payload["default_entity_id"])

    @property
    def unique_id(self) -> str:
        return str(self.payload["unique_id"])

    @property
    def filename(self) -> str:
        return f"{self.component}.{self.object_id}.json"

    def envelope(self) -> Dict[str, Any]:
        """The on-disk / on-the-wire representation for one entity."""
        return {"topic": self.topic, "qos": 1, "retain": True, "payload": self.payload}


@dataclass(frozen=True)
class Layout:
    """Topic and identity naming derived from the CLI arguments."""

    device_id: str
    base_topic: str
    discovery_prefix: str
    firmware_version: str
    hardware_version: str
    device_name: str
    suggested_area: str
    entity_prefix: str = "moggie"

    @property
    def node_id(self) -> str:
        """Discovery node id: ``moggie_<device_id>``."""
        return f"moggie_{self.device_id}"

    def entity_id(self, component: str, object_id: str) -> str:
        """Entity id for a component.

        The default prefix reproduces the documented control plane exactly:
        ``media_player.moggie_play5``, ``number.moggie_bass``.  A second speaker
        needs its own prefix (``--entity-prefix moggie_kitchen``) or its entities
        would fight the first one for the same ids.
        """
        if component == "media_player":
            return f"media_player.{self.entity_prefix}_{self.device_id}"
        return f"{component}.{self.entity_prefix}_{object_id}"

    @property
    def root(self) -> str:
        """State/command topic root: ``<base_topic>/<device_id>``."""
        return f"{self.base_topic}/{self.device_id}"

    def topic(self, suffix: str) -> str:
        return f"{self.root}/{suffix}" if suffix else self.root

    @property
    def availability_topic(self) -> str:
        return self.topic("availability")

    @property
    def player_state_topic(self) -> str:
        """Single JSON document describing playback, volume and source."""
        return self.topic("player")

    def discovery_topic(self, component: str, object_id: str) -> str:
        return f"{self.discovery_prefix}/{component}/{self.node_id}/{object_id}/config"

    def device(self) -> Dict[str, Any]:
        return {
            "identifiers": [self.node_id],
            "name": self.device_name,
            "manufacturer": MANUFACTURER,
            "model": MODEL,
            "sw_version": self.firmware_version,
            "hw_version": self.hardware_version,
            "suggested_area": self.suggested_area,
        }

    def origin(self) -> Dict[str, Any]:
        return {
            "name": ORIGIN_NAME,
            "sw_version": self.firmware_version,
            "support_url": SUPPORT_URL,
        }


def _base_payload(
    layout: Layout,
    component: str,
    object_id: str,
    name: str,
    *,
    entity_category: Optional[str] = None,
    icon: Optional[str] = None,
) -> Dict[str, Any]:
    """Keys every Moggie entity carries: identity, availability, device, origin."""
    payload: Dict[str, Any] = {
        "name": name,
        "unique_id": f"{layout.node_id}_{component}_{object_id}",
        "default_entity_id": layout.entity_id(component, object_id),
    }
    if entity_category:
        payload["entity_category"] = entity_category
    if icon:
        payload["icon"] = icon

    payload["availability_topic"] = layout.availability_topic
    payload["payload_available"] = "online"
    payload["payload_not_available"] = "offline"
    payload["device"] = layout.device()
    payload["origin"] = layout.origin()
    return payload


def _add(
    layout: Layout,
    component: str,
    object_id: str,
    name: str,
    *,
    required: bool = True,
    entity_category: Optional[str] = None,
    icon: Optional[str] = None,
    **extra: Any,
) -> Entity:
    payload = _base_payload(
        layout, component, object_id, name, entity_category=entity_category, icon=icon
    )
    payload.update(extra)
    return Entity(
        component=component,
        object_id=object_id,
        topic=layout.discovery_topic(component, object_id),
        payload=payload,
        required=required,
    )


# --- entity definitions -------------------------------------------------------


def media_player_entity(layout: Layout) -> Entity:
    """The speaker itself.

    Home Assistant's MQTT integration does not implement a media_player platform
    (verified against ``SUPPORTED_COMPONENTS`` in homeassistant/components/mqtt/
    const.py), so this document is retained for other MQTT discovery consumers
    and for the day HA grows the platform.  ``tools/ha/ha-config/packages/
    moggie_media_player.yaml`` builds a working HA media player on top of the
    same topics via the universal platform.
    """
    return _add(
        layout,
        "media_player",
        "speaker",
        "Speaker",
        device_class="speaker",
        state_topic=layout.player_state_topic,
        value_template="{{ value_json.state }}",
        volume_level_template="{{ value_json.volume_level }}",
        source_template="{{ value_json.source }}",
        source_list=list(SOURCES),
        command_topic=layout.topic("player/command"),
        payload_on="turn_on",
        payload_off="turn_off",
        volume_set_topic=layout.topic("player/volume/set"),
        source_topic=layout.topic("player/source/set"),
    )


def number_entities(layout: Layout) -> Iterable[Entity]:
    for object_id, name, icon in (
        ("bass", "Bass", "mdi:music-note"),
        ("treble", "Treble", "mdi:music-note-eighth"),
        ("balance", "Balance", "mdi:balance"),
    ):
        yield _add(
            layout,
            "number",
            object_id,
            name,
            entity_category="config",
            icon=icon,
            state_topic=layout.topic(f"eq/{object_id}"),
            command_topic=layout.topic(f"eq/{object_id}/set"),
            min=-10,
            max=10,
            step=1,
            mode="slider",
        )


def switch_entities(layout: Layout) -> Iterable[Entity]:
    for object_id, name, icon in (
        ("loudness", "Loudness", "mdi:surround-sound"),
        ("led", "LED", "mdi:led-on"),
        ("night_mode", "Night mode", "mdi:weather-night"),
    ):
        yield _add(
            layout,
            "switch",
            object_id,
            name,
            entity_category="config",
            icon=icon,
            state_topic=layout.topic(object_id),
            command_topic=layout.topic(f"{object_id}/set"),
            payload_on="ON",
            payload_off="OFF",
            state_on="ON",
            state_off="OFF",
        )


def sensor_entities(layout: Layout) -> Iterable[Entity]:
    yield _add(
        layout,
        "sensor",
        "temperature",
        "Temperature",
        entity_category="diagnostic",
        icon="mdi:thermometer",
        state_topic=layout.topic("temperature"),
        device_class="temperature",
        unit_of_measurement="°C",
        state_class="measurement",
        suggested_display_precision=1,
    )
    yield _add(
        layout,
        "sensor",
        "uptime",
        "Uptime",
        entity_category="diagnostic",
        icon="mdi:clock-outline",
        state_topic=layout.topic("uptime"),
        device_class="duration",
        unit_of_measurement="s",
        state_class="total_increasing",
    )
    yield _add(
        layout,
        "sensor",
        "buffer",
        "Buffer",
        entity_category="diagnostic",
        icon="mdi:memory",
        state_topic=layout.topic("buffer"),
        unit_of_measurement="%",
        state_class="measurement",
    )


def button_entities(layout: Layout) -> Iterable[Entity]:
    yield _add(
        layout,
        "button",
        "identify",
        "Identify",
        icon="mdi:bullhorn",
        device_class="identify",
        command_topic=layout.topic("identify"),
        payload_press="PRESS",
    )
    yield _add(
        layout,
        "button",
        "reboot",
        "Reboot",
        icon="mdi:restart",
        device_class="restart",
        command_topic=layout.topic("reboot"),
        payload_press="PRESS",
    )


def update_entity(layout: Layout) -> Entity:
    """Firmware update entity.

    Home Assistant reads the *installed* version from the state payload and the
    latest available version from the same JSON document, so one retained
    message drives the whole entity — including OTA progress via
    ``in_progress``/``update_percentage``.  ``state_topic`` must therefore stay
    template-free: a ``value_template`` would collapse the document to a string
    and hide ``latest_version``.
    """
    return _add(
        layout,
        "update",
        "firmware",
        "Firmware",
        icon="mdi:package-up",
        device_class="firmware",
        state_topic=layout.topic("update"),
        command_topic=layout.topic("update/install"),
        payload_install="install",
        title="Moggie firmware",
        release_summary="Signed OTA image for the Play:5 (Gen 1).",
    )


def shim_entities(layout: Layout) -> Iterable[Entity]:
    """Backing entities for the universal media player (see docs/HA.md)."""
    yield _add(
        layout,
        "number",
        "volume",
        "Volume",
        required=False,
        icon="mdi:volume-high",
        state_topic=layout.player_state_topic,
        value_template="{{ value_json.volume_level }}",
        command_topic=layout.topic("player/volume/set"),
        min=0,
        max=1,
        step=0.01,
        mode="slider",
    )
    yield _add(
        layout,
        "select",
        "source",
        "Source",
        required=False,
        icon="mdi:import",
        state_topic=layout.player_state_topic,
        value_template="{{ value_json.source }}",
        command_topic=layout.topic("player/source/set"),
        options=list(SOURCES),
    )
    yield _add(
        layout,
        "sensor",
        "playback_state",
        "Playback state",
        required=False,
        entity_category="diagnostic",
        icon="mdi:play-circle-outline",
        state_topic=layout.player_state_topic,
        value_template="{{ value_json.state }}",
    )


# --- assembly -----------------------------------------------------------------


def build_entities(
    device_id: str = DEFAULT_DEVICE_ID,
    base_topic: str = DEFAULT_BASE_TOPIC,
    firmware_version: str = DEFAULT_FIRMWARE_VERSION,
    *,
    hardware_version: str = DEFAULT_HARDWARE_VERSION,
    device_name: str = DEFAULT_DEVICE_NAME,
    suggested_area: str = DEFAULT_SUGGESTED_AREA,
    discovery_prefix: str = DEFAULT_DISCOVERY_PREFIX,
    entity_prefix: str = DEFAULT_ENTITY_PREFIX,
    include_media_player_shim: bool = True,
) -> List[Entity]:
    """Build every discovery document for one speaker, in a stable order."""
    layout = Layout(
        device_id=device_id,
        base_topic=base_topic,
        discovery_prefix=discovery_prefix,
        firmware_version=firmware_version,
        hardware_version=hardware_version,
        device_name=device_name,
        suggested_area=suggested_area,
        entity_prefix=entity_prefix,
    )

    entities: List[Entity] = [media_player_entity(layout)]
    entities.extend(number_entities(layout))
    entities.extend(switch_entities(layout))
    entities.extend(sensor_entities(layout))
    entities.extend(button_entities(layout))
    entities.append(update_entity(layout))
    if include_media_player_shim:
        entities.extend(shim_entities(layout))
    return entities


def state_messages(
    device_id: str = DEFAULT_DEVICE_ID,
    base_topic: str = DEFAULT_BASE_TOPIC,
    firmware_version: str = DEFAULT_FIRMWARE_VERSION,
    *,
    entity_prefix: str = DEFAULT_ENTITY_PREFIX,
    latest_version: Optional[str] = None,
) -> List[Dict[str, Any]]:
    """Retained state/LWT messages that make the entities look like a live speaker.

    ``publish.py --seed-state`` uses this so a fresh HA install shows real values
    instead of a wall of ``unknown``.  The device itself replaces these the first
    time it announces itself.
    """
    layout = Layout(
        device_id=device_id,
        base_topic=base_topic,
        discovery_prefix=DEFAULT_DISCOVERY_PREFIX,
        firmware_version=firmware_version,
        hardware_version=DEFAULT_HARDWARE_VERSION,
        device_name=DEFAULT_DEVICE_NAME,
        suggested_area=DEFAULT_SUGGESTED_AREA,
        entity_prefix=entity_prefix,
    )
    player = {"state": "paused", "volume_level": 0.35, "source": SOURCES[0]}
    messages: List[Dict[str, Any]] = [
        {"topic": layout.availability_topic, "payload": "online", "retain": True},
        {
            "topic": layout.player_state_topic,
            "payload": json.dumps(player, separators=(",", ":")),
            "retain": True,
        },
    ]
    for name, value in (
        ("bass", 0),
        ("treble", 0),
        ("balance", 0),
        ("loudness", "OFF"),
        ("led", "ON"),
        ("night_mode", "OFF"),
        ("temperature", 31.5),
        ("uptime", 4210),
        ("buffer", 62),
    ):
        topic = layout.topic(f"eq/{name}" if name in ("bass", "treble", "balance") else name)
        messages.append({"topic": topic, "payload": str(value), "retain": True})

    messages.append(
        {
            "topic": layout.topic("update"),
            "payload": json.dumps(
                {
                    "installed_version": firmware_version,
                    "latest_version": latest_version or firmware_version,
                    "in_progress": False,
                    "update_percentage": None,
                },
                separators=(",", ":"),
            ),
            "retain": True,
        }
    )
    return messages


# --- emission -----------------------------------------------------------------


def manifest(
    entities: Sequence[Entity],
    *,
    device_id: str,
    base_topic: str,
    firmware_version: str,
    discovery_prefix: str,
    entity_prefix: str = DEFAULT_ENTITY_PREFIX,
) -> Dict[str, Any]:
    return {
        "generator": "tools/ha/gen_discovery.py",
        "discovery_prefix": discovery_prefix,
        "node_id": f"moggie_{device_id}",
        "device_id": device_id,
        "base_topic": base_topic,
        "entity_prefix": entity_prefix,
        "firmware_version": firmware_version,
        "entity_count": len(entities),
        "required_entity_count": sum(1 for entity in entities if entity.required),
        "entities": [
            {
                "entity_id": entity.entity_id,
                "component": entity.component,
                "object_id": entity.object_id,
                "unique_id": entity.unique_id,
                "topic": entity.topic,
                "file": entity.filename,
                "required": entity.required,
            }
            for entity in entities
        ],
    }


def write_payloads(entities: Sequence[Entity], out_dir: Path, meta: Dict[str, Any]) -> List[Path]:
    """Write one JSON file per entity plus ``_manifest.json``.  Overwrites only."""
    out_dir.mkdir(parents=True, exist_ok=True)
    written: List[Path] = []
    for entity in entities:
        path = out_dir / entity.filename
        path.write_text(_dump(entity.envelope()), encoding="utf-8")
        written.append(path)
    manifest_path = out_dir / "_manifest.json"
    manifest_path.write_text(_dump(meta), encoding="utf-8")
    written.append(manifest_path)
    return written


def _dump(value: Any) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def _topic_ok(value: str) -> bool:
    """True for a publishable MQTT topic: no wildcards, no empty levels."""
    if not isinstance(value, str) or not value or value.startswith("/") or value.endswith("/"):
        return False
    if any(char in value for char in ("+", "#", "\0")):
        return False
    return all(part for part in value.split("/"))


def _slug_ok(value: str) -> bool:
    return bool(value) and all(char.islower() or char.isdigit() or char == "_" for char in value)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate Home Assistant MQTT discovery payloads for a Moggie speaker.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--device-id", default=DEFAULT_DEVICE_ID,
                        help="short device id; entity ids and the node id derive from it")
    parser.add_argument("--base-topic", default=DEFAULT_BASE_TOPIC,
                        help="root of the state/command topic tree")
    parser.add_argument("--discovery-prefix", default=DEFAULT_DISCOVERY_PREFIX,
                        help="MQTT discovery prefix configured in Home Assistant")
    parser.add_argument("--firmware", default=DEFAULT_FIRMWARE_VERSION,
                        help="firmware version reported as sw_version")
    parser.add_argument("--hardware", default=DEFAULT_HARDWARE_VERSION,
                        help="hardware version reported as hw_version")
    parser.add_argument("--device-name", default=DEFAULT_DEVICE_NAME,
                        help="device registry name shown in Home Assistant")
    parser.add_argument("--suggested-area", default=DEFAULT_SUGGESTED_AREA,
                        help="area Home Assistant suggests when the device is discovered")
    parser.add_argument("--entity-prefix", default=DEFAULT_ENTITY_PREFIX,
                        help="prefix for entity ids; change it for a second speaker")
    parser.add_argument("--no-media-player-shim", action="store_true",
                        help="omit the helper entities the universal media player needs")
    parser.add_argument("--out", default=None,
                        help="output directory (default: tools/ha/payloads)")
    parser.add_argument("--json", action="store_true",
                        help="print the payloads to stdout instead of writing files")
    args = parser.parse_args(argv)

    if not _slug_ok(args.device_id):
        parser.error(
            f"--device-id must be lowercase alphanumeric/underscore: {args.device_id!r}"
        )
    if not _slug_ok(args.entity_prefix):
        parser.error(
            f"--entity-prefix must be lowercase alphanumeric/underscore: {args.entity_prefix!r}"
        )
    for label, value in (
        ("--base-topic", args.base_topic),
        ("--discovery-prefix", args.discovery_prefix),
    ):
        if not _topic_ok(value):
            parser.error(f"{label} is not a usable MQTT topic: {value!r}")

    entities = build_entities(
        device_id=args.device_id,
        base_topic=args.base_topic,
        firmware_version=args.firmware,
        hardware_version=args.hardware,
        device_name=args.device_name,
        suggested_area=args.suggested_area,
        discovery_prefix=args.discovery_prefix,
        entity_prefix=args.entity_prefix,
        include_media_player_shim=not args.no_media_player_shim,
    )
    meta = manifest(
        entities,
        device_id=args.device_id,
        base_topic=args.base_topic,
        firmware_version=args.firmware,
        discovery_prefix=args.discovery_prefix,
        entity_prefix=args.entity_prefix,
    )

    if args.json:
        sys.stdout.write(_dump({"manifest": meta, "entities": [e.envelope() for e in entities]}))
        return 0

    out_dir = Path(args.out) if args.out else Path(__file__).resolve().parent / "payloads"
    written = write_payloads(entities, out_dir, meta)
    required = meta["required_entity_count"]
    print(
        f"wrote {len(entities)} discovery payloads ({required} required, "
        f"{len(entities) - required} shim) to {out_dir}"
    )
    print(f"last file: {written[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
