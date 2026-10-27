#!/usr/bin/env bash
# MQTT integration tests: sonosd against a real Mosquitto broker.
#
# This is the test that was missing. The unit tests only ever exercised the mock
# MQTT backend, so "compiles against libmosquitto" was never proof that the
# daemon could actually connect, subscribe, publish or handle a command.
#
# Requirements:
#   - libmosquitto (headers + library). Point MOSQ_PREFIX at a non-system install:
#       MOSQ_PREFIX=$PWD/dl/mosq-host/usr/local tests/mqtt/run.sh
#   - docker, for the broker (image eclipse-mosquitto:2)
#
# Exits 1 on failure, 0 on pass, 77 on "cannot run here" (skipped, reported).

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SRC="$REPO/firmware/sonosd"
WORK="${MQTT_WORK:-$REPO/build/tmp/mqtt-test}"
PORT="${MQTT_TEST_PORT:-18841}"
CONTAINER="moggie-mqtt-test-$$"

MOSQ_PREFIX="${MOSQ_PREFIX:-}"
if [ -n "$MOSQ_PREFIX" ]; then
    export PKG_CONFIG_PATH="$MOSQ_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export LD_LIBRARY_PATH="$MOSQ_PREFIX/lib:${LD_LIBRARY_PATH:-}"
fi

pass=0
fail=0
skip=0

ok()   { printf '  ok   %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  FAIL %s\n       %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }
note() { printf '  ..   %s\n' "$1"; }

skip_all() {
    printf '  SKIP %s\n' "$1"
    printf '\nmqtt: skipped (0 passed, 0 failed)\n'
    exit 77
}

cleanup() {
    docker rm -f "$CONTAINER" >/dev/null 2>&1
    if [ -n "${DAEMON_PID:-}" ]; then
        kill "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

# --- preflight --------------------------------------------------------------

command -v docker >/dev/null 2>&1 || skip_all "docker not available"
pkg-config --exists libmosquitto 2>/dev/null \
    || skip_all "libmosquitto not found (set MOSQ_PREFIX)"

rm -rf "$WORK"
mkdir -p "$WORK"

echo "MQTT integration tests"
echo "work:  $WORK"
echo "broker: eclipse-mosquitto:2 on 127.0.0.1:$PORT"
echo

# --- build with the real backend -------------------------------------------

note "building sonosd against libmosquitto"
if ! ( cd "$SRC" && make clean >/dev/null 2>&1 && make >"$WORK/build.log" 2>&1 ); then
    bad "build" "see $WORK/build.log"
    exit 1
fi
# Check the build log from that run: re-running make when nothing is stale
# prints nothing, and the earlier version of this test failed on exactly that.
grep -q "mqtt: libmosquitto" "$WORK/build.log" \
    && ok "built with the real libmosquitto backend" \
    || bad "backend" "make did not report the libmosquitto backend"

# --- broker -----------------------------------------------------------------

cat > "$WORK/mosquitto.conf" <<'CONF'
listener 1883 0.0.0.0
allow_anonymous true
persistence false
CONF

docker run -d --name "$CONTAINER" \
    -p "127.0.0.1:$PORT:1883" \
    -v "$WORK/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" \
    eclipse-mosquitto:2 >/dev/null 2>&1 || { bad "start broker" "docker run failed"; exit 1; }

for _ in $(seq 1 20); do
    docker exec "$CONTAINER" mosquitto_sub -t '$SYS/broker/uptime' -C 1 -W 1 >/dev/null 2>&1 && break
    sleep 0.5
done
docker exec "$CONTAINER" mosquitto_sub -t '$SYS/broker/uptime' -C 1 -W 1 >/dev/null 2>&1 \
    && ok "broker is up" || { bad "broker" "never became ready"; exit 1; }

mqtt_sub() { timeout 8 docker exec "$CONTAINER" mosquitto_sub "$@" 2>/dev/null; }
mqtt_pub() { docker exec "$CONTAINER" mosquitto_pub "$@" >/dev/null 2>&1; }

# --- daemon -----------------------------------------------------------------

cat > "$WORK/sonosd.conf" <<CONF
device_id = play5
device_name = Moggie Play:5
mqtt_host = 127.0.0.1
mqtt_port = $PORT
mqtt_keepalive = 15
log_level = 3
CONF

"$SRC/sonosd" --mock --config "$WORK/sonosd.conf" >"$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!

for _ in $(seq 1 30); do
    grep -q "sonosd .* ready" "$WORK/daemon.log" 2>/dev/null && break
    kill -0 "$DAEMON_PID" 2>/dev/null || break
    sleep 0.2
done

if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    bad "daemon start" "exited early; see $WORK/daemon.log"
    tail -15 "$WORK/daemon.log"
    exit 1
fi
ok "daemon started with the real backend"
note "$(grep -m1 'mqtt connected' "$WORK/daemon.log" | sed 's/^[^ ]* //')"

# --- assertions -------------------------------------------------------------

AVAIL=$(mqtt_sub -t 'sonos/play5/availability' -C 1 -W 4)
[ "$AVAIL" = "online" ] && ok "availability is online" || bad "availability" "got '$AVAIL'"

DISC=$(mqtt_sub -t 'homeassistant/media_player/play5/config' -C 1 -W 4)
case "$DISC" in
    *'"unique_id":"moggie_play5"'*) ok "discovery document is retained and well-formed" ;;
    *) bad "discovery" "got: ${DISC:0:120}" ;;
