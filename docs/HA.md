# Home Assistant

The control plane is MQTT discovery and nothing else: `sonosd` publishes one
retained discovery document per entity, Home Assistant creates the entities, and
from then on the speaker is a normal HA device. No YAML in HA, no custom
component, no cloud.

This directory holds the host-side tooling for it:

| file | what it does |
|---|---|
| `tools/ha/gen_discovery.py` | generates every discovery document (JSON, one file per entity) |
| `tools/ha/publish.py` | publishes them to a broker — stdlib MQTT client, no dependencies |
| `tools/ha/compose.yaml` | mosquitto + Home Assistant test rig on non-default host ports |
| `tools/ha/payloads/` | the generated reference payloads, checked in |
| `tools/ha/ha-config/` | Home Assistant's own config, including the universal media player wrapper |
| `tests/ha/test_payloads.py` | validates the payloads against the discovery schema rules |

Everything below is exercised by that rig; nothing here has run against real
hardware yet.

## Entities

> **The table below is the *target* contract.** What the daemon actually
> publishes today uses one retained state document rather than one topic per
> entity — see [Implemented today](#implemented-today) at the end of this
> document for the real topics and the reason for the difference.

Discovery topic: `homeassistant/<component>/moggie_<device_id>/<object_id>/config`,
published retained with QoS 1. State and command topics sit under
`moggie/<device_id>/`.

| Entity | Component | State topic | Command topic |
|---|---|---|---|
| `media_player.moggie_play5` ¹ | media_player | `player` (JSON) | `player/command` |
| `number.moggie_bass` | number | `eq/bass` | `eq/bass/set` |
| `number.moggie_treble` | number | `eq/treble` | `eq/treble/set` |
| `number.moggie_balance` | number | `eq/balance` | `eq/balance/set` |
| `switch.moggie_loudness` | switch | `loudness` | `loudness/set` |
| `switch.moggie_led` | switch | `led` | `led/set` |
| `switch.moggie_night_mode` | switch | `night_mode` | `night_mode/set` |
| `sensor.moggie_temperature` | sensor | `temperature` | — |
| `sensor.moggie_uptime` | sensor | `uptime` | — |
| `sensor.moggie_buffer` | sensor | `buffer` | — |
| `button.moggie_identify` | button | — | `identify` |
| `button.moggie_reboot` | button | — | `reboot` |
| `update.moggie_firmware` | update | `update` (JSON) | `update/install` |

Plus three helper entities that exist only so a stock Home Assistant can present
a working media player (see [the media_player problem](#the-media_player-problem)):

| Entity | Component | State topic | Command topic |
|---|---|---|---|
| `number.moggie_volume` | number | `player` (JSON) | `player/volume/set` |
| `select.moggie_source` | select | `player` (JSON) | `player/source/set` |
| `sensor.moggie_playback_state` | sensor | `player` (JSON) | — |

All paths above are relative to `moggie/play5/`; prefix them with the base topic
and device id for anything else.

¹ The discovery document for the player is published as specified, but stock Home
Assistant ignores it — the MQTT integration has no media_player platform. What
you actually see in HA is a universal media player wrapped around the three
helper entities; same topics, same commands. See
[the media_player problem](#the-media_player-problem).

The equaliser numbers accept `-10..10` in steps of one, the volume helper takes
`0.0..1.0`, the switches use `ON`/`OFF`, and the buttons take `PRESS`.

### Payload rules that matter

- **`unique_id`** is `<node>_<component>_<object_id>`, e.g.
  `moggie_play5_number_bass`. It is what HA keys the entity registry on, so it
  must never change between firmware versions.
- **`default_entity_id`** pins the entity id, e.g. `number.moggie_bass`. Without
  it HA would invent `number.moggie_moggie_play5_bass` from the device name and
  the discovery node id. Requires HA 2025.10 or newer; on older versions the
  entity still works, it just gets the longer generated id.
- **`device`** is repeated in every document (same values, so HA merges them into
  one device): manufacturer `Sonos`, model `Play:5 (Gen 1)`, plus `sw_version`,
  `hw_version` and `suggested_area`. `sw_version` is the installed firmware
  version, shown on the device page; the `update` entity takes the same value
  from its own state document, so keep the two in step.
- **`availability_topic`** is `moggie/<device_id>/availability` with
  `payload_available: online` and `payload_not_available: offline`. The broker
  must hold the offline payload as the connection's last will, and the firmware
  publishes `online` retained once it is ready.
- Discovery documents are **retained**, so a restart of Home Assistant
  rediscovers everything without the speaker having to notice.

## The media_player problem

Home Assistant's MQTT integration has **no media_player platform**. That is not a
guess: `media_player` is absent from `SUPPORTED_COMPONENTS` in
`homeassistant/components/mqtt/const.py`, and there is no MQTT media player page
in the documentation. A discovery document published to
`homeassistant/media_player/...` is therefore ignored — no entity, no error.

Moggie still generates it, because it is the right contract for the device and
other MQTT discovery consumers (and a future HA) can use it. To get a *working*
`media_player` on a stock install, the rig loads
`tools/ha/ha-config/packages/moggie_media_player.yaml`, a
[universal media player](https://www.home-assistant.io/integrations/universal/)
wrapped around the MQTT entities:

- state ← `sensor.moggie_playback_state`
- volume ← `number.moggie_volume`
- source ← `select.moggie_source`
- `play`/`pause`/`stop`/`turn_on`/`turn_off` → `moggie/play5/player/command`
- `volume_set` → `number.moggie_volume`, `select_source` → `select.moggie_source`

So every control still travels over MQTT and the speaker stays the single source
of truth; the wrapper only gives HA an entity to hang the transport buttons on.
Its `name: Moggie Play5` is what produces the entity id `media_player.moggie_play5`
(Home Assistant slugs the name), and its `unique_id` is what keeps that entity
stable across restarts. Pass `--no-media-player-shim` to `gen_discovery.py` if
you want strictly the thirteen required entities.

The native alternative is UPnP: the same Home Assistant image ships
`integrations/dlna_dmr`, which autodiscovers a MediaRenderer over SSDP.
`firmware/dlna/` implements that device (SSDP, description, SCPDs, SOAP, GENA),
so a control point can create a real `media_player` without the universal-player
wrapper. It is protocol-tested but has not been run against a live HA instance,
and its audio output still depends on the sonosd backend. See
[DLNA.md](DLNA.md).

## Topics

```
moggie/play5/availability              online | offline        (LWT)
moggie/play5/player                    {"state","volume_level","source"}
moggie/play5/player/command            play | pause | stop | turn_on | turn_off
moggie/play5/player/volume/set         0.0 – 1.0
moggie/play5/player/source/set         AirPlay | Spotify | DLNA | Snapcast
moggie/play5/eq/{bass,treble,balance}  -10 – 10
moggie/play5/eq/{...}/set              -10 – 10
moggie/play5/{loudness,led,night_mode} ON | OFF
moggie/play5/{...}/set                 ON | OFF
moggie/play5/temperature               °C, one decimal
moggie/play5/uptime                    seconds since boot
moggie/play5/buffer                    percent of the audio buffer in use
moggie/play5/identify                  PRESS  → play the identification tone
moggie/play5/reboot                    PRESS  → orderly reboot
moggie/play5/update                    {"installed_version","latest_version","in_progress","update_percentage"}
moggie/play5/update/install            install → run the OTA path
```

The player state document is one message so the volume, source and playback
entities never disagree with each other:

```json
{"state": "playing", "volume_level": 0.35, "source": "AirPlay"}
```

`state` is one of `playing`, `paused`, `idle`, `off`. `volume_level` is 0–1.
`source` is one of the arbiter's inputs.

The update document is also one message. The `update` entity stays template-free
on purpose: Home Assistant decodes the JSON itself and takes
`installed_version` and `latest_version` from it, and shows a progress bar from
`update_percentage` while an OTA runs.

```json
{"installed_version": "0.1.0", "latest_version": "0.2.0", "in_progress": false,
 "update_percentage": null}
```

`on` means the two versions differ. To watch the entity flip without touching
firmware, publish a newer `latest_version`:

```bash
docker exec moggie-ha-mosquitto mosquitto_pub -t moggie/play5/update -m \
  '{"installed_version":"0.1.0","latest_version":"0.2.0","in_progress":false}'
```

`update/install` is the only topic that can change the firmware: on `install`,
`sonosd` hands over to the updater, which fetches the signed manifest, verifies
it, streams the image into the inactive slot, reads it back and flips
`/jffs/bootslot`. See [`ARCHITECTURE.md`](ARCHITECTURE.md#ota). Nothing on the
MQTT side can bypass the signature check.

## Generating payloads

```bash
python3 tools/ha/gen_discovery.py                  # writes tools/ha/payloads/
python3 tools/ha/gen_discovery.py --json           # same documents on stdout
python3 tools/ha/gen_discovery.py --device-id kitchen --entity-prefix moggie_kitchen \
    --base-topic moggie --firmware 0.2.0 --out /tmp/kitchen
```

| flag | default | meaning |
|---|---|---|
| `--device-id` | `play5` | node id becomes `moggie_<device_id>`; also the topic segment |
| `--base-topic` | `moggie` | root of the state/command tree |
| `--discovery-prefix` | `homeassistant` | must match HA's discovery prefix |
| `--entity-prefix` | `moggie` | prefix for entity ids; change it for a second speaker |
| `--firmware` | `0.1.0` | `sw_version`, and the version the update entity compares against |
| `--hardware` | `Play:5 Gen 1` | `hw_version` |
| `--device-name` | `Moggie` | device registry name |
| `--suggested-area` | `Living Room` | area HA suggests on discovery |
| `--no-media-player-shim` | off | emit only the thirteen required entities |
| `--out` | `tools/ha/payloads` | output directory |

One speaker per `--device-id`. Two speakers need two entity prefixes as well, or
`number.moggie_bass` on the second one collides with the first.

## Publishing

`publish.py` speaks MQTT 3.1.1 directly, so it needs nothing but python3.

```bash
# to the local rig, with retained state so the entities show values immediately
python3 tools/ha/publish.py --host 127.0.0.1 --port 18830 --seed-state --verify

# to a broker with credentials (keep the password out of shell history)
MOGGIE_MQTT_PASSWORD=... python3 tools/ha/publish.py --host broker.lan --username moggie

# show what would be sent
python3 tools/ha/publish.py --dry-run
```

`--verify` subscribes back to every topic it published and compares the retained
payloads, which proves the broker kept them (and therefore that HA will see them
on its next subscribe). `--seed-state` also publishes the availability topic as
`online` plus a plausible snapshot of every state topic; without it the entities
exist but read `unknown` until the firmware says otherwise.

## The rig

```bash
cd tools/ha
docker compose up -d          # pulls eclipse-mosquitto:2 and home-assistant:stable
./sync-config.sh              # copies ha-config/ into the HA volume and restarts it
docker compose ps
```

| service | image | host port |
|---|---|---|
| `mosquitto` | `eclipse-mosquitto:2` | `18830` → 1883 |
| `homeassistant` | `ghcr.io/home-assistant/home-assistant:stable` | `18123` → 8123 |

The host ports are deliberately not 1883/8123, so the rig cannot touch a real
broker or Home Assistant on the same machine. `tools/ha/mosquitto/mosquitto.conf`
allows anonymous access — it is a development rig bound to localhost ports, and
the file says so at the top. Do not reuse it anywhere else.

Home Assistant's `/config` is a **named volume** (`moggie-ha-config`), not a bind
mount. That is deliberate: with userns-remapped Docker (`userns-remap=default`,
the default on several distros) a container cannot write to a bind mount owned
by your user, and Home Assistant dies on `mkdir /config/deps`. The repository
copy in `tools/ha/ha-config/` is the source of truth — edit it, run
`./sync-config.sh`, and the rig follows. `packages/` holds the media player
wrapper, loaded through the `packages:` key in `configuration.yaml`.

If your Docker does not remap uids and you would rather edit the live config
directly, swap `ha-config:/config` for `./ha-config:/config` in `compose.yaml`
and skip `sync-config.sh`.

### First boot

1. **Onboard.** Open <http://localhost:18123>, create an owner account. This is
   local only; nothing leaves the machine.

2. **Add MQTT.** Settings → Devices & services → **Add integration** → *MQTT*:

   | field | value |
   |---|---|
   | broker | `mosquitto` |
   | port | `1883` |
   | username / password | empty |
   | discovery | on, prefix `homeassistant` |

   Broker settings cannot be put in `configuration.yaml` any more — Home
   Assistant only accepts them through the UI or its config-entry flow, which is
   why this step is manual. Scripted setups can drive the same flow over REST:
   `POST /api/config/config_entries/flow` with `{"handler": "mqtt"}`, then
   `POST /api/config/config_entries/flow/<flow_id>` with `broker`, `port`,
   `protocol` and `other_settings`.

3. **Publish.** From the repo root:

   ```bash
   python3 tools/ha/publish.py --host 127.0.0.1 --port 18830 --seed-state --verify
   ```

   The entities appear under Settings → Devices & services → MQTT → *Moggie*,
   in the area Home Assistant suggests (`Living Room`), and the media player
   shows `paused`, 35 %, source `AirPlay`.

4. **Get a token** for scripting. Click your user at the bottom of the sidebar,
   open the **Security** tab, find *Long-lived access tokens* and choose
   **Create token**. Name it `moggie-rig`. Copy it once — HA never shows it
   again.

   ```bash
   export HA_TOKEN='...'
   curl -s -H "Authorization: Bearer $HA_TOKEN" http://localhost:18123/api/ \
     | python3 -m json.tool
   ```

5. **Check the entities.**

   ```bash
   curl -s -H "Authorization: Bearer $HA_TOKEN" http://localhost:18123/api/states \
     | python3 -c 'import json, sys
   for state in sorted(json.load(sys.stdin), key=lambda s: s["entity_id"]):
       if state["entity_id"].split(".")[1].startswith("moggie"):
           print(state["entity_id"], state["state"])'
   ```

6. **Drive it.** `POST /api/services/media_player/media_play` with
   `{"entity_id": "media_player.moggie_play5"}`, then watch the command land:

   ```bash
   docker exec moggie-ha-mosquitto mosquitto_sub -t 'moggie/#' -v -C 5
   ```

### Stopping

```bash
docker compose down            # keeps the named volumes, so onboarding survives
```

Do not pass `-v` unless you want to lose the HA install and re-onboard.

## Troubleshooting

**Entities never appear.** Check the retained documents are actually on the
broker: `python3 tools/ha/publish.py --verify`. Then check HA's log —
`docker compose logs homeassistant | grep -i mqtt`. A payload that fails
validation is logged with the offending key and dropped; the other entities
still load.

**Entity ids have `moggie_play5` in them.** The HA version is older than 2025.10
and ignores `default_entity_id`. Rename once in the UI, or upgrade.

**Everything is `unavailable`.** Nothing published `online` to
`moggie/play5/availability`. `--seed-state` does it for the rig; the firmware
must do it on boot, retained, and let the broker hold `offline` as its last will.

**Media player is missing but everything else is there.** Expected: see
[the media_player problem](#the-media_player-problem). Check that
`tools/ha/ha-config/packages/moggie_media_player.yaml` exists and that
`configuration.yaml` includes `packages: !include_dir_named packages`.

**The update entity always shows an update.** The `update` document's
`latest_version` is greater than its `installed_version`. Publish a document
where they match to clear it, or generate payloads with `--firmware` at the
newer version once the speaker is actually running it.

## What the firmware owes this contract

`sonosd` (or the arbiter) must:

- publish `online` retained to `moggie/<id>/availability` at startup, and register
  `offline` as the broker's last will for that topic;
- publish retained discovery documents on boot, or at least after a factory
  reset — they are cheap and they make HA recovery automatic;
- keep the `player` document authoritative: one message carrying `state`,
  `volume_level` and `source`, updated whenever any of them changes;
- echo every accepted command onto the matching state topic, so HA never has to
  guess;
- ignore a discovery document whose `sw_version` is older than the running
  firmware, and bump `sw_version` on upgrade.

Reboot and `update/install` are the two commands with consequences. Reboot goes
through the same orderly path as a GPIO long-press; `install` goes through the
OTA path and its signature check, never around it.

## Implemented today

Verified by `tests/mqtt/run.sh` against a real Mosquitto broker: 20 checks, 0
failures.

The daemon publishes **one retained state document** and **one command topic**,
and every entity reads from it with `value_template`. That is a deliberate
departure from the one-topic-per-entity table above:

- a 32 MiB device should not hold twelve retained documents when one will do;
- one parser handles inbound commands instead of one per entity;
- HA's `value_template` is designed for exactly this.

### Topics

```
sonos/<device_id>/state          retained JSON, one document, all values
sonos/<device_id>/cmd            inbound command JSON
sonos/<device_id>/availability   retained "online"/"offline" (+ last will)
sonos/<device_id>/uptime         seconds since start
```

### Entities published

| Entity | Component | Source |
|---|---|---|
| `moggie_<id>_volume` | number | `value_json.volume`, 0–100 |
| `moggie_<id>_bass` | number | `value_json.bass`, −10–10 |
| `moggie_<id>_treble` | number | `value_json.treble`, −10–10 |
| `moggie_<id>_balance` | number | `value_json.balance`, −10–10 |
| `moggie_<id>_loudness` | switch | `value_json.loudness` |
| `moggie_<id>_night_mode` | switch | `value_json.night_mode` |
| `moggie_<id>_uptime` | sensor | `sonos/<id>/uptime` |

HA sends the bare value to `cmd`, so each entity carries a `command_template`
that wraps it back into the JSON the daemon parses — `{"volume": 42}` for a
number, `{"loudness": "ON"}` for a switch. The switch payload is a quoted
string, which is why `json_get_bool()` accepts `ON`/`OFF` as well as
`true`/`false`/`1`/`0`.

### Deliberately not published

Publishing discovery for a control that does nothing is worse than having no
entity — a user sees a working-looking switch and gets silence.

| Entity | Why not |
|---|---|
| `media_player` | HA has no MQTT media_player; see #13 and the `dlna_dmr` plan |
| `led` | no GPIO/LED support in the daemon yet |
| `temperature` | no sensor read path |
| `buffer` | no PCM stream to measure |
| `update` | OTA is not wired into the daemon |
| `identify`, power-cycle button | no safe path implemented |

### Still to reconcile

`tools/ha/payloads/` and `tools/ha/gen_discovery.py` still generate the
target-contract documents (one topic per entity). Until they are aligned, the
rig validates a contract the daemon does not speak. Tracked in #14.
