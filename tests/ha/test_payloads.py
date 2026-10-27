#!/usr/bin/env python3
"""Validate the Moggie Home Assistant MQTT discovery payloads.

Runs with the standard library alone:

    python3 tests/ha/test_payloads.py

and is also pytest-compatible (every ``test_*`` function is a plain assertion).
The checks encode the parts of Home Assistant's MQTT discovery schema that
matter for this device — the required key per component, unique ids, the device
block, availability/LWT, and the topic contract — plus the specific entity set
the control plane promises in docs/HA.md.

Exit code 0 means every check passed. Anything else prints the failing checks.
"""

from __future__ import annotations

import importlib.util
import json
import re
import sys
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = REPO_ROOT / "tools" / "ha" / "gen_discovery.py"
PAYLOAD_DIR = REPO_ROOT / "tools" / "ha" / "payloads"

MANUFACTURER = "Sonos"
MODEL = "Play:5 (Gen 1)"
DISCOVERY_PREFIX = "homeassistant"

# --- schema rules -------------------------------------------------------------

#: Keys every discovery document must carry, regardless of component.
COMMON_REQUIRED: Sequence[str] = (
    "name",
    "unique_id",
    "default_entity_id",
    "availability_topic",
    "payload_available",
    "payload_not_available",
    "device",
    "origin",
)

#: Keys required per component.  Home Assistant requires less than this for some
#: components (a button needs only command_topic), but a Moggie entity that
#: cannot report state is not a useful entity, so the contract is stricter.
COMPONENT_REQUIRED: Dict[str, Sequence[str]] = {
    "number": ("command_topic", "state_topic", "min", "max", "step"),
    "switch": ("command_topic", "state_topic", "payload_on", "payload_off"),
    "sensor": ("state_topic",),
    "button": ("command_topic", "payload_press"),
    "select": ("command_topic", "state_topic", "options"),
    "update": (
        "command_topic",
        "state_topic",
        "device_class",
        "payload_install",
    ),
    # media_player is not an MQTT component in Home Assistant; the keys below are
    # the Moggie contract (see docs/HA.md), matching the historical MQTT media
    # player shape so the document is usable by other discovery consumers.
    "media_player": (
        "state_topic",
        "value_template",
        "command_topic",
        "volume_set_topic",
        "source_topic",
        "source_list",
    ),
}

#: Permitted top-level keys per component, taken from the Home Assistant
#: documentation for each MQTT platform — catching a typo such as `stat_topic`
#: is worth more than accepting unknown keys silently.
COMMON_ALLOWED: Sequence[str] = COMMON_REQUIRED + (
    "qos",
    "retain",
    "platform",
    "encoding",
    "enabled_by_default",
    "entity_picture",
    "icon",
    "json_attributes_template",
    "json_attributes_topic",
    "message_expiry_interval",
    "object_id",
    "visibility",
    "visible_by_default",
    "availability_mode",
    "availability",
    "availability_template",
    "device_class",
    "entity_category",
)

COMPONENT_ALLOWED: Dict[str, Sequence[str]] = {
    "number": (
        "command_template",
        "mode",
        "min",
        "max",
        "step",
        "optimistic",
        "payload_reset",
        "state_topic",
        "command_topic",
        "unit_of_measurement",
        "value_template",
        "suggested_display_precision",
    ),
    "switch": (
        "command_topic",
        "command_template",
        "state_topic",
        "state_on",
        "state_off",
        "payload_on",
        "payload_off",
        "payload_available",
        "payload_not_available",
        "optimistic",
        "value_template",
        "group",
    ),
    "sensor": (
        "state_topic",
        "value_template",
        "unit_of_measurement",
        "state_class",
        "last_reset_value_template",
        "expire_after",
        "force_update",
        "options",
        "suggested_display_precision",
    ),
    "button": ("command_topic", "command_template", "payload_press"),
    "select": ("command_topic", "command_template", "state_topic", "options", "optimistic",
               "value_template"),
    "update": (
        "command_topic",
        "latest_version_topic",
        "latest_version_template",
        "state_topic",
        "value_template",
        "payload_install",
        "release_summary",
        "release_url",
        "title",
        "display_precision",
    ),
    "media_player": (
        "state_topic",
        "value_template",
        "command_topic",
        "volume_set_topic",
        "volume_level_template",
        "source_topic",
        "source_template",
        "source_list",
        "payload_on",
        "payload_off",
        "payload_play",
        "payload_pause",
        "payload_stop",
        "supported_features",
    ),
}

