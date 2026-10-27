#!/usr/bin/env bash
# Verify that Home Assistant exposes Moggie as a *native* media_player through
# its dlna_dmr integration, and that commands actually reach the renderer.
#
# This is the check that #13 asked for. It is deliberately API-driven rather
# than log-driven: an entity that merely exists in the registry proves nothing.
#
# Prerequisites (see docs/HA.md):
#   - the rig is up:            tools/ha/docker compose up -d && ./sync-config.sh
#   - the renderer is running:  dlna-renderer --http-addr 0.0.0.0 --http-port 49494 \
#                                 --ip 172.17.0.1 --ssdp-port 19000
#   - the dlna_dmr entry is added (config entry, NOT YAML -- see docs/HA.md)
#   - HA_API_TOKEN and HA_URL are set, HA_ENTITY_ID defaults to the dlna entity
#
# Exit status is 0 only if every assertion passed.

set -uo pipefail

HA_URL="${HA_URL:-http://127.0.0.1:18123}"
TOKEN="${HA_API_TOKEN:?set HA_API_TOKEN to a long-lived access token}"
ENTITY="${HA_ENTITY_ID:-media_player.moggie_play_5}"
DEVICE="${MOGGIE_DLNA_URL:-http://172.17.0.1:49494}"

pass=0
fail=0

ok()   { printf '  ok   %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  FAIL %s\n     %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

api()  { curl -s -H "Authorization: Bearer $TOKEN" "$HA_URL$1"; }
svc()  { curl -s -X POST "$HA_URL/api/services/media_player/$1" \
              -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
              -d "{\"entity_id\":\"$ENTITY\"${2:+,$2}}" -o /dev/null -w '%{http_code}'; }

attr() { api "/api/states/$ENTITY" | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print(''); raise SystemExit
print(d['attributes'].get('$1',''))
"; }

feat() { api "/api/states/$ENTITY" | python3 -c "
import json,sys
try: print(json.load(sys.stdin)['attributes'].get('supported_features',0))
except Exception: print(0)
"; }

state() { api "/api/states/$ENTITY" | python3 -c "
import json,sys
try: print(json.load(sys.stdin)['state'])
except Exception: print('unavailable')
"; }

# MediaPlayerEntityFeature bits
SUPPORT_VOLUME_SET=4
SUPPORT_VOLUME_MUTE=8
SUPPORT_PLAY=16384

echo "== entity: $ENTITY"

s=$(state)
if [ "$s" = "unavailable" ] || [ -z "$s" ]; then
    bad "entity is available" "state=$s -- is the config entry loaded and the renderer up?"
    echo; echo "$pass passed, $fail failed"; exit 1
fi
ok "entity is available (state=$s)"

f=$(feat)
for bit in $SUPPORT_VOLUME_SET $SUPPORT_VOLUME_MUTE $SUPPORT_PLAY; do
    if [ $(( f & bit )) -ne 0 ]; then
        case $bit in
            $SUPPORT_VOLUME_SET)  name=VOLUME_SET ;;
            $SUPPORT_VOLUME_MUTE) name=VOLUME_MUTE ;;
            $SUPPORT_PLAY)        name=PLAY ;;
        esac
        ok "supports $name"
    else
        bad "supports $name" "supported_features=$f -- check the RenderingControl SCPD"
    fi
done

echo "== volume"
code=$(svc volume_set '"volume_level": 0.80')
[ "$code" = "200" ] && ok "volume_set accepted" || bad "volume_set accepted" "http $code"
sleep 2

# HA's own attribute could be cached; ask the device.
dev_vol=$(curl -s -X POST "$DEVICE/RenderingControl/control" \
    -H 'Content-Type: text/xml; charset="utf-8"' \
    -H 'SOAPACTION: "urn:schemas-upnp-org:service:RenderingControl:1#GetVolume"' \
    -d '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetVolume xmlns:u="urn:schemas-upnp-org:service:RenderingControl:1"><InstanceID>0</InstanceID><Channel>Master</Channel></u:GetVolume></s:Body></s:Envelope>' \
    | grep -oE '<CurrentVolume>[0-9]+' | grep -oE '[0-9]+$')

[ "$dev_vol" = "80" ] && ok "device reports volume 80" \
                      || bad "device reports volume 80" "device said '$dev_vol'"

echo "== transport"
code=$(svc play_media '"media_content_id":"http://172.17.0.1:8000/verify.mp3","media_content_type":"music"')
[ "$code" = "200" ] && ok "play_media accepted" || bad "play_media accepted" "http $code"
sleep 3
s=$(state)
[ "$s" = "playing" ] && ok "state is playing" || bad "state is playing" "state=$s"

# The device must hold the URI, not just HA.
uri=$(curl -s -X POST "$DEVICE/AVTransport/control" \
    -H 'Content-Type: text/xml; charset="utf-8"' \
    -H 'SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo"' \
    -d '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetPositionInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID></u:GetPositionInfo></s:Body></s:Envelope>' \
    | grep -oE '<TrackURI>[^<]*' | sed 's/<TrackURI>//')

case "$uri" in
    *verify.mp3) ok "device holds the URI HA sent" ;;
    *)           bad "device holds the URI HA sent" "device TrackURI='$uri'" ;;
esac

code=$(svc media_pause); [ "$code" = "200" ] && ok "pause accepted" || bad "pause accepted" "http $code"
sleep 2
s=$(state)
[ "$s" = "paused" ] && ok "state is paused" || bad "state is paused" "state=$s"

code=$(svc media_stop); [ "$code" = "200" ] && ok "stop accepted" || bad "stop accepted" "http $code"
sleep 2
s=$(state)
[ "$s" = "idle" ] && ok "state is idle after stop" || bad "state is idle after stop" "state=$s"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
