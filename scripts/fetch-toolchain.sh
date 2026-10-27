#!/usr/bin/env bash
# Fetch and verify the pinned PowerPC toolchain.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck disable=SC1091
source ./toolchain.lock

mkdir -p dl toolchain
tarball="dl/$(basename "$TOOLCHAIN_URL")"

if [ ! -f "$tarball" ]; then
    echo "Downloading toolchain..."
    curl -fL --progress-bar -o "$tarball.part" "$TOOLCHAIN_URL"
    mv "$tarball.part" "$tarball"
else
    echo "Using cached $tarball"
fi

echo "Verifying SHA-256..."
actual=$(sha256sum "$tarball" | awk '{print $1}')
if [ "$actual" != "$TOOLCHAIN_SHA256" ]; then
    echo "FAIL: toolchain hash mismatch"
    echo "  expected $TOOLCHAIN_SHA256"
    echo "  actual   $actual"
    exit 1
fi
echo "ok: $actual"

if [ ! -d "toolchain/$TOOLCHAIN_DIR" ]; then
    echo "Extracting..."
    tar -xJf "$tarball" -C toolchain
fi

echo "Toolchain ready: toolchain/$TOOLCHAIN_DIR"