#: Entity id -> (component, discovery object id, state topic suffix, command
#: topic suffix).  Suffixes are resolved against the device's topic root, so the
#: same table validates any device id / base topic.
ENTITY_CONTRACT: Dict[str, Tuple[str, str, Optional[str], Optional[str]]] = {
    "media_player.moggie_play5": ("media_player", "speaker", "player", "player/command"),
    "number.moggie_bass": ("number", "bass", "eq/bass", "eq/bass/set"),
    "number.moggie_treble": ("number", "treble", "eq/treble", "eq/treble/set"),
    "number.moggie_balance": ("number", "balance", "eq/balance", "eq/balance/set"),
    "switch.moggie_loudness": ("switch", "loudness", "loudness", "loudness/set"),
    "switch.moggie_led": ("switch", "led", "led", "led/set"),
    "switch.moggie_night_mode": ("switch", "night_mode", "night_mode", "night_mode/set"),
    "sensor.moggie_temperature": ("sensor", "temperature", "temperature", None),
    "sensor.moggie_uptime": ("sensor", "uptime", "uptime", None),
    "sensor.moggie_buffer": ("sensor", "buffer", "buffer", None),
    "button.moggie_identify": ("button", "identify", None, "identify"),
    "button.moggie_reboot": ("button", "reboot", None, "reboot"),
    "update.moggie_firmware": ("update", "firmware", "update", "update/install"),
}

#: Extra entities that exist only to let a universal media player mirror the
#: speaker on a stock Home Assistant (see tools/ha/ha/packages).
SHIM_CONTRACT: Dict[str, Tuple[str, str, Optional[str], Optional[str]]] = {
    "number.moggie_volume": ("number", "volume", "player", "player/volume/set"),
    "select.moggie_source": ("select", "source", "player", "player/source/set"),
    "sensor.moggie_playback_state": ("sensor", "playback_state", "player", None),
}

DISCOVERY_TOPIC_RE = re.compile(
    r"^" + DISCOVERY_PREFIX + r"/(?P<component>[a-z_]+)/(?P<node>[a-z0-9_]+)/"
    r"(?P<object_id>[a-z0-9_]+)/config$"
)
ENTITY_ID_RE = re.compile(r"^[a-z_]+\.[a-z0-9_]+$")
OBJECT_ID_RE = re.compile(r"^[a-z0-9][a-z0-9_]*$")
TOPIC_FORBIDDEN = ("+", "#", "\0", " ")


# --- load the generator -------------------------------------------------------


def load_generator():
    spec = importlib.util.spec_from_file_location("moggie_gen_discovery", GENERATOR_PATH)
    if spec is None or spec.loader is None:  # pragma: no cover - broken checkout
        raise RuntimeError(f"cannot load {GENERATOR_PATH}")
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves annotations through sys.modules, so register first.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


gen = load_generator()


def default_entities():
    return gen.build_entities()


def entities_for(device_id: str, base_topic: str, firmware: str = "0.1.0", **kwargs):
    return gen.build_entities(
        device_id=device_id, base_topic=base_topic, firmware_version=firmware, **kwargs
    )


# --- helpers ------------------------------------------------------------------


def _entity_index(entities) -> Dict[str, Any]:
    return {entity.entity_id: entity for entity in entities}


def _payload_json(payload: Dict[str, Any]) -> str:
    return json.dumps(payload, separators=(",", ":"))


