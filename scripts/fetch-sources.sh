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
fetch "common.txz"       "$GPL_BASE/common.txz"
fetch "includes1.txz"    "$GPL_BASE/includes1.txz"
fetch "includes2.txz"    "$GPL_BASE/includes2.txz"
fetch "sonos-kernel.txz" "$GPL_BASE/sonos-kernel.txz"

echo
echo "Contents:"
ls -lh dl/

echo
echo "Extracting drivers..."
tar -xJf "dl/$GPL_DRIVERS" -C src

echo "Extracting kernel..."
tar -xJf "dl/$GPL_KERNEL" -C src

# The kernel includes <mdp.h>, <sonos_*.h> and friends via SONOS_PLATFORMS_INCLUDES.
# Sonos shipped those headers as separate tarballs; merge them into one include dir.
echo "Extracting platform headers..."
mkdir -p src/platforms/includes
for t in includes1 includes2 common; do
    rm -rf "src/.tmp-$t"
    mkdir -p "src/.tmp-$t"
    tar -xJf "dl/$t.txz" -C "src/.tmp-$t"
    cp -r "src/.tmp-$t"/*/* src/platforms/includes/ 2>/dev/null || true
    rm -rf "src/.tmp-$t"
done

echo
echo "Sources ready under src/"
echo "  kernel:           $(ls -d src/linux-2.6.35 2>/dev/null || echo MISSING)"
echo "  drivers:          $(ls -d src/drivers 2>/dev/null || echo MISSING)"
echo "  platform headers: $(find src/platforms/includes -type f 2>/dev/null | wc -l) files"
