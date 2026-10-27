#!/usr/bin/env bash
# Boot chooser tests.
#
# firmware/bootchooser/init is the initramfs PID 1 that decides which half of
# mtd3 to switch_root into. It has a simulation mode (MOGGIE_SIM=1) precisely so
# its decision logic can be tested on a build host, and until now nothing
# exercised it.
#
# What is covered here is *selection*: which slot is chosen, when a slot is
# skipped, when the failure budget is consumed or cleared, and when the chooser
# gives up and drops to recovery.
#
# What is NOT covered, and cannot be here: mounting a real squashfs, switch_root,
# NAND behaviour, or what happens when the kernel actually panics mid-boot.
#
# Exit: 0 pass, 1 fail, 77 cannot run here.

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
CHOOSER="$REPO/firmware/bootchooser/init"

[ -f "$CHOOSER" ] || { echo "  SKIP no boot chooser at $CHOOSER"; exit 77; }

pass=0
fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  FAIL %s\n       %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

SLOT_BYTES=4096

# --- fixture ---------------------------------------------------------------

new_env() {
    T=$(mktemp -d "${TMPDIR:-/tmp}/moggie-boot-XXXXXX")
    mkdir -p "$T/state" "$T/slots" "$T/root"
}

# make_slot <a|b> [magic] [size]
make_slot() {
    _n=$1
    _magic=${2:-hsqs}
    _sz=${3:-$SLOT_BYTES}
    { printf '%s' "$_magic"; head -c $((_sz - 4)) /dev/zero; } > "$T/slots/slot_$_n.img"
    _sum=$(sha256sum "$T/slots/slot_$_n.img" | cut -d' ' -f1)
    printf '%s' "$_sz" > "$T/state/slot_size.$_n"
    printf '%s' "$_sum" > "$T/state/slot_sha256.$_n"
    printf 'ok' > "$T/state/slot_status.$_n"
}

state() { printf '%s' "$2" > "$T/state/$1"; }
read_state() { cat "$T/state/$1" 2>/dev/null | tr -d '\r\n'; }

run_chooser() {
    MOGGIE_SIM=1 \
    MOGGIE_SLOT_DIR="$T/slots" \
    MOGGIE_STATE_DIR="$T/state" \
    MOGGIE_ROOT_MNT="$T/root" \
    MOGGIE_FAIL_LIMIT="${FAIL_LIMIT:-2}" \
    sh "$CHOOSER" >"$T/log" 2>&1
    echo $?
}

booted() { cat "$T/slots/booted" 2>/dev/null | tr -d '\r\n'; }

expect_boot() { # rc slot label
    _rc=$1; _want=$2; _label=$3
    if [ "$_rc" != "0" ]; then
        bad "$_label" "expected a boot, chooser exited $_rc: $(tail -2 "$T/log" | tr '\n' ' ')"
        return
    fi
    _got=$(booted)
    if [ "$_got" = "$_want" ]; then
        ok "$_label -> slot $_want"
    else
        bad "$_label" "expected slot $_want, got '${_got:-none}'"
    fi
}

expect_recovery() { # rc label
    if [ "$1" = "3" ]; then
        ok "$2 -> recovery"
    else
        bad "$2" "expected recovery (exit 3), got exit $1"
    fi
}

echo "boot chooser tests"
echo

# --- cases -----------------------------------------------------------------

# 1. A healthy slot a boots.
new_env
make_slot a
state bootslot a
expect_boot "$(run_chooser)" a "healthy slot a"

# 2. A fresh install with no bootslot defaults to a.
new_env
make_slot a
expect_boot "$(run_chooser)" a "no bootslot defaults to a"

# 3. Corrupt image (hash mismatch) is skipped and b is used.
new_env
make_slot a
make_slot b
state bootslot a
printf 'X' | dd of="$T/slots/slot_a.img" bs=1 seek=100 conv=notrunc 2>/dev/null
expect_boot "$(run_chooser)" b "hash mismatch on a falls back to b"

# 4. Wrong squashfs magic is skipped.
new_env
make_slot a "XXXX"
make_slot b
state bootslot a
expect_boot "$(run_chooser)" b "bad magic on a falls back to b"

# 5. A slot that is not a regular file is skipped.
new_env
make_slot b
state bootslot a
expect_boot "$(run_chooser)" b "missing slot a image falls back to b"

# 6. Failure budget exhausted means the slot is skipped.
new_env
make_slot a
make_slot b
state bootslot a
state bootfail.a 2
expect_boot "$(run_chooser)" b "slot a at the failure limit is skipped"

# 7. Both slots unusable drops to recovery.
new_env
make_slot a "XXXX"
make_slot b "XXXX"
state bootslot a
expect_recovery "$(run_chooser)" "both slots corrupt"