def _collect(entities, *, root: str) -> List[str]:
    """Generic schema checks that apply to any Moggie device."""
    problems: List[str] = []

    seen_unique: Dict[str, str] = {}
    seen_entity: Dict[str, str] = {}
    seen_discovery: Dict[str, str] = {}
    seen_command: Dict[str, str] = {}
    devices = {}
    availability = set()

    for entity in entities:
        payload = entity.payload
        label = f"{entity.component}.{entity.object_id}"

        # 1. discovery topic shape and uniqueness
        match = DISCOVERY_TOPIC_RE.match(entity.topic)
        if not match:
            problems.append(f"{label}: discovery topic does not match the contract: {entity.topic}")
        else:
            if match.group("component") != entity.component:
                problems.append(
                    f"{label}: discovery topic component {match.group('component')!r} "
                    f"!= payload component {entity.component!r}"
                )
            if match.group("object_id") != entity.object_id:
                problems.append(
                    f"{label}: discovery topic object id {match.group('object_id')!r} "
                    f"!= {entity.object_id!r}"
                )
            expected_node = entity.topic.split("/")[2]
            if not expected_node.startswith("moggie_"):
                problems.append(f"{label}: node id is not moggie_<device_id>: {expected_node}")
        if entity.topic in seen_discovery:
            problems.append(
                f"{label}: discovery topic collides with {seen_discovery[entity.topic]}: {entity.topic}"
            )
        seen_discovery[entity.topic] = label

        # 2. required keys
        missing = [key for key in COMMON_REQUIRED if key not in payload]
        missing += [key for key in COMPONENT_REQUIRED.get(entity.component, ()) if key not in payload]
        if missing:
            problems.append(f"{label}: missing required keys: {', '.join(sorted(missing))}")

        # 3. unique id
        unique_id = payload.get("unique_id")
        if not isinstance(unique_id, str) or not unique_id:
            problems.append(f"{label}: unique_id is missing or empty")
        else:
            if unique_id in seen_unique:
                problems.append(f"{label}: unique_id {unique_id!r} duplicates {seen_unique[unique_id]}")
            seen_unique[unique_id] = label

        # 4. entity id
        entity_id = payload.get("default_entity_id")
        if not isinstance(entity_id, str) or not ENTITY_ID_RE.match(entity_id):
            problems.append(f"{label}: default_entity_id is not <domain>.<object_id>: {entity_id!r}")
        else:
            domain = entity_id.split(".", 1)[0]
            if domain != entity.component:
                problems.append(
                    f"{label}: default_entity_id domain {domain!r} != component {entity.component!r}"
                )
            if entity_id in seen_entity:
                problems.append(
                    f"{label}: default_entity_id {entity_id!r} duplicates {seen_entity[entity_id]}"
                )
            seen_entity[entity_id] = label

        # 5. availability (LWT) — one topic per device, with both payloads defined
        if payload.get("availability_topic") is None:
            problems.append(f"{label}: no availability_topic (entities must share the LWT)")
        else:
            availability.add(payload["availability_topic"])
            if payload.get("payload_available") in (None, ""):
                problems.append(f"{label}: payload_available missing")
            if payload.get("payload_not_available") in (None, ""):
                problems.append(f"{label}: payload_not_available missing")
            if payload.get("availability_topic") != payload["availability_topic"].strip():
                problems.append(f"{label}: availability_topic has surrounding whitespace")

        # 6. device block
        device = payload.get("device")
        if not isinstance(device, dict):
            problems.append(f"{label}: device block missing or not an object")
        else:
            for key in ("identifiers", "name", "manufacturer", "model", "sw_version",
                        "hw_version", "suggested_area"):
                if not device.get(key):
                    problems.append(f"{label}: device block missing {key}")
            identifiers = device.get("identifiers")
            if not isinstance(identifiers, list) or not identifiers:
                problems.append(f"{label}: device identifiers must be a non-empty list")
            if device.get("manufacturer") != MANUFACTURER:
                problems.append(
                    f"{label}: device manufacturer is {device.get('manufacturer')!r}, "
                    f"expected {MANUFACTURER!r}"
                )
            if device.get("model") != MODEL:
                problems.append(
                    f"{label}: device model is {device.get('model')!r}, expected {MODEL!r}"
                )
            devices[label] = json.dumps(device, sort_keys=True)

        # 7. topics: valid, device-scoped, and not ambiguous
        for key, value in payload.items():
            if not key.endswith("_topic") or not isinstance(value, str):
                continue
            if any(bad in value for bad in TOPIC_FORBIDDEN):
                problems.append(f"{label}: {key} contains an illegal MQTT character: {value!r}")
            if value.startswith("/") or value.endswith("/"):
                problems.append(f"{label}: {key} has an empty topic level: {value!r}")
            if not value.startswith(root + "/"):
                problems.append(f"{label}: {key} is outside the device topic root: {value!r}")
            if len(value.encode("utf-8")) > 65535:
                problems.append(f"{label}: {key} exceeds the MQTT string limit")

        command_topic = payload.get("command_topic")
        if isinstance(command_topic, str):
            if command_topic in seen_command:
                problems.append(
                    f"{label}: command_topic collides with {seen_command[command_topic]}: "
                    f"{command_topic}"
                )
            seen_command[command_topic] = label
            if payload.get("state_topic") == command_topic:
                problems.append(
                    f"{label}: state_topic and command_topic are the same topic, so the "
                    "device's echo would be indistinguishable from a command"
                )

        # 8. unknown keys
        allowed = set(COMMON_ALLOWED) | set(COMPONENT_ALLOWED.get(entity.component, ()))
        for key in payload:
            if key not in allowed:
                problems.append(f"{label}: unexpected key {key!r} for component {entity.component}")

        # 9. payload must fit in one MQTT message
        size = len(_payload_json(payload).encode("utf-8"))
        if size > 65535:
            problems.append(f"{label}: payload is {size} bytes, over the MQTT limit")

    # cross-entity consistency
    if len(set(devices.values())) > 1:
        problems.append("device block differs between entities; they must describe one device")
    if len(availability) > 1:
        problems.append(f"entities use more than one availability topic: {sorted(availability)}")

    return problems


