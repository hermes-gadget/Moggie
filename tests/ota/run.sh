#!/usr/bin/env bash
# Moggie OTA negative tests.
#
# Every case here is a way an update can go wrong: a corrupt image, a truncated
# transfer, a forged manifest, the wrong speaker model, an image over budget, a
# power cut mid-write, and a slot that boots but never checks in.  In all of
# them the invariant is the same: the running slot keeps booting.
#
# NO MTD DEVICE IS EVER OPENED.  Slots are 8 MiB regions of a 16 MiB regular
# file, and the boot chooser runs in simulation mode against files.
#
#   bash tests/ota/run.sh            # everything
#   OTA_KEEP=1 bash tests/ota/run.sh # keep build/tmp/ota-test for inspection
#
# Exit status: 0 if every case passed, 1 otherwise.

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TOOLS="$REPO/tools"
UPD="$REPO/firmware/updater/build/moggie-updater"
UPD_TEST="$REPO/firmware/updater/build/moggie-updater-test"
CHOOSER="$REPO/firmware/bootchooser/init"

WORK="${OTA_WORK:-$REPO/build/tmp/ota-test}"
LOGS="$WORK/logs"
SLOT_BYTES=8388608
FLASH_BYTES=16777216
MAX_IMAGE=7864320

PASSED=0
FAILED=0
FAILED_NAMES=()

# --- output helpers ---------------------------------------------------------

note() { printf '  ..   %s\n' "$*"; }

assert_eq() { # expected actual description
    if [ "$1" = "$2" ]; then
        printf '  ok   %s\n' "$3"
    else
        printf '  FAIL %s\n       expected: %s\n       actual:   %s\n' "$3" "$1" "$2"
        return 1
    fi
}

assert_ne() { # unexpected actual description
    if [ "$1" != "$2" ]; then
        printf '  ok   %s\n' "$3"
    else
        printf '  FAIL %s (both were %s)\n' "$3" "$1"
        return 1
    fi
}

assert_contains() { # haystack needle description
    case "$1" in
    *"$2"*) printf '  ok   %s\n' "$3" ;;
    *)
        printf '  FAIL %s\n       wanted text: %s\n' "$3" "$2"
        printf '%s\n' "$1" | sed 's/^/       | /'
        return 1
        ;;
    esac
}

assert_exists() { # path description
    if [ -e "$1" ]; then
        printf '  ok   %s\n' "$2"
    else
        printf '  FAIL %s (%s does not exist)\n' "$2" "$1"
        return 1
    fi
}

assert_missing() { # path description
    if [ -e "$1" ]; then
        printf '  FAIL %s (%s should not exist)\n' "$2" "$1"
        return 1
    fi
    printf '  ok   %s\n' "$2"
}

# --- fixture helpers --------------------------------------------------------

image_hash() { sha256sum "$1" | cut -d' ' -f1; }

slot_hash() { # mtd3-file a|b -> sha256 of that whole 8 MiB half
    local skip=0
    [ "$2" = "b" ] && skip=2048
    dd if="$1" bs=4096 skip=$skip count=2048 status=none | sha256sum | cut -d' ' -f1
}

write_slot() { # mtd3-file a|b source-image
    local seek=0
    [ "$2" = "b" ] && seek=2048
    dd if="$3" of="$1" bs=4096 seek=$seek conv=notrunc status=none
}

mk_env() { # dir -> fresh state dir + 16 MiB stand-in for mtd3
    mkdir -p "$1/state" "$1/keys" "$1/img"
    printf 'a\n' > "$1/state/bootslot"
    # The chooser records what it actually booted; the updater refuses to write
    # anything without it. A fixture that sets only bootslot is now modelling a
    # machine that never booted through the chooser, which is exactly the state
    # the updater must refuse.
    printf 'a' > "$1/state/running_slot"
    truncate -s "$FLASH_BYTES" "$1/mtd3.img"
}

mk_key() { # dir
    python3 "$TOOLS/mkmanifest.py" --keygen --key-dir "$1/keys" \
        --pubkey-txt "$1/keys/pubkey.txt" --emit-c-header "$1/keys/pubkey.h" >/dev/null
}

mk_image() { # dir name [payload-bytes] -> path on stdout
    local d=$1 name=$2 bytes=${3:-0}
    local src="$d/img/$name.src"
    mkdir -p "$src"
    printf 'moggie test image %s\n' "$name" > "$src/VERSION"
    if [ "$bytes" -gt 0 ]; then
        head -c "$bytes" /dev/urandom > "$src/payload.bin"
        mksquashfs "$src" "$d/img/$name.squashfs" -noappend -no-progress -quiet \
            -comp gzip -noI -noD -noF -noX >/dev/null
    else
        mksquashfs "$src" "$d/img/$name.squashfs" -noappend -no-progress -quiet \
            -comp gzip >/dev/null
    fi
    printf '%s\n' "$d/img/$name.squashfs"
}

