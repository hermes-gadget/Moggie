#!/usr/bin/env bash
# Run the PowerPC binaries on this machine, in qemu user-mode emulation.
#
# This is the step between "it cross-compiles" and "it runs on hardware". A
# cross-compiled ELF that links is not evidence that it executes: unaligned
# access, endianness bugs, ABI mismatches and missing syscalls all show up only
# when the binary actually runs. qemu-user gives us that without a speaker.
#
# qemu-ppc-static comes from the multiarch/qemu-user-static container (this host
# has docker but no root to install a package). It is cached in dl/.
#
# Usage: tests/emulated/run.sh
# Exit: 0 pass, 1 fail, 77 cannot run here.

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

# shellcheck disable=SC1091
source "$REPO/toolchain.lock"

# dl/ is gitignored, so a worktree does not have one. The main checkout is the
# shared download cache.
DL="${MOGGIE_DL:-$HOME/Moggie/dl}"
[ -d "$REPO/dl" ] && DL="$REPO/dl"

QEMU="$DL/qemu-ppc-static"

# Same for the toolchain: it is gitignored, so a fresh worktree only has it if
# the main checkout does.
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
SRC="$REPO/firmware/sonosd"
WORK="${EMU_WORK:-$REPO/build/tmp/emu-ppc}"

pass=0
fail=0

ok()  { printf '  ok   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  FAIL %s\n       %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }
note(){ printf '  ..   %s\n' "$1"; }

echo "PowerPC emulation tests"
echo "work: $WORK"
echo

[ -d "$SYSROOT" ] || { echo "  SKIP toolchain sysroot missing"; exit 77; }

# --- qemu -------------------------------------------------------------------

if [ ! -x "$QEMU" ]; then
    note "qemu-ppc-static not present, extracting from multiarch/qemu-user-static"
    command -v docker >/dev/null 2>&1 || { echo "  SKIP no docker, cannot fetch qemu"; exit 77; }
    CID=$(docker create multiarch/qemu-user-static:latest 2>/dev/null)
    docker cp "$CID":/usr/bin/qemu-ppc-static "$QEMU" >/dev/null 2>&1
    docker rm -f "$CID" >/dev/null 2>&1
    chmod +x "$QEMU" 2>/dev/null
fi

[ -x "$QEMU" ] || { echo "  SKIP qemu-ppc-static unavailable"; exit 77; }
ok "qemu-ppc: $("$QEMU" --version | head -1)"

# --- build for the target ---------------------------------------------------

rm -rf "$WORK"
mkdir -p "$WORK"

export PATH="$TOOLROOT/bin:$PATH"

MOSQ_PPC="$REPO/dl/mosq-ppc/usr"
BUILD_ENV=(env -u CROSS_COMPILE -u ARCH PATH="$PATH")
if [ ! -d "$MOSQ_PPC/lib/pkgconfig" ]; then
    note "building libmosquitto for PowerPC into dl/ (once per worktree)"
    "$REPO/scripts/build-mosquitto.sh" ppc >"$WORK/mosq-build.log" 2>&1 \
        || { bad "libmosquitto for ppc" "see $WORK/mosq-build.log"; exit 1; }
fi
if [ -d "$MOSQ_PPC/lib/pkgconfig" ]; then
    BUILD_ENV+=(PKG_CONFIG_PATH="$MOSQ_PPC/lib/pkgconfig")
    note "linking the PowerPC libmosquitto from dl/mosq-ppc"
fi

( cd "$SRC" && "${BUILD_ENV[@]}" make clean >/dev/null 2>&1 )
if ! ( cd "$SRC" && "${BUILD_ENV[@]}" make CROSS_COMPILE=powerpc-buildroot-linux-gnu- \
        >"$WORK/build.log" 2>&1 ); then
    bad "cross build" "see $WORK/build.log"
    exit 1
fi
ok "cross build succeeded"

cp "$SRC/sonosd" "$WORK/sonosd"
[ -f "$MOSQ_PPC/lib/libmosquitto.so.1" ] && cp "$MOSQ_PPC/lib/libmosquitto.so.1" "$WORK/"

file "$WORK/sonosd" | grep -q "32-bit MSB.*PowerPC" \
    && ok "binary is 32-bit MSB PowerPC" \
    || bad "binary arch" "$(file -b "$WORK/sonosd")"

# --- run it -----------------------------------------------------------------

run_ppc() {
    ( cd "$WORK" && env LD_LIBRARY_PATH=. "$QEMU" -L "$SYSROOT" "$@" 2>&1 )
}

out=$(run_ppc ./sonosd --version)
case "$out" in
    *"sonosd"*"(audio backend: linux)"*) ok "--version executes on PowerPC" ;;
    *) bad "--version" "$out" ;;
esac

out=$(run_ppc ./sonosd --selftest)
if echo "$out" | grep -q "selftest: PASS"; then
    n=$(echo "$out" | grep -oE "selftest: [0-9]+ checks" | head -1)
    ok "--selftest passes under emulation ($n)"
else
    bad "--selftest" "$(echo "$out" | tail -3)"
fi

# The daemon must also survive a config it does not understand, on the target
# ABI — a broken strtol or a signed-char assumption would show here.
printf 'device_id = play5\nlog_level = 3\nnonsense_key = 1\n' > "$WORK/bad.conf"
out=$(run_ppc ./sonosd --mock --config "$WORK/bad.conf" --once)
if echo "$out" | grep -qi "unknown"; then
    ok "rejects an unknown config key on the target ABI"
else
    bad "config validation under emulation" "$(echo "$out" | tail -2)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "$pass passed, 0 failed"
    echo "PowerPC binaries execute correctly under qemu-user"
    rm -rf "$WORK"
    exit 0
fi
echo "$pass passed, $fail failed"
echo "artifacts kept in $WORK"
exit 1