# --- tests --------------------------------------------------------------------


def test_every_payload_matches_the_schema() -> None:
    problems = _collect(default_entities(), root=f"{gen.DEFAULT_BASE_TOPIC}/{gen.DEFAULT_DEVICE_ID}")
    assert not problems, "schema violations:\n  " + "\n  ".join(problems)


def test_required_entity_set_is_present() -> None:
    index = _entity_index(default_entities())
    missing = [entity_id for entity_id in gen.REQUIRED_ENTITY_IDS if entity_id not in index]
    assert not missing, f"missing required entities: {missing}"
    extra_required = [
        entity.entity_id
        for entity in default_entities()
        if entity.required and entity.entity_id not in gen.REQUIRED_ENTITY_IDS
    ]
    assert not extra_required, f"unexpected entities flagged required: {extra_required}"


def test_topic_contract() -> None:
    layout = gen.Layout(
        device_id=gen.DEFAULT_DEVICE_ID,
        base_topic=gen.DEFAULT_BASE_TOPIC,
        discovery_prefix=DISCOVERY_PREFIX,
        firmware_version=gen.DEFAULT_FIRMWARE_VERSION,
        hardware_version=gen.DEFAULT_HARDWARE_VERSION,
        device_name=gen.DEFAULT_DEVICE_NAME,
        suggested_area=gen.DEFAULT_SUGGESTED_AREA,
    )
    index = _entity_index(default_entities())
    problems = []

    for entity_id, (component, object_id, state_suffix, command_suffix) in ENTITY_CONTRACT.items():
        entity = index.get(entity_id)
        if entity is None:
            problems.append(f"{entity_id}: not generated")
            continue
        if entity.component != component:
            problems.append(f"{entity_id}: component is {entity.component}, expected {component}")
        if entity.object_id != object_id:
            problems.append(f"{entity_id}: discovery object id is {entity.object_id!r}, expected {object_id!r}")
        expected_topic = layout.discovery_topic(component, object_id)
        if entity.topic != expected_topic:
            problems.append(f"{entity_id}: discovery topic {entity.topic} != {expected_topic}")
        if state_suffix is None:
            if "state_topic" in entity.payload:
                problems.append(f"{entity_id}: should be command-only but has a state_topic")
        elif entity.payload.get("state_topic") != layout.topic(state_suffix):
            problems.append(
                f"{entity_id}: state_topic {entity.payload.get('state_topic')!r} "
                f"!= {layout.topic(state_suffix)!r}"
            )
        if command_suffix is None:
            if "command_topic" in entity.payload:
                problems.append(f"{entity_id}: should be state-only but has a command_topic")
        elif entity.payload.get("command_topic") != layout.topic(command_suffix):
            problems.append(
                f"{entity_id}: command_topic {entity.payload.get('command_topic')!r} "
                f"!= {layout.topic(command_suffix)!r}"
            )

    assert not problems, "topic contract violations:\n  " + "\n  ".join(problems)