mk_prod() { # dir -> build a production binary with that dir's key embedded
    make -C "$REPO/firmware/updater" BUILD="$1/build" PUBKEY_HEADER="$1/keys/pubkey.h" \
        "$1/build/moggie-updater" >/dev/null
    printf '%s\n' "$1/build/moggie-updater"
}

sign() { # dir image version out [extra mkmanifest args...]
    local d=$1 image=$2 version=$3 out=$4
    shift 4
    python3 "$TOOLS/mkmanifest.py" --image "$image" --version "$version" \
        --key "$d/keys/ota_signing.key" --out "$out" \
        --pubkey "$d/keys/ota_signing.pub.pem" "$@" >/dev/null
}

run_test_updater() { # dir extra args... -> sets OUT and RC
    local d=$1
    shift
    RC=0
    OUT=$("$UPD_TEST" --pubkey-txt "$d/keys/pubkey.txt" --test-mode \
          --target "$d/mtd3.img" --state-dir "$d/state" --no-reboot "$@" 2>&1) || RC=$?
    return 0
}

run_chooser() { # dir (expects dir/slots and dir/state) -> sets OUT, RC, BOOTED
    local d=$1
    : > "$d/slots/booted"
    RC=0
    OUT=$(MOGGIE_SIM=1 MOGGIE_SLOT_DIR="$d/slots" MOGGIE_STATE_DIR="$d/state" \
          sh "$CHOOSER" 2>&1) || RC=$?
    BOOTED=$(cat "$d/slots/booted" 2>/dev/null)
    return 0
}

seed_slots() { # dir image-a [image-b]
    local d=$1 a=$2 b=${3:-}
    mkdir -p "$d/slots"
    cp "$a" "$d/slots/slot_a.img"
    if [ -n "$b" ]; then
        cp "$b" "$d/slots/slot_b.img"
    else
        : > "$d/slots/slot_b.img"
    fi
    local s
    for s in a b; do
        if [ -s "$d/slots/slot_$s.img" ]; then
            stat -c %s "$d/slots/slot_$s.img" > "$d/state/slot_size.$s"
            image_hash "$d/slots/slot_$s.img" > "$d/state/slot_sha256.$s"
            printf 'ok\n' > "$d/state/slot_status.$s"
        fi
    done
}

# --- cases ------------------------------------------------------------------

# 1. The happy path, so the negative cases have something to be negative about.
case_happy_path() {
    set -e
    local d=$WORK/happy
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    write_slot "$d/mtd3.img" a "$img"
    local old_a; old_a=$(slot_hash "$d/mtd3.img" a)

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$img"
    assert_eq 0 "$RC" "update succeeds"
    assert_eq "b" "$(cat "$d/state/bootslot")" "bootslot moved to b"
    assert_eq "ok" "$(cat "$d/state/slot_status.b")" "slot b marked ok"
    assert_eq "$(image_hash "$img")" "$(cat "$d/state/slot_sha256.b")" \
        "recorded hash is the image hash"
    assert_eq "$(stat -c %s "$img")" "$(cat "$d/state/slot_size.b")" \
        "recorded size is the image size"
    assert_eq "1.0.0" "$(cat "$d/state/slot_version.b")" "recorded version"
    assert_eq "$old_a" "$(slot_hash "$d/mtd3.img" a)" "the old slot is byte-identical"

    assert_eq "hsqs" "$(dd if="$d/mtd3.img" bs=4 skip=2097152 count=1 status=none)" \
        "slot b starts with the squashfs magic"

    # The independent Python verifier agrees with the C updater.
    RC=0
    OUT=$(python3 "$TOOLS/verify.py" --manifest "$d/manifest.json" --image "$img" \
          --pubkey "$d/keys/ota_signing.pub.pem" 2>&1) || RC=$?
    assert_eq 0 "$RC" "verify.py agrees the manifest is good"

    # And the chooser picks the new slot, then commits it on the next boot.
    seed_slots "$d" "$d/img/rootfs.squashfs" "$d/img/rootfs.squashfs"
    cp "$d/state/slot_size.b" "$d/state/slot_size.a"
    cp "$d/state/slot_sha256.b" "$d/state/slot_sha256.a"
    run_chooser "$d"
    assert_eq 0 "$RC" "chooser boots after the update"
    assert_eq "b" "$BOOTED" "chooser booted slot b"
    printf 'b\n' > "$d/state/boot_ok"
    run_chooser "$d"
    assert_eq 0 "$RC" "chooser boots a second time"
    assert_eq "b" "$BOOTED" "still slot b"
    assert_eq "0" "$(cat "$d/state/bootfail.b")" "boot_ok reset the failure count"
    assert_eq "" "$(cat "$d/state/boot_ok")" "boot_ok was consumed"
}

