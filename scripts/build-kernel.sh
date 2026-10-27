#!/usr/bin/env bash
# Build the kernel and audiodev.ko from the Sonos GPL drop.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck disable=SC1091
source ./toolchain.lock

TOOLCHAIN="toolchain/$TOOLCHAIN_DIR/bin"
if [ ! -d "$TOOLCHAIN" ]; then
    echo "Toolchain not found. Run: make toolchain"
    exit 1
fi

KERNEL_DIR=$(find src -maxdepth 2 -type d -name "linux-2.6.35*" | head -1)
if [ -z "$KERNEL_DIR" ]; then
    echo "Kernel sources not found. Run: make sources"
    exit 1
fi

echo "Kernel source: $KERNEL_DIR"
echo
echo "NOT IMPLEMENTED YET (Phase 1)."
echo
echo "Steps this target will perform:"
echo "  1. obtain the stock .config (extract-ikconfig from the shipped kernel image)"
echo "  2. export ARCH=powerpc CROSS_COMPILE=$TOOLCHAIN/powerpc-buildroot-linux-gnu-"
echo "  3. make oldconfig, verifying CONFIG_MTD_CMDLINE_PARTS and CONFIG_SQUASHFS"
echo "  4. make vmlinux zImage"
echo "  5. build drivers/audiodev as an external module"
echo "  6. record sizes, vermagic and SHA-256s in out/artifacts.sha256"
exit 1