def test_availability_is_the_device_lwt() -> None:
    root = f"{gen.DEFAULT_BASE_TOPIC}/{gen.DEFAULT_DEVICE_ID}"
    entities = default_entities()
    expected = f"{root}/availability"
    problems = [
        f"{entity.entity_id}: availability_topic {entity.payload.get('availability_topic')!r}"
        for entity in entities
        if entity.payload.get("availability_topic") != expected
    ]
    assert not problems, f"expected every entity on {expected!r}:\n  " + "\n  ".join(problems)


def test_equaliser_range_and_steps() -> None:
    index = _entity_index(default_entities())
    problems = []
    for entity_id in ("number.moggie_bass", "number.moggie_treble", "number.moggie_balance"):
        payload = index[entity_id].payload
        if (payload.get("min"), payload.get("max")) != (-10, 10):
            problems.append(f"{entity_id}: range is {payload.get('min')}..{payload.get('max')}, expected -10..10")
        if payload.get("step") != 1:
            problems.append(f"{entity_id}: step is {payload.get('step')!r}, expected 1")
    volume = index["number.moggie_volume"].payload
    if (volume.get("min"), volume.get("max")) != (0, 1):
        problems.append(f"number.moggie_volume: range is {volume.get('min')}..{volume.get('max')}, expected 0..1")
    assert not problems, "\n  ".join(problems)


def test_switch_payloads_are_on_off() -> None:
    index = _entity_index(default_entities())
    problems = []
    for entity_id in ("switch.moggie_loudness", "switch.moggie_led", "switch.moggie_night_mode"):
        payload = index[entity_id].payload
        if payload.get("payload_on") != "ON" or payload.get("payload_off") != "OFF":
            problems.append(
                f"{entity_id}: payload_on/off are {payload.get('payload_on')!r}/"
                f"{payload.get('payload_off')!r}, expected ON/OFF"
            )
    assert not problems, "\n  ".join(problems)


def test_diagnostic_sensors() -> None:
    index = _entity_index(default_entities())
    problems = []

    temperature = index["sensor.moggie_temperature"].payload
    if temperature.get("device_class") != "temperature":
        problems.append("sensor.moggie_temperature: device_class must be temperature")
    if temperature.get("unit_of_measurement") != "°C":
        problems.append(
            f"sensor.moggie_temperature: unit is {temperature.get('unit_of_measurement')!r}, expected °C"
        )

    uptime = index["sensor.moggie_uptime"].payload
    if uptime.get("device_class") != "duration":
        problems.append("sensor.moggie_uptime: device_class must be duration")
    if uptime.get("unit_of_measurement") != "s":
        problems.append(f"sensor.moggie_uptime: unit is {uptime.get('unit_of_measurement')!r}, expected s")

    buffer_sensor = index["sensor.moggie_buffer"].payload
    if buffer_sensor.get("unit_of_measurement") != "%":
        problems.append(
            f"sensor.moggie_buffer: unit is {buffer_sensor.get('unit_of_measurement')!r}, expected %"
        )

    for entity_id in ("sensor.moggie_temperature", "sensor.moggie_uptime", "sensor.moggie_buffer"):
        if index[entity_id].payload.get("entity_category") != "diagnostic":
            problems.append(f"{entity_id}: entity_category must be diagnostic")
    assert not problems, "\n  ".join(problems)


def test_update_entity_triggers_ota() -> None:
    payload = _entity_index(default_entities())["update.moggie_firmware"].payload
    problems = []
    if payload.get("device_class") != "firmware":
        problems.append("update.moggie_firmware: device_class must be firmware")
    if payload.get("payload_install") != "install":
        problems.append("update.moggie_firmware: payload_install must be install")
    if not str(payload.get("command_topic", "")).endswith("/update/install"):
        problems.append(
            f"update.moggie_firmware: command_topic {payload.get('command_topic')!r} "
            "must end in /update/install (the OTA trigger)"
        )
    # The state topic carries a JSON document with installed_version and
    # latest_version; a value_template would collapse it to a string and hide
    # latest_version, which is why it must stay absent.
    if payload.get("value_template"):
        problems.append("update.moggie_firmware: value_template would defeat JSON state parsing")
    if payload.get("state_topic", "").rsplit("/", 1)[-1] != "update":
        problems.append(
            f"update.moggie_firmware: state_topic {payload.get('state_topic')!r} must be the "
            "update document"
        )
    # The device block's sw_version is what the device page shows; it must agree
    # with the installed_version in the state document.
    if payload.get("device", {}).get("sw_version") != gen.DEFAULT_FIRMWARE_VERSION:
        problems.append("update.moggie_firmware: device.sw_version is not the firmware version")
    assert not problems, "\n  ".join(problems)