# 2. --check verifies without touching anything (production binary).
case_check_only() {
    set -e
    local d=$WORK/check
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    local before; before=$(image_hash "$d/mtd3.img")

    RC=0
    OUT=$("$UPD_TEST" --manifest "$d/manifest.json" --image "$img" \
          --pubkey-txt "$d/keys/pubkey.txt" --check 2>&1) || RC=$?
    assert_eq 0 "$RC" "verifies a good image"
    assert_contains "$OUT" "verified" "it says so"

    cp "$img" "$d/corrupt.squashfs"
    python3 "$HERE/mutate.py" flip --file "$d/corrupt.squashfs" --at middle >/dev/null
    RC=0
    OUT=$("$UPD_TEST" --manifest "$d/manifest.json" --image "$d/corrupt.squashfs" \
          --pubkey-txt "$d/keys/pubkey.txt" --check 2>&1) || RC=$?
    assert_ne 0 "$RC" "--check rejects a corrupt image"
    assert_contains "$OUT" "sha256" "and names the hash mismatch"
    assert_eq "$before" "$(image_hash "$d/mtd3.img")" "nothing was written"
}

# 3. A signature that does not verify: abort before the first byte.
case_bad_signature() {
    set -e
    local d=$WORK/badsig
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    python3 "$HERE/mutate.py" bad-signature --file "$d/manifest.json" --out "$d/bad.json" >/dev/null
    local before; before=$(slot_hash "$d/mtd3.img" a)

    run_test_updater "$d" --manifest "$d/bad.json" --image "$img"
    assert_eq 2 "$RC" "exit code 2 (manifest)"
    assert_contains "$OUT" "signature" "rejected for its signature"
    assert_eq "a" "$(cat "$d/state/bootslot")" "bootslot unchanged"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "target untouched"
    assert_missing "$d/state/slot_sha256.b" "no slot recorded"
    assert_eq "a" "$(cat "$d/state/bootslot")" "still on slot a"
}

# 4. A correctly signed manifest for a different speaker.
case_wrong_model() {
    set -e
    local d=$WORK/model
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json" --model 7
    local before; before=$(slot_hash "$d/mtd3.img" a)

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$img"
    assert_eq 2 "$RC" "exit code 2 (manifest)"
    assert_contains "$OUT" "model 7" "names the wrong model"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "target untouched"
    assert_missing "$d/state/slot_status.b" "the slot was never touched"
}

# 5. The manifest is for the full image, the file on disk is short.
case_truncated_image() {
    set -e
    local d=$WORK/trunc
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    cp "$img" "$d/cut.squashfs"
    local size; size=$(stat -c %s "$img")
    python3 "$HERE/mutate.py" truncate --file "$d/cut.squashfs" --size $((size - 100)) >/dev/null
    local before; before=$(slot_hash "$d/mtd3.img" a)

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$d/cut.squashfs"
    assert_eq 3 "$RC" "exit code 3 (image)"
    assert_contains "$OUT" "truncated" "reports a truncated image"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "target untouched"
    assert_eq "a" "$(cat "$d/state/bootslot")" "bootslot unchanged"
}

# 6. An image over the 7.5 MiB budget: the tool refuses to sign it, and the
#    device refuses it even with a valid signature.
case_oversize_image() {
    set -e
    local d=$WORK/oversize
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    cp "$img" "$d/big.squashfs"
    python3 "$HERE/mutate.py" pad --file "$d/big.squashfs" --size $((MAX_IMAGE + 4096)) >/dev/null
    local big_size; big_size=$(stat -c %s "$d/big.squashfs")
    note "oversize image is $big_size bytes (budget $MAX_IMAGE)"

    RC=0
    OUT=$(python3 "$TOOLS/mkmanifest.py" --image "$d/big.squashfs" --version 1.0.0 \
          --key "$d/keys/ota_signing.key" --out "$d/nope.json" 2>&1) || RC=$?
    assert_ne 0 "$RC" "mkmanifest refuses to sign an oversize image"
    assert_contains "$OUT" "7.5 MiB" "and says which budget was broken"

    sign "$d" "$d/big.squashfs" 1.0.0 "$d/big.json" --force-oversize
    local before; before=$(slot_hash "$d/mtd3.img" a)
    run_test_updater "$d" --manifest "$d/big.json" --image "$d/big.squashfs"
    assert_eq 2 "$RC" "the updater rejects a validly signed oversize manifest"
    assert_contains "$OUT" "outside the allowed range" "and says the size is out of range"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "target untouched"
}