esac

# The control surface beyond the player. Each of these must exist *and* be
# backed by something the daemon can actually do.
for ent in number/moggie_play5_volume number/moggie_play5_bass \
           number/moggie_play5_treble number/moggie_play5_balance \
           switch/moggie_play5_loudness switch/moggie_play5_night_mode \
           sensor/moggie_play5_uptime; do
    doc=$(mqtt_sub -t "homeassistant/$ent/config" -C 1 -W 4)
    case "$doc" in
        *'"unique_id"'*) ok "discovery for $ent" ;;
        *) bad "discovery for $ent" "not retained" ;;
    esac
done

UPTIME=$(mqtt_sub -t 'sonos/play5/uptime' -C 1 -W 4)
case "$UPTIME" in
    ''|*[!0-9]*) bad "uptime topic" "expected an integer, got '$UPTIME'" ;;
    *) ok "uptime is published as a bare integer" ;;
esac

mqtt_pub -t 'sonos/play5/cmd' -m '{"volume":42}'
sleep 0.5
STATE=$(mqtt_sub -t 'sonos/play5/state' -C 1 -W 4)
case "$STATE" in
    *'"volume":42'*) ok "single-key command applied" ;;
    *) bad "single-key command" "state: $STATE" ;;
esac

# The regression this test exists for: a multi-key payload used to apply only
# the first recognised key and silently drop the rest.
mqtt_pub -t 'sonos/play5/cmd' -m '{"mute":true,"loudness":true,"night_mode":true}'
sleep 0.5
STATE=$(mqtt_sub -t 'sonos/play5/state' -C 1 -W 4)
case "$STATE" in
    *'"mute":true'*'"loudness":true'*'"night_mode":true'*)
        ok "multi-key command applies every key" ;;
    *) bad "multi-key command" "state: $STATE" ;;
esac

# Tone is applied as one update, so all three must land together.
mqtt_pub -t 'sonos/play5/cmd' -m '{"bass":5,"treble":-3,"balance":2}'
sleep 0.5
STATE=$(mqtt_sub -t 'sonos/play5/state' -C 1 -W 4)
case "$STATE" in
    *'"bass":5'*'"treble":-3'*'"balance":2'*)
        ok "three tone keys applied together" ;;
    *) bad "tone group" "state: $STATE" ;;
esac

mqtt_pub -t 'sonos/play5/cmd' -m '{"bass":99}'
sleep 0.5
STATE=$(mqtt_sub -t 'sonos/play5/state' -C 1 -W 4)
case "$STATE" in
    *'"bass":10'*) ok "out-of-range value clamps instead of being accepted" ;;
    *) bad "clamping" "state: $STATE" ;;
esac

# Home Assistant's switch command_template produces {"loudness": "ON"} — a
# quoted string, not a JSON boolean. That is the payload the real integration
# sends, so it has to work.
mqtt_pub -t 'sonos/play5/cmd' -m '{"loudness": "OFF"}'
sleep 0.5
mqtt_pub -t 'sonos/play5/cmd' -m '{"loudness": "ON"}'
sleep 0.5
STATE=$(mqtt_sub -t 'sonos/play5/state' -C 1 -W 4)
case "$STATE" in
    *'"loudness":true'*) ok "HA-style quoted ON is accepted" ;;
    *) bad "HA switch payload" "state: $STATE" ;;
esac

mqtt_pub -t 'sonos/play5/cmd' -m 'not json at all'
sleep 0.5
kill -0 "$DAEMON_PID" 2>/dev/null \
    && ok "daemon survives an unparseable payload" \
    || bad "robustness" "daemon died on a bad payload"

mqtt_pub -t 'sonos/play5/cmd' -m '{"cmd":"status"}'
sleep 0.5
kill -0 "$DAEMON_PID" 2>/dev/null \
    && ok "daemon survives a command payload" \
    || bad "robustness" "daemon died on cmd payload"

# --- result -----------------------------------------------------------------

echo
if [ "$fail" -eq 0 ]; then
    echo "$pass passed, 0 failed"
    echo "all MQTT integration cases passed"
    rm -rf "$WORK"
    exit 0
fi
echo "$pass passed, $fail failed"
echo "logs kept in $WORK"
exit 1