def test_media_player_commands_are_covered() -> None:
    payload = _entity_index(default_entities())["media_player.moggie_play5"].payload
    problems = []
    for key in ("command_topic", "volume_set_topic", "source_topic", "state_topic"):
        if not payload.get(key):
            problems.append(f"media_player.moggie_play5: {key} missing")
    if payload.get("payload_on") in (None, "") or payload.get("payload_off") in (None, ""):
        problems.append("media_player.moggie_play5: turn_on/turn_off payloads missing")
    for source in gen.SOURCES:
        if source not in payload.get("source_list", []):
            problems.append(f"media_player.moggie_play5: source {source!r} missing from source_list")
    # volume_set must accept 0..1 and select_source must accept a source name,
    # so the volume/state payloads carry a float and a JSON document respectively.
    if "volume_level" not in payload.get("volume_level_template", ""):
        problems.append("media_player.moggie_play5: volume_level_template does not read volume_level")
    if "state" not in payload.get("value_template", ""):
        problems.append("media_player.moggie_play5: value_template does not read state")
    if "source" not in payload.get("source_template", ""):
        problems.append("media_player.moggie_play5: source_template does not read source")
    assert not problems, "\n  ".join(problems)


def test_shim_entities_back_the_media_player() -> None:
    index = _entity_index(default_entities())
    problems = []
    for entity_id, (component, object_id, state_suffix, command_suffix) in SHIM_CONTRACT.items():
        entity = index.get(entity_id)
        if entity is None:
            problems.append(f"{entity_id}: shim entity not generated")
            continue
        if entity.required:
            problems.append(f"{entity_id}: shim entity must not be flagged required")
        if state_suffix and entity.payload.get("state_topic", "").split("/")[-1] != state_suffix.split("/")[-1]:
            problems.append(f"{entity_id}: state_topic does not read the player state document")
        if command_suffix and not str(entity.payload.get("command_topic", "")).endswith(command_suffix):
            problems.append(f"{entity_id}: command_topic does not end in {command_suffix}")
    assert not problems, "\n  ".join(problems)


def test_device_block_identity() -> None:
    device = default_entities()[0].payload["device"]
    problems = []
    if device.get("identifiers") != [f"moggie_{gen.DEFAULT_DEVICE_ID}"]:
        problems.append(f"identifiers are {device.get('identifiers')!r}")
    if device.get("suggested_area") != gen.DEFAULT_SUGGESTED_AREA:
        problems.append(f"suggested_area is {device.get('suggested_area')!r}")
    if device.get("sw_version") != gen.DEFAULT_FIRMWARE_VERSION:
        problems.append(f"sw_version is {device.get('sw_version')!r}")
    if not device.get("hw_version"):
        problems.append("hw_version is empty")
    assert not problems, "\n  ".join(problems)


def test_generation_is_deterministic() -> None:
    first = [_payload_json(entity.payload) for entity in default_entities()]
    second = [_payload_json(entity.payload) for entity in default_entities()]
    assert first == second, "two runs of the generator produced different payloads"