# 7. The image is corrupted after signing: the stream hash catches it.
case_corrupt_image_midstream() {
    set -e
    local d=$WORK/corrupt
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs 262144)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    write_slot "$d/mtd3.img" a "$img"
    local old_a; old_a=$(slot_hash "$d/mtd3.img" a)
    cp "$img" "$d/corrupt.squashfs"
    python3 "$HERE/mutate.py" flip --file "$d/corrupt.squashfs" --at middle >/dev/null

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$d/corrupt.squashfs"
    assert_eq 3 "$RC" "exit code 3 (image)"
    assert_contains "$OUT" "changed while streaming" "caught by the streaming hash"
    assert_eq "bad" "$(cat "$d/state/slot_status.b")" "the target slot is marked bad"
    assert_eq "a" "$(cat "$d/state/bootslot")" "bootslot not moved"
    assert_missing "$d/state/slot_sha256.b" "no hash recorded for the bad slot"
    assert_eq "$old_a" "$(slot_hash "$d/mtd3.img" a)" "the running slot is untouched"

    # The chooser still boots the old slot.
    seed_slots "$d" "$img" ""
    run_chooser "$d"
    assert_eq 0 "$RC" "chooser still boots"
    assert_eq "a" "$BOOTED" "chooser booted the old slot"
}

# 8. A signed field edited after signing.
case_tampered_manifest_field() {
    set -e
    local d=$WORK/tamper
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    python3 "$HERE/mutate.py" tamper --file "$d/manifest.json" --field size \
        --value $(( $(stat -c %s "$img") + 1 )) --out "$d/size.json" >/dev/null
    python3 "$HERE/mutate.py" tamper --file "$d/manifest.json" --field version \
        --value 9.9.9 --out "$d/version.json" >/dev/null
    local before; before=$(slot_hash "$d/mtd3.img" a)

    run_test_updater "$d" --manifest "$d/size.json" --image "$img"
    assert_eq 2 "$RC" "edited size is rejected"
    assert_contains "$OUT" "signature" "because the signature no longer covers it"

    run_test_updater "$d" --manifest "$d/version.json" --image "$img"
    assert_eq 2 "$RC" "edited version is rejected"
    assert_contains "$OUT" "signature" "for the same reason"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "target untouched"

    # The reference verifier agrees on both.
    RC=0
    python3 "$TOOLS/verify.py" --manifest "$d/size.json" --image "$img" \
        --pubkey "$d/keys/ota_signing.pub.pem" --quiet 2>/dev/null || RC=$?
    assert_eq 1 "$RC" "verify.py agrees the edited size is invalid"
}

# 9. A manifest with no signature member at all.
case_missing_signature() {
    set -e
    local d=$WORK/nosig
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    python3 "$HERE/mutate.py" strip-signature --file "$d/manifest.json" --out "$d/nosig.json" >/dev/null

    run_test_updater "$d" --manifest "$d/nosig.json" --image "$img"
    assert_eq 2 "$RC" "an unsigned manifest is refused"
    assert_contains "$OUT" "signature" "and the reason mentions the signature"
    assert_eq "a" "$(cat "$d/state/bootslot")" "bootslot unchanged"
}

# 10. Power cut mid-write: kill -9 the updater and prove the old slot still boots.
case_power_cut_mid_write() {
    set -e
    local d=$WORK/powercut
    mk_env "$d"; mk_key "$d"
    local old; old=$(mk_image "$d" old_rootfs)
    local new; new=$(mk_image "$d" new_rootfs 524288)   # ~512 KiB: 8 chunks
    sign "$d" "$new" 2.0.0 "$d/manifest.json"
    write_slot "$d/mtd3.img" a "$old"
    local old_a; old_a=$(slot_hash "$d/mtd3.img" a)
    local new_size; new_size=$(stat -c %s "$new")
    note "new image is $new_size bytes; 100 ms per 64 KiB write"

    MOGGIE_TEST_WRITE_DELAY_MS=100 "$UPD_TEST" --pubkey-txt "$d/keys/pubkey.txt" \
        --test-mode --target "$d/mtd3.img" --state-dir "$d/state" --no-reboot \
        --manifest "$d/manifest.json" --image "$new" >"$d/powercut.log" 2>&1 &
    local pid=$!
    # Wait until the first chunk has actually landed, then pull the power.  A
    # fixed sleep would be racy on a loaded machine.
    local waited=0
    while [ "$waited" -lt 400 ]; do
        if [ "$(dd if="$d/mtd3.img" bs=4 skip=2097152 count=1 status=none)" = "hsqs" ]; then
            break
        fi
        sleep 0.05
        waited=$((waited + 50))
    done
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    note "killed pid $pid after ${waited}ms, mid-write"

    assert_eq "a" "$(cat "$d/state/bootslot")" "bootslot still points at the old slot"
    assert_eq "$old_a" "$(slot_hash "$d/mtd3.img" a)" "the running slot is byte-identical"
    assert_eq "writing" "$(cat "$d/state/slot_status.b")" "slot b is left marked writing"
    assert_missing "$d/state/slot_sha256.b" "slot b was never committed"
    # Something really was written, and the slot really is incomplete.
    local magic; magic=$(dd if="$d/mtd3.img" bs=4 skip=2097152 count=1 status=none)
    assert_eq "hsqs" "$magic" "the half-written slot starts with the new image"
    local got_hash; got_hash=$(dd if="$d/mtd3.img" bs=4096 skip=2048 count=$(( (new_size + 4095) / 4096 )) status=none | sha256sum | cut -d' ' -f1)
    assert_ne "$(image_hash "$new")" "$got_hash" "but its contents are not the whole image"

    # The device still boots.
    seed_slots "$d" "$old" ""
    run_chooser "$d"
    assert_eq 0 "$RC" "chooser boots after the power cut"
    assert_eq "a" "$BOOTED" "and chooses the old slot"

    # And a clean retry over the half-written slot still works.
    run_test_updater "$d" --manifest "$d/manifest.json" --image "$new"
    assert_eq 0 "$RC" "a retry after the power cut succeeds"
    assert_eq "b" "$(cat "$d/state/bootslot")" "and commits slot b"
}

