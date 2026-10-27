#!/usr/bin/env bash
# Build the squashfs root filesystem.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck disable=SC1091
source ./toolchain.lock

echo "NOT IMPLEMENTED YET (Phase 2)."
echo
echo "Steps this target will perform:"
echo "  1. Buildroot, powerpc / e300c3, glibc, squashfs output"
echo "  2. packages: busybox, dropbear, mtd-utils, libmosquitto, sonosd, endpoints"
echo "  3. enforce the ${ROOTFS_MAX_BYTES}-byte budget"
echo "  4. emit out/rootfs.squashfs + SHA-256"
exit 1