def test_checked_in_payloads_match_the_generator() -> None:
    if not PAYLOAD_DIR.is_dir():
        return
    files = sorted(path for path in PAYLOAD_DIR.glob("*.json") if not path.name.startswith("_"))
    if not files:
        return
    on_disk = {}
    for path in files:
        data = json.loads(path.read_text(encoding="utf-8"))
        on_disk[path.name] = data
    generated = {entity.filename: entity.envelope() for entity in default_entities()}
    problems = []
    for name, envelope in generated.items():
        if name not in on_disk:
            problems.append(f"{name}: generated but missing from {PAYLOAD_DIR.name}/")
            continue
        if on_disk[name]["topic"] != envelope["topic"]:
            problems.append(f"{name}: topic on disk is {on_disk[name]['topic']!r}")
        if _payload_json(on_disk[name]["payload"]) != _payload_json(envelope["payload"]):
            problems.append(f"{name}: payload on disk differs from the generator")
    for name in on_disk:
        if name not in generated:
            problems.append(f"{name}: stale file, not produced by the generator")
    if problems:
        raise AssertionError(
            "checked-in payloads are out of date — run "
            "`python3 tools/ha/gen_discovery.py`:\n  " + "\n  ".join(problems)
        )


def test_manifest_is_consistent() -> None:
    if not PAYLOAD_DIR.is_dir():
        return
    manifest_path = PAYLOAD_DIR / "_manifest.json"
    if not manifest_path.is_file():
        return
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entities = default_entities()
    problems = []
    if manifest.get("entity_count") != len(entities):
        problems.append(f"manifest says {manifest.get('entity_count')}, generator makes {len(entities)}")
    listed = {entry["entity_id"] for entry in manifest.get("entities", [])}
    generated = {entity.entity_id for entity in entities}
    if listed != generated:
        problems.append(f"manifest entities differ: {sorted(listed ^ generated)}")
    if manifest.get("node_id") != f"moggie_{gen.DEFAULT_DEVICE_ID}":
        problems.append(f"manifest node_id is {manifest.get('node_id')!r}")
    assert not problems, "\n  ".join(problems)


def test_other_device_ids_are_valid() -> None:
    cases = (
        ("kitchen", "moggie", "1.2.3", "moggie_kitchen"),
        ("study_shelf", "home/moggie", "0.9.0", "moggie_study"),
        ("play5", "moggie/dev", "2.0.0-rc1", gen.DEFAULT_ENTITY_PREFIX),
    )
    for device_id, base_topic, firmware, entity_prefix in cases:
        entities = entities_for(
            device_id, base_topic, firmware, entity_prefix=entity_prefix
        )
        problems = _collect(entities, root=f"{base_topic}/{device_id}")
        assert not problems, (
            f"schema violations for device_id={device_id} base_topic={base_topic}:\n  "
            + "\n  ".join(problems)
        )
        index = _entity_index(entities)
        if entity_prefix == gen.DEFAULT_ENTITY_PREFIX:
            assert set(index) == set(gen.REQUIRED_ENTITY_IDS) | set(gen.SHIM_ENTITY_IDS), (
                f"device_id={device_id}: unexpected entity ids {sorted(index)}"
            )
            continue
        # a second speaker must not fight the first for entity ids
        overlap = sorted(set(index) & (set(gen.REQUIRED_ENTITY_IDS) | set(gen.SHIM_ENTITY_IDS)))
        assert not overlap, f"device_id={device_id} reused default entity ids: {overlap}"
        for entity in entities:
            assert f"moggie_{device_id}" in entity.payload["unique_id"], (
                f"device_id={device_id}: unique_id {entity.unique_id!r} is not device scoped"
            )
            assert f"moggie_{device_id}" in entity.topic, (
                f"device_id={device_id}: discovery topic {entity.topic!r} is not device scoped"
            )


def test_shim_can_be_disabled() -> None:
    entities = gen.build_entities(include_media_player_shim=False)
    entity_ids = {entity.entity_id for entity in entities}
    assert entity_ids == set(gen.REQUIRED_ENTITY_IDS), (
        "disabling the shim must leave exactly the required entity set, got "
        f"{sorted(entity_ids ^ set(gen.REQUIRED_ENTITY_IDS))}"
    )
    problems = _collect(entities, root=f"{gen.DEFAULT_BASE_TOPIC}/{gen.DEFAULT_DEVICE_ID}")
    assert not problems, "\n  ".join(problems)