# 11. The write succeeds but the read-back does not match: the slot must not be
#     made bootable.  (Fault injection stands in for a bad NAND block.)
case_readback_mismatch() {
    set -e
    local d=$WORK/readback
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs 65536)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    local before; before=$(slot_hash "$d/mtd3.img" a)

    export MOGGIE_TEST_READBACK_CORRUPT=1
    run_test_updater "$d" --manifest "$d/manifest.json" --image "$img"
    unset MOGGIE_TEST_READBACK_CORRUPT
    assert_eq 5 "$RC" "exit code 5 (read-back)"
    assert_contains "$OUT" "read-back mismatch" "and says the read-back differed"
    assert_eq "bad" "$(cat "$d/state/slot_status.b")" "slot b marked bad"
    assert_eq "a" "$(cat "$d/state/bootslot")" "bootslot not moved"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "the running slot is untouched"
}

# 12. A slot that boots but never writes boot_ok falls back after two attempts.
case_fallback_without_boot_ok() {
    set -e
    local d=$WORK/fallback
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    seed_slots "$d" "$img" "$img"
    write_slot "$d/mtd3.img" a "$img"

    run_chooser "$d"
    assert_eq 0 "$RC" "boot 1 succeeds"
    assert_eq "a" "$BOOTED" "boot 1 chose slot a"
    assert_eq "1" "$(cat "$d/state/bootfail.a")" "one failed attempt counted"

    run_chooser "$d"
    assert_eq "a" "$BOOTED" "boot 2 still tries slot a (attempt 2 of 2)"
    assert_eq "2" "$(cat "$d/state/bootfail.a")" "second attempt counted"

    run_chooser "$d"
    assert_eq 0 "$RC" "boot 3 succeeds"
    assert_eq "b" "$BOOTED" "boot 3 falls back to slot b"
    assert_eq "b" "$(cat "$d/state/bootslot")" "bootslot follows the fallback"
    assert_eq "bad" "$(cat "$d/state/slot_status.a")" "slot a is marked bad"
}

# 13. boot_ok commits the slot and clears its failure count.
case_boot_ok_commits() {
    set -e
    local d=$WORK/commit
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    seed_slots "$d" "$img" "$img"

    run_chooser "$d"
    assert_eq "1" "$(cat "$d/state/bootfail.a")" "first boot counted as an attempt"
    printf 'a\n' > "$d/state/boot_ok"

    run_chooser "$d"
    assert_eq 0 "$RC" "boot after boot_ok succeeds"
    assert_eq "a" "$BOOTED" "still slot a"
    assert_eq "0" "$(cat "$d/state/bootfail.a")" "the count was cleared"
    assert_eq "" "$(cat "$d/state/boot_ok")" "boot_ok was consumed"
    assert_eq "ok" "$(cat "$d/state/slot_status.a")" "slot a is marked ok"

    # With the count cleared, two more silent boots are needed before fallback.
    run_chooser "$d"
    assert_eq "a" "$BOOTED" "no immediate fallback after a committed boot"
    assert_eq "1" "$(cat "$d/state/bootfail.a")" "count restarts from zero"
}

