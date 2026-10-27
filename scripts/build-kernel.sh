#!/usr/bin/env bash
# Build the kernel and audiodev.ko from the Sonos GPL drop.
#
# This is not a vanilla kernel build. linux-2.6.35 needs four things to build with
# a modern toolchain, all applied here:
#
#   1. a compiler-gcc15.h alias (the tree only knows up to gcc 5)
#   2. -std=gnu89 -fcommon (GCC 15 defaults are incompatible)
#   3. warnings demoted from errors (GCC 14+ promotes several by default)
#   4. sonos_common.mk, which Sonos never published — reconstructed at the repo root
#
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck disable=SC1091
source ./toolchain.lock

TOOLCHAIN_BIN="toolchain/$TOOLCHAIN_DIR/bin"
export PATH="$PWD/$TOOLCHAIN_BIN:$PATH"
export ARCH=powerpc
export CROSS_COMPILE=powerpc-buildroot-linux-gnu-

KERNEL_DIR="src/linux-2.6.35"
MODULE_DIR="src/drivers/audiodev"
OUT=out

# Flags that make a 2011 kernel build under GCC 15.
KFLAGS="-fcommon -std=gnu89 -w -Wno-error -fpermissive \
-Wno-error=incompatible-pointer-types -Wno-error=int-conversion \
-Wno-error=implicit-function-declaration -Wno-error=return-mismatch \
-Wno-error=implicit-int"

INC="-I$PWD/src/platforms/includes -I$PWD/src/platforms/includes/button_event -I$PWD/src/drivers/include"

die() { echo "FAIL: $*" >&2; exit 1; }

[ -d "$TOOLCHAIN_BIN" ] || die "toolchain missing — run: make toolchain"
[ -d "$KERNEL_DIR" ]    || die "kernel sources missing — run: make sources"
[ -d "$MODULE_DIR" ]    || die "driver sources missing — run: make sources"

mkdir -p "$OUT"

# --- 1. GCC 15 header alias -------------------------------------------------
if [ ! -f "$KERNEL_DIR/include/linux/compiler-gcc15.h" ]; then
    echo "==> adding compiler-gcc15.h alias"
    {
        echo "/* GCC 15 alias for the GCC 5 feature set — added by Moggie's build. */"
        cat "$KERNEL_DIR/include/linux/compiler-gcc5.h"
    } > "$KERNEL_DIR/include/linux/compiler-gcc15.h"
fi

# --- 2. Platform header tree ------------------------------------------------
# The kernel includes <mdp.h> and friends via SONOS_PLATFORMS_INCLUDES.
if [ ! -e src/common ]; then
    echo "==> linking src/common -> platforms/includes"
    ln -s platforms/includes src/common
fi

# button_event.c / button_sim.c are compiled as part of the module but shipped in
# the common tarball rather than in drivers/audiodev/.
for f in button_event.c button_sim.c; do
    if [ ! -f "$MODULE_DIR/$f" ]; then
        cp "src/platforms/includes/button_event/$f" "$MODULE_DIR/$f"
    fi
done

# button_sim.c calls hwevtq_send_event{,,_defer}, which live in a hardware-event
# module Sonos never published. Install the no-op shim so the module links.
if [ ! -f "$MODULE_DIR/compat_hwevtq.c" ]; then
    echo "==> installing compat_hwevtq.c"
    cp patches/audiodev/compat_hwevtq.c "$MODULE_DIR/compat_hwevtq.c"
fi
if ! grep -q "compat_hwevtq.o" "$MODULE_DIR/Kbuild"; then
    echo "==> adding compat_hwevtq.o to the module"
    sed -i 's/button_event\.o button_sim\.o/button_event.o button_sim.o compat_hwevtq.o/' \
        "$MODULE_DIR/Kbuild"
fi

# --- 3. Configure -----------------------------------------------------------
echo "==> fenway_op_defconfig"
make -C "$KERNEL_DIR" fenway_op_defconfig >/dev/null

echo "==> enabling the options Moggie needs"
# -Os matters: the kernel partition is 2 MB and a -O2 build does not fit.
sed -i 's/^# CONFIG_CC_OPTIMIZE_FOR_SIZE is not set/CONFIG_CC_OPTIMIZE_FOR_SIZE=y/' \
    "$KERNEL_DIR/.config"
sed -i 's/^# CONFIG_SQUASHFS is not set/CONFIG_SQUASHFS=y/' "$KERNEL_DIR/.config"
sed -i 's/^# CONFIG_BLK_DEV_INITRD is not set/CONFIG_BLK_DEV_INITRD=y/' \
    "$KERNEL_DIR/.config"
make -C "$KERNEL_DIR" oldconfig </dev/null >/dev/null

for opt in CC_OPTIMIZE_FOR_SIZE SQUASHFS BLK_DEV_INITRD; do
    grep -q "^CONFIG_$opt=y" "$KERNEL_DIR/.config" \
        || die "$opt did not stick in .config"
done

# --- 4. Build ---------------------------------------------------------------
echo "==> building vmlinux"
make -C "$KERNEL_DIR" -j"$(nproc)" KCFLAGS="$KFLAGS" vmlinux

cp "$KERNEL_DIR/vmlinux" "$OUT/vmlinux.elf"
powerpc-buildroot-linux-gnu-strip -o "$OUT/vmlinux.bin" "$OUT/vmlinux.elf"

GZ=$(gzip -9 -c "$OUT/vmlinux.bin" | wc -c)
CAP=$((2 * 1024 * 1024))
[ "$GZ" -lt "$CAP" ] || die "gzip payload $GZ exceeds the 2 MB kernel partition"
echo "    vmlinux.bin $(stat -c%s "$OUT/vmlinux.bin") B; gzip $GZ B of $CAP ($((GZ * 100 / CAP))%)"

echo "==> building audiodev.ko"
make -C "$KERNEL_DIR" M="$PWD/$MODULE_DIR" KCFLAGS="$KFLAGS $INC" modules

cp "$MODULE_DIR/audiodev.ko" "$OUT/audiodev.ko"

# --- 5. Record --------------------------------------------------------------
(
    cd "$OUT"
    sha256sum vmlinux.bin audiodev.ko > artifacts.sha256
)
echo
echo "Artifacts:"
cat "$OUT/artifacts.sha256"
