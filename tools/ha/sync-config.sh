#!/usr/bin/env bash
# Copy the repository's Home Assistant configuration into the running rig and
# restart Home Assistant so it is picked up.
#
# Home Assistant's /config is a named volume (see compose.yaml) because
# userns-remapped Docker cannot write to a bind mount owned by the invoking user.
# The repository copy is authoritative: edit tools/ha/ha-config/, run this, done.
#
# Safe to re-run. It never touches .storage, the recorder database or anything
# else Home Assistant created — only the files this repository owns.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

if ! docker compose ps --status running --services 2>/dev/null | grep -qx homeassistant; then
    echo "homeassistant is not running; starting the rig first" >&2
    docker compose up -d
    echo "waiting for the Home Assistant API..." >&2
    for _ in $(seq 1 60); do
        if docker compose exec -T homeassistant python3 -c \
            "import socket; socket.create_connection(('127.0.0.1', 8123), 2).close()" 2>/dev/null; then
            break
        fi
        sleep 2
    done
fi

echo "copying configuration.yaml and packages/ into moggie-ha:/config"
docker compose cp ha-config/configuration.yaml homeassistant:/config/configuration.yaml
docker compose exec -T homeassistant mkdir -p /config/packages
docker compose cp ha-config/packages/. homeassistant:/config/packages/

echo "restarting homeassistant"
docker compose restart homeassistant >/dev/null

for _ in $(seq 1 60); do
    if docker compose exec -T homeassistant python3 -c \
        "import socket; socket.create_connection(('127.0.0.1', 8123), 2).close()" 2>/dev/null; then
        echo "homeassistant is up: http://localhost:${MOGGIE_HA_PORT:-18123}"
        exit 0
    fi
    sleep 2
done

echo "homeassistant did not come back within 2 minutes; check:" >&2
echo "  docker compose logs homeassistant" >&2
exit 1