# 14. The chooser refuses a slot whose bytes do not match, and gives up loudly
#     when neither slot is usable.
case_chooser_slot_integrity() {
    set -e
    local d=$WORK/integrity
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    seed_slots "$d" "$img" "$img"

    # Corrupt slot a's squashfs magic: b must be tried next.
    printf 'XXXX' | dd of="$d/slots/slot_a.img" bs=1 count=4 conv=notrunc status=none
    run_chooser "$d"
    assert_eq 0 "$RC" "boot continues with the other slot"
    assert_eq "b" "$BOOTED" "slot b was chosen"
    assert_contains "$OUT" "no squashfs magic" "and the reason is reported"
    assert_eq "bad" "$(cat "$d/state/slot_status.a")" "slot a marked bad"

    # Now corrupt b's bytes but leave the magic intact: the stored hash catches it.
    printf 'a\n' > "$d/state/bootslot"
    python3 "$HERE/mutate.py" flip --file "$d/slots/slot_b.img" --offset 100 >/dev/null
    run_chooser "$d"
    assert_eq 3 "$RC" "both slots unusable -> recovery"
    assert_contains "$OUT" "recovery" "recovery banner shown"
    assert_contains "$OUT" "does not match" "hash mismatch reported"

    # Both slots blank -> recovery too.
    : > "$d/slots/slot_a.img"
    : > "$d/slots/slot_b.img"
    printf 'a\n' > "$d/state/bootslot"
    printf 'ok\n' > "$d/state/slot_status.a"
    printf 'ok\n' > "$d/state/slot_status.b"
    run_chooser "$d"
    assert_eq 3 "$RC" "two empty slots -> recovery"
    assert_eq "" "$BOOTED" "nothing was booted"
}

# 15. The updater refuses to write the slot it is running from.
case_refuses_running_slot() {
    set -e
    local d=$WORK/running
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    local before; before=$(slot_hash "$d/mtd3.img" a)

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$img" --slot a
    assert_eq 1 "$RC" "exit code 1 (usage)"
    assert_contains "$OUT" "running slot" "explains that a is running"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "nothing was written"
}

# 16. The production build never writes a regular file, even when asked nicely.
case_production_refuses_regular_file() {
    set -e
    local d=$WORK/prodguard
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    local before; before=$(slot_hash "$d/mtd3.img" a)
    local prod; prod=$(mk_prod "$d")

    RC=0
    OUT=$("$prod" --manifest "$d/manifest.json" --image "$img" --state-dir "$d/state" \
          --device "$d/mtd3.img" --no-reboot 2>&1) || RC=$?
    assert_eq 4 "$RC" "exit code 4 (device)"
    assert_contains "$OUT" "only the rootfs device" "refuses a regular-file device"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "the file was not written"

    RC=0
    OUT=$("$prod" --manifest "$d/manifest.json" --image "$img" --test-mode \
          --target "$d/mtd3.img" --state-dir "$d/state" 2>&1) || RC=$?
    assert_ne 0 "$RC" "the production build rejects --test-mode outright"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "still nothing written"
}

# 17. mtd0 (U-Boot) is not a legal target under any circumstances.
case_refuses_mtd0() {
    set -e
    local d=$WORK/mtd0
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    local prod; prod=$(mk_prod "$d")

    RC=0
    OUT=$("$prod" --manifest "$d/manifest.json" --image "$img" --state-dir "$d/state" \
          --device /dev/mtd0 --no-reboot 2>&1) || RC=$?
    assert_eq 4 "$RC" "exit code 4 (device)"
    assert_contains "$OUT" "/dev/mtd0" "the refusal names the device"
    assert_contains "$OUT" "only the rootfs device" "and explains the rule"
}

# 18. A production build with a key compiled in, no side-channel key file.
case_embedded_key() {
    set -e
    local d=$WORK/embed
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"

    local prod; prod=$(mk_prod "$d")
    assert_exists "$prod" "built a binary with the test key embedded"

    RC=0
    OUT=$("$prod" --manifest "$d/manifest.json" --image "$img" \
          --check 2>&1) || RC=$?
    assert_eq 0 "$RC" "the embedded key verifies the manifest"

    # A different key must not verify against it.
    python3 "$TOOLS/mkmanifest.py" --keygen --key-dir "$d/other" >/dev/null
    python3 "$TOOLS/mkmanifest.py" --image "$img" --version 1.0.0 \
        --key "$d/other/ota_signing.key" --out "$d/other.json" >/dev/null
    RC=0
    OUT=$("$prod" --manifest "$d/other.json" --image "$img" \
          --check 2>&1) || RC=$?
    assert_eq 2 "$RC" "a manifest signed by another key is refused"
    assert_contains "$OUT" "does not verify" "for the right reason"
}