# 8. boot_ok commits the slot: failure count cleared, flag consumed.
new_env
make_slot a
state bootslot a
state bootfail.a 1
state boot_ok a
_rc=$(run_chooser)
expect_boot "$_rc" a "boot_ok commits slot a"
[ "$(read_state bootfail.a)" = "0" ] \
    && ok "boot_ok cleared the failure count" \
    || bad "boot_ok clear" "bootfail.a is '$(read_state bootfail.a)'"
[ -z "$(read_state boot_ok)" ] \
    && ok "boot_ok flag was consumed" \
    || bad "boot_ok consume" "flag still reads '$(read_state boot_ok)'"

# 9. boot_ok for the *other* slot must not commit this one.
new_env
make_slot a
state bootslot a
state bootfail.a 1
state boot_ok b
run_chooser >/dev/null
# One existing failure plus the attempt charged for this boot, and no commit.
[ "$(read_state bootfail.a)" = "2" ] \
    && ok "boot_ok for the other slot does not commit this one" \
    || bad "boot_ok mismatch" "bootfail.a became '$(read_state bootfail.a)', expected 2 (1 + charged attempt)"
[ "$(read_state boot_ok)" = "b" ] \
    && ok "a mismatched boot_ok is left alone, not consumed" \
    || bad "mismatched boot_ok" "flag reads '$(read_state boot_ok)'"

# 10. An attempt is charged before handover, so a hang is counted.
new_env
make_slot a
state bootslot a
run_chooser >/dev/null
[ "$(read_state bootfail.a)" = "1" ] \
    && ok "a boot attempt is charged before switch_root" \
    || bad "attempt accounting" "bootfail.a is '$(read_state bootfail.a)'"

# 11. Failing over moves bootslot, so userspace knows who it is.
new_env
make_slot a "XXXX"
make_slot b
state bootslot a
run_chooser >/dev/null
[ "$(read_state bootslot)" = "b" ] \
    && ok "failover moves bootslot to b" \
    || bad "failover bootslot" "bootslot is '$(read_state bootslot)'"

# 12. A slot marked bad in state is skipped even with a valid image.
new_env
make_slot a
make_slot b
state bootslot a
state slot_status.a bad
expect_boot "$(run_chooser)" b "slot_status=bad is skipped"

# --- shell portability -----------------------------------------------------
# The chooser runs under busybox ash on the device, not bash. If other shells
# are present, the same first case must pass under each.

new_env
make_slot a
state bootslot a
for sh_bin in dash "busybox sh"; do
    case "$sh_bin" in
        "busybox sh") command -v busybox >/dev/null 2>&1 || continue ;;
        *) command -v "${sh_bin%% *}" >/dev/null 2>&1 || continue ;;
    esac
    rm -f "$T/slots/booted"
    MOGGIE_SIM=1 MOGGIE_SLOT_DIR="$T/slots" MOGGIE_STATE_DIR="$T/state" \
    MOGGIE_ROOT_MNT="$T/root" $sh_bin "$CHOOSER" >"$T/log" 2>&1
    _rc=$?
    if [ "$_rc" = "0" ] && [ "$(booted)" = "a" ]; then
        ok "runs under $sh_bin"
    else
        bad "runs under $sh_bin" "exit $_rc: $(tail -1 "$T/log")"
    fi
done

# --- static device table ----------------------------------------------------
# Linux maps MTD character devices as (minor >> 1) = device, (minor & 1) =
# read-only alias. A table that numbers mtd0..mtd5 as minors 0..5 therefore
# points /dev/mtd3 at read-only mtd1 and /dev/mtd4 at mtd2 -- and the updater
# writes "mtd4" and "mtd3" by name. On a device whose mtd2 is the ramdisk that
# is not a failed update, it is a different partition being written.
TABLE="$REPO/buildroot/board/moggie/device_table_dev.txt"
if [ -f "$TABLE" ]; then
    bad_minors=""
    for n in 0 1 2 3 4 5; do
        minor=$(awk -F'\t' -v dev="/dev/mtd$n" '$1==dev && $2=="c" {print $7}' "$TABLE")
        [ "$minor" = "$((n * 2))" ] || bad_minors="$bad_minors mtd$n=$minor(want $((n * 2)))"
    done
    if [ -z "$bad_minors" ]; then
        pass=$((pass + 1)); echo "ok    MTD char minors follow minor = 2 * device"
    else
        fail=$((fail + 1)); echo "FAIL  MTD char minors follow minor = 2 * device:$bad_minors"
    fi
else
    echo "skip  device table not found"
fi

echo
rm -rf "$T" 2>/dev/null
if [ "$fail" -eq 0 ]; then
    echo "$pass passed, 0 failed"
    echo
    echo "NOTE: this covers slot selection only. Mounting real squashfs,"
    echo "      switch_root and NAND behaviour need the device."
    exit 0
fi
echo "$pass passed, $fail failed"
exit 1