def test_state_seed_messages_are_publishable() -> None:
    root = f"{gen.DEFAULT_BASE_TOPIC}/{gen.DEFAULT_DEVICE_ID}"
    messages = gen.state_messages()
    problems = []
    for message in messages:
        topic = message["topic"]
        if not topic.startswith(root + "/"):
            problems.append(f"{topic}: outside the device topic root")
        if any(bad in topic for bad in TOPIC_FORBIDDEN):
            problems.append(f"{topic}: illegal MQTT character")
        if not isinstance(message["payload"], str) or not message["payload"]:
            problems.append(f"{topic}: empty payload")
    update_doc = next(m for m in messages if m["topic"] == f"{root}/update")
    update_payload = json.loads(update_doc["payload"])
    if update_payload.get("installed_version") != gen.DEFAULT_FIRMWARE_VERSION:
        problems.append("update document does not carry installed_version")
    if "latest_version" not in update_payload:
        problems.append("update document does not carry latest_version")
    player = next(m for m in messages if m["topic"] == f"{root}/player")
    document = json.loads(player["payload"])
    for key in ("state", "volume_level", "source"):
        if key not in document:
            problems.append(f"player state document is missing {key}")
    assert document["state"] in {"playing", "paused", "idle", "off"}, document["state"]
    assert 0.0 <= float(document["volume_level"]) <= 1.0, document["volume_level"]
    assert not problems, "\n  ".join(problems)


# --- runner -------------------------------------------------------------------


def _test_functions() -> List[Callable[[], Any]]:
    return [
        value
        for name, value in sorted(globals().items())
        if name.startswith("test_") and callable(value)
    ]


def test_every_documented_entity_is_accounted_for() -> None:
    """The contract may not promise anything that is not accounted for.

    Three states are allowed: published by the daemon, blocked with a recorded
    reason, or explicitly superseded. Anything else is a document claiming a
    capability nobody has built -- which is exactly the drift #14 describes.
    """
    documented = set(gen.REQUIRED_ENTITY_IDS)
    published = set(gen.PUBLISHED_ENTITY_IDS)
    blocked = set(gen.UNSUPPORTED_ENTITY_IDS)

    assert published.isdisjoint(blocked), (
        f"entity both published and blocked: {sorted(published & blocked)}"
    )

    superseded = documented - published - blocked
    assert superseded == {"media_player.moggie_play5"}, (
        "documented entities neither published, blocked nor explained as "
        f"superseded: {sorted(superseded)}"
    )


def test_blocked_entities_carry_a_reason() -> None:
    for entity_id, reason in gen.UNSUPPORTED_ENTITY_IDS.items():
        assert reason.strip(), f"{entity_id} is blocked but records no reason"


def test_published_set_matches_the_generated_entities() -> None:
    entity_ids = {entity.entity_id for entity in gen.build_entities()}
    assert set(gen.PUBLISHED_ENTITY_IDS) <= entity_ids, (
        "published entities missing from the generated payloads: "
        f"{sorted(set(gen.PUBLISHED_ENTITY_IDS) - entity_ids)}"
    )


def main() -> int:
    entities = default_entities()
    required = sum(1 for entity in entities if entity.required)
    failures: List[Tuple[str, str]] = []

    published = sum(1 for e in entities if e.entity_id in gen.PUBLISHED_ENTITY_IDS)
    shim = sum(1 for e in entities if e.entity_id in gen.SHIM_ENTITY_IDS)
    print(f"Moggie MQTT discovery payloads: {len(entities)} entities "
          f"({published} published by sonosd, {shim} media-player shim, "
          f"{len(entities) - published - shim} design-only)")
    print(f"discovery prefix: {DISCOVERY_PREFIX}/<component>/moggie_<device_id>/<object_id>/config")
    print()

    for function in _test_functions():
        try:
            function()
        except AssertionError as error:
            failures.append((function.__name__, str(error)))
            print(f"FAIL  {function.__name__}")
        except Exception as error:  # noqa: BLE001 - a broken test must not hide the rest
            failures.append((function.__name__, f"{type(error).__name__}: {error}"))
            print(f"ERROR {function.__name__}: {type(error).__name__}: {error}")
        else:
            print(f"ok    {function.__name__}")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed:\n")
        for name, message in failures:
            print(f"--- {name} ---\n{message}\n")
        return 1

    print(f"all checks passed — {len(entities)} entities ({published} published "
          f"by sonosd, {shim} media-player shim, "
          f"{len(entities) - published - shim} design-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
