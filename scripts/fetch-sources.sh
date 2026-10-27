#!/usr/bin/env bash
# Fetch the Sonos GPL drop pieces we build against.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck disable=SC1091
source ./toolchain.lock

mkdir -p dl src

fetch() {
    local name="$1" url="$2"
    if [ -f "dl/$name" ]; then
        echo "Using cached dl/$name"
    else
        echo "Downloading $name..."
        curl -fL --progress-bar -o "dl/$name.part" "$url"
        mv "dl/$name.part" "dl/$name"
    fi
}

fetch "$GPL_DRIVERS" "$GPL_BASE/$GPL_DRIVERS"
fetch "$GPL_KERNEL"  "$GPL_BASE/$GPL_KERNEL"

echo
echo "Contents:"
ls -lh dl/

echo
echo "Extracting drivers..."
mkdir -p src
tar -xJf "dl/$GPL_DRIVERS" -C src

echo "Extracting kernel..."
tar -xJf "dl/$GPL_KERNEL" -C src

echo
echo "Sources ready under src/"
