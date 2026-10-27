#!/usr/bin/env bash
# Build and run the runtime compatibility probe for the target.
#
# The probe answers a question the compiler cannot: does this libc actually work
# on this kernel? It exercises heap, filesystem, threads, sockets, timing,
# entropy, mmap and signals, and exits non-zero on any failure.
#
# Run it in three places:
#   1. here, under qemu-ppc        -> the architecture and libc load
#   2. the target rootfs, in qemu  -> same libc, emulated kernel
#   3. the real speaker            -> the only result that proves compatibility
#
# Usage:
#   tests/compat/run.sh                 # build + run static and dynamic under qemu
#   tests/compat/run.sh --sysroot DIR   # use a specific sysroot for the dynamic run
#
# Exit: 0 pass, 1 fail, 77 cannot run here.

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

# shellcheck disable=SC1091
source "$REPO/toolchain.lock"

DL="${MOGGIE_DL:-$HOME/Moggie/dl}"
[ -d "$REPO/dl" ] && DL="$REPO/dl"

# Probe the toolchain the image is actually built with. toolchain.lock pins the
# 2.6.35 toolchain (TOOLCHAIN26_PATH); TOOLCHAIN_DIR is the Bootlin fallback,
# which produces a rootfs that cannot run on 2.6.35. Resolving TOOLCHAIN_DIR
# here is how these probes once reported green against the wrong toolchain.
if [ -d "${TOOLCHAIN26_PATH:-/nonexistent}" ] || [ -n "${MOGGIE_TOOLCHAIN26:-}" ]; then
    TOOLROOT="${TOOLCHAIN_ROOT:-$TOOLCHAIN26_PATH}"
    TOOLCHAIN_KIND="2.6.35"
else
    TOOLROOT="${TOOLCHAIN_ROOT:-$REPO/toolchain/$TOOLCHAIN_DIR}"
    [ -d "$TOOLROOT" ] || TOOLROOT="$HOME/Moggie/toolchain/$TOOLCHAIN_DIR"
    TOOLCHAIN_KIND="Bootlin fallback"
    echo "WARNING: the 2.6.35 toolchain was not found; probing the Bootlin fallback." >&2
    echo "         These results do NOT describe the shipped image." >&2
fi

SYSROOT="$TOOLROOT/powerpc-buildroot-linux-gnu/sysroot"
QEMU="$DL/qemu-ppc-static"
WORK="${PROBE_WORK:-$REPO/build/tmp/compat}"
CC="$TOOLROOT/bin/powerpc-buildroot-linux-gnu-gcc"

while [ $# -gt 0 ]; do
    case "$1" in
        --sysroot) SYSROOT="$2"; shift 2 ;;
        *) echo "usage: $0 [--sysroot DIR]" >&2; exit 2 ;;
    esac
done

[ -x "$CC" ] || { echo "  SKIP no cross gcc at $CC"; exit 77; }
[ -x "$QEMU" ] || { echo "  SKIP no qemu at $QEMU"; exit 77; }

rm -rf "$WORK"
mkdir -p "$WORK"

pass=0
fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  FAIL %s\n       %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

echo "compat probe"
echo "toolchain: $TOOLROOT"
echo "sysroot:   $SYSROOT"
echo

# --- static ----------------------------------------------------------------
# Static links everything in, so it runs anywhere including a bare rootfs with
# no dynamic loader. It is also the build that would expose a broken libc at
# startup.

if "$CC" -O2 -Wall -Wextra -static -o "$WORK/probe-static" \
        "$HERE/compat-probe.c" -lpthread >"$WORK/build-static.log" 2>&1; then
    ok "static build"
else
    bad "static build" "see $WORK/build-static.log"
fi

if [ -x "$WORK/probe-static" ]; then
    file "$WORK/probe-static" | grep -q "32-bit MSB" \
        && ok "static binary is 32-bit MSB" \
        || bad "static binary arch" "$(file -b "$WORK/probe-static")"

    out=$("$QEMU" "$WORK/probe-static" 2>&1)
    if echo "$out" | grep -q "compat probe: PASS"; then
        n=$(echo "$out" | grep -oE "[0-9]+ passed, [0-9]+ failed" | head -1)
        ok "static probe under qemu ($n)"
    else
        bad "static probe under qemu" "$(echo "$out" | tail -3)"
    fi
fi

# --- dynamic ---------------------------------------------------------------
# This one is the real test of the toolchain's libc: it loads the actual
# ld.so.1 and libc.so.6 that would ship on the device.

if "$CC" -O2 -Wall -Wextra -o "$WORK/probe-dyn" \
        "$HERE/compat-probe.c" -lpthread >"$WORK/build-dyn.log" 2>&1; then
    ok "dynamic build"
else
    bad "dynamic build" "see $WORK/build-dyn.log"
fi

if [ -x "$WORK/probe-dyn" ] && [ -d "$SYSROOT" ]; then
    out=$("$QEMU" -L "$SYSROOT" "$WORK/probe-dyn" 2>&1)
    if echo "$out" | grep -q "compat probe: PASS"; then
        n=$(echo "$out" | grep -oE "[0-9]+ passed, [0-9]+ failed" | head -1)
        ok "dynamic probe under qemu ($n)"
    else
        bad "dynamic probe under qemu" "$(echo "$out" | tail -3)"
    fi

    # Which libc did it actually load? On a toolchain under test this is the
    # line that matters.
    banner=$(strings -a "$SYSROOT/lib/libc.so.6" 2>/dev/null \
             | grep "GNU C Library" | head -1)
    floor=$(strings -a "$SYSROOT/lib/libc.so.6" 2>/dev/null \
            | grep "Minimum supported kernel" | head -1)
    [ -n "$banner" ] || banner="unknown"
    [ -n "$floor" ] || floor="not declared (check --enable-kernel in the build log)"
    printf '  info libc:   %s\n' "$banner"
    printf '  info floor:  %s\n' "$floor"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "$pass passed, 0 failed"
    echo
    echo "NOTE: qemu runs on the host kernel. A pass here proves the binary and"
    echo "      its libc load and work on this machine — not on Linux 2.6.35."
    echo "      The only definitive result comes from the speaker."
    rm -rf "$WORK"
    exit 0
fi
echo "$pass passed, $fail failed"
echo "artifacts kept in $WORK"
exit 1