# 19. End to end: install into b, boot b, check in, stay on b.
case_install_boot_commit() {
    set -e
    local d=$WORK/e2e
    mk_env "$d"; mk_key "$d"
    local old; old=$(mk_image "$d" old)
    local new; new=$(mk_image "$d" new 131072)
    sign "$d" "$new" 2.0.0 "$d/manifest.json"
    write_slot "$d/mtd3.img" a "$old"

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$new"
    assert_eq 0 "$RC" "install into the inactive slot"
    assert_eq "b" "$(cat "$d/state/bootslot")" "bootslot is now b"

    mkdir -p "$d/slots"
    dd if="$d/mtd3.img" of="$d/slots/slot_a.img" bs=4096 count=2048 status=none
    dd if="$d/mtd3.img" of="$d/slots/slot_b.img" bs=4096 skip=2048 count=2048 status=none
    cp "$d/state/slot_size.b" "$d/state/slot_size.a"
    cp "$d/state/slot_sha256.b" "$d/state/slot_sha256.a"

    run_chooser "$d"
    assert_eq 0 "$RC" "the chooser boots"
    assert_eq "b" "$BOOTED" "it boots the freshly installed slot"
    assert_eq "1" "$(cat "$d/state/bootfail.b")" "the attempt is counted before handover"

    printf 'b\n' > "$d/state/boot_ok"
    run_chooser "$d"
    assert_eq "b" "$BOOTED" "the next boot stays on b"
    assert_eq "0" "$(cat "$d/state/bootfail.b")" "and commits it"

    # A second update from b must target a.
    local newer; newer=$(mk_image "$d" newer 65536)
    sign "$d" "$newer" 2.1.0 "$d/manifest2.json"
    run_test_updater "$d" --manifest "$d/manifest2.json" --image "$newer"
    assert_eq 0 "$RC" "the next update succeeds"
    assert_eq "a" "$(cat "$d/state/bootslot")" "and alternates back to slot a"
}

# 20. The chooser must behave the same under every shell it may meet on the
#     device: dash now, busybox ash in the initramfs.
# The updater used to read `bootslot` as "the slot that is running". bootslot
# actually means "boot this one next" and is rewritten at commit time, so after
# installing B -- before any reboot -- a second invocation believed it was
# running B and targeted A, which was still the live rootfs.
case_second_update_before_reboot() {
    set -e
    local d=$WORK/reboot
    mk_env "$d"; mk_key "$d"
    local old; old=$(mk_image "$d" old)
    local new; new=$(mk_image "$d" new 131072)
    sign "$d" "$new" 2.0.0 "$d/manifest.json"
    write_slot "$d/mtd3.img" a "$old"

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$new"
    assert_eq 0 "$RC" "first install succeeds"
    assert_eq "b" "$(cat "$d/state/bootslot")" "bootslot now points at b"
    assert_eq "a" "$(cat "$d/state/running_slot")" "but a is still the running slot"

    dd if="$d/mtd3.img" bs=4096 count=2048 status=none of="$d/slot_a.before"
    run_test_updater "$d" --manifest "$d/manifest.json" --image "$new"
    dd if="$d/mtd3.img" bs=4096 count=2048 status=none of="$d/slot_a.after"

    if cmp -s "$d/slot_a.before" "$d/slot_a.after"; then same=yes; else same=no; fi
    assert_eq "yes" "$same" "the running slot is byte-identical after a second update"
    assert_eq "a" "$(cat "$d/state/running_slot")" "running slot still a"
}

case_chooser_shells() {
    set -e
    local d=$WORK/shells
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)

    local sh_list="sh"
    command -v busybox >/dev/null 2>&1 && sh_list="$sh_list busybox-sh"
    command -v dash >/dev/null 2>&1 && sh_list="$sh_list dash"

    local shell
    for shell in $sh_list; do
        local e="$d/$shell"
        mk_env "$e"
        seed_slots "$e" "$img" "$img"
        : > "$e/slots/booted"
        RC=0
        case "$shell" in
        busybox-sh)
            OUT=$(MOGGIE_SIM=1 MOGGIE_SLOT_DIR="$e/slots" MOGGIE_STATE_DIR="$e/state" \
                  busybox sh "$CHOOSER" 2>&1) || RC=$?
            ;;
        *)
            OUT=$(MOGGIE_SIM=1 MOGGIE_SLOT_DIR="$e/slots" MOGGIE_STATE_DIR="$e/state" \
                  "$shell" "$CHOOSER" 2>&1) || RC=$?
            ;;
        esac
        assert_eq 0 "$RC" "chooser runs under $shell"
        assert_eq "a" "$(cat "$e/slots/booted")" "$shell boots slot a"
    done
}

# 21. Two manifests with a *valid* signature that must still be refused: a
#     re-ordered body and a duplicated field.  If either got through, the bytes
#     that were signed and the bytes that were used could differ.
case_manifest_shape() {
    set -e
    local d=$WORK/shape
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"

    python3 "$HERE/mutate.py" reorder --file "$d/manifest.json" \
        --key "$d/keys/ota_signing.key" --out "$d/reordered.json" >/dev/null
    python3 "$HERE/mutate.py" duplicate --file "$d/manifest.json" \
        --key "$d/keys/ota_signing.key" --field size --out "$d/duplicate.json" >/dev/null

    RC=0
    OUT=$(python3 "$TOOLS/verify.py" --manifest "$d/reordered.json" --image "$img" \
          --pubkey "$d/keys/ota_signing.pub.pem" 2>&1) || RC=$?
    assert_eq 1 "$RC" "verify.py rejects a validly signed re-ordered body"
    assert_contains "$OUT" "canonical" "because it is not canonical"

    run_test_updater "$d" --manifest "$d/reordered.json" --image "$img"
    assert_eq 2 "$RC" "the updater rejects it too"
    assert_contains "$OUT" "canonical" "for the same reason"

    run_test_updater "$d" --manifest "$d/duplicate.json" --image "$img"
    assert_eq 2 "$RC" "a duplicated field is rejected"
    assert_eq "a" "$(cat "$d/state/bootslot")" "nothing was installed by either"
}

# 22. A signed file that is not a squashfs at all (right size, right hash,
#     wrong magic): the updater must not put it in a slot.
case_bad_magic() {
    set -e
    local d=$WORK/magic
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    local size; size=$(stat -c %s "$img")
    { printf '\177ELF'; dd if=/dev/zero bs=1 count=$((size - 4)) status=none; } > "$d/notrootfs.bin"
    sign "$d" "$d/notrootfs.bin" 1.0.0 "$d/manifest.json"
    local before; before=$(slot_hash "$d/mtd3.img" a)

    run_test_updater "$d" --manifest "$d/manifest.json" --image "$d/notrootfs.bin"
    assert_eq 3 "$RC" "exit code 3 (image)"
    assert_contains "$OUT" "squashfs magic" "rejected for its magic"
    assert_eq "$before" "$(slot_hash "$d/mtd3.img" a)" "nothing was written"
}

# 23. Forty mutated manifests: the verifier must always say no politely.  A
#     crash here would be a parser bug reachable from unauthenticated input,
#     because the manifest is the first thing an update touches.
case_fuzz_manifest() {
    set -e
    local d=$WORK/fuzz
    mk_env "$d"; mk_key "$d"
    local img; img=$(mk_image "$d" rootfs)
    sign "$d" "$img" 1.0.0 "$d/manifest.json"
    python3 "$HERE/mutate.py" fuzz --file "$d/manifest.json" \
        --out-dir "$d/mutants" --count 40 --seed 7 >/dev/null

    local m count=0 bad=0
    for m in "$d"/mutants/mut_*.json; do
        run_test_updater "$d" --manifest "$m" --image "$img" --check
        count=$((count + 1))
        if [ "$RC" -ne 0 ] && [ "$RC" -ne 2 ]; then
            note "$(basename "$m") exited $RC"
            bad=$((bad + 1))
        fi
    done
    assert_eq "40" "$count" "all 40 mutants were tried"
    assert_eq "0" "$bad" "no mutant crashed or was accepted for the wrong reason"
    assert_eq "a" "$(cat "$d/state/bootslot")" "no mutant changed the boot slot"
}

# --- runner -----------------------------------------------------------------

run_case() {
    local name=$1
    shift
    local out rc
    out=$("$@" 2>&1)
    rc=$?
    printf '%s\n' "$out" > "$LOGS/$name.log"
    if [ $rc -eq 0 ]; then
        printf 'PASS  %s\n' "$name"
        PASSED=$((PASSED + 1))
    else
        printf 'FAIL  %s\n' "$name"
        printf '%s\n' "$out" | sed 's/^/      /'
        printf '      (full log: %s)\n' "${LOGS#"$REPO"/}/$name.log"
        FAILED=$((FAILED + 1))
        FAILED_NAMES+=("$name")
    fi
}

preflight() {
    local missing=0
    for tool in python3 openssl mksquashfs sha256sum dd truncate make cc; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "MISSING: $tool" >&2
            missing=1
        fi
    done
    [ "$missing" -eq 0 ] || return 1

    echo "building the updater"
    make -C "$REPO/firmware/updater" >/dev/null || {
        echo "build failed" >&2
        return 1
    }
    for binary in "$UPD" "$UPD_TEST"; do
        [ -x "$binary" ] || { echo "missing $binary" >&2; return 1; }
    done
    [ -x "$CHOOSER" ] || { echo "boot chooser is not executable" >&2; return 1; }
    return 0
}

main() {
    echo "Moggie OTA test suite"
    echo "repo:  $REPO"
    echo "work:  $WORK"
    echo

    preflight || exit 1

    rm -rf "$WORK"
    mkdir -p "$LOGS"

    local cases
    cases=$(declare -F | awk '{print $3}' | grep '^case_' | sort)

    local name
    for name in $cases; do
        run_case "${name#case_}" "$name"
    done

    echo
    echo "-------------------------------------------"
    printf '%d passed, %d failed\n' "$PASSED" "$FAILED"
    if [ "$FAILED" -ne 0 ]; then
        printf 'failed cases: %s\n' "${FAILED_NAMES[*]}"
        echo "artifacts left in $WORK"
        return 1
    fi
    echo "all OTA cases passed"
    if [ "${OTA_KEEP:-0}" != "1" ]; then
        rm -rf "$WORK"
    else
        echo "kept $WORK (OTA_KEEP=1)"
    fi
    return 0
}

main "$@"
