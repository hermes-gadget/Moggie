#!/usr/bin/env bash
#
# Build the Moggie root filesystem with Buildroot and measure it.
#
#   scripts/build-rootfs.sh            build (incremental) and measure
#   scripts/build-rootfs.sh --clean    wipe out/buildroot first (full rebuild)
#   scripts/build-rootfs.sh --measure  do not build, just measure what is there
#
# Everything downloaded stays in dl/. The Buildroot tree itself is extracted
# into dl/ and is not committed; out/ is build output.
set -euo pipefail

cd "$(dirname "$0")/.."
REPO=$PWD

# shellcheck disable=SC1091
source ./toolchain.lock
# shellcheck disable=SC1091
source ./buildroot.lock

BR_TARBALL="$REPO/dl/$BUILDROOT_TARBALL"
BR_SRC="$REPO/dl/buildroot-$BUILDROOT_VERSION"
BR2_EXTERNAL="$REPO/buildroot"
DEFCONFIG=moggie_powerpc_defconfig
OUT="$REPO/out"
BUILD="$OUT/buildroot"
JOBS=${JOBS:-$(nproc)}

# Buildroot must not inherit an ambient cross-build environment. This box
# routinely exports toolchain variables for other projects; a stale
# PKG_CONFIG_PATH in particular makes host-pkgconf answer for the wrong
# architecture.
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR \
      CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CC CXX CPP LD AR AS NM RANLIB \
      STRIP OBJCOPY CROSS_COMPILE ARCH
export LC_ALL=C

# This host runs in a user namespace where a real chown(2) to uid 0 returns
# EINVAL. fakeroot normally tries the real syscall first and fails; telling
# it not to try makes it record the fake ownership, which is all the
# squashfs build needs. Without this, target-finalize dies on Buildroot's
# own "chown -h -R 0:0 $TARGET_DIR".
export FAKEROOTDONTTRYCHOWN=1

die() { echo "build-rootfs: error: $*" >&2; exit 1; }
say() { echo "build-rootfs: $*"; }

MODE=build
case "${1:-}" in
    --clean)   MODE=clean ;;
    --measure) MODE=measure ;;
    "")        ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *)         die "unknown argument '$1' (try --help)" ;;
esac

# ---------------------------------------------------------------------------
# 1. Buildroot: download, verify, extract
# ---------------------------------------------------------------------------
verify_buildroot() {
    [ -f "$BR_TARBALL" ] || {
        say "downloading $BUILDROOT_TARBALL"
        curl -fL --retry 3 -o "$BR_TARBALL.part" "$BUILDROOT_URL" \
            || die "download failed: $BUILDROOT_URL"
        mv "$BR_TARBALL.part" "$BR_TARBALL"
    }
    local actual
    actual=$(sha256sum "$BR_TARBALL" | awk '{print $1}')
    if [ "$actual" != "$BUILDROOT_SHA256" ]; then
        die "Buildroot hash mismatch
  expected $BUILDROOT_SHA256
  actual   $actual
  file     $BR_TARBALL"
    fi
    say "Buildroot $BUILDROOT_VERSION sha256 ok ($actual)"
}

ensure_buildroot() {
    verify_buildroot
    if [ ! -d "$BR_SRC" ]; then
        say "extracting $BUILDROOT_TARBALL"
        tar -xf "$BR_TARBALL" -C "$REPO/dl"
    fi
    [ -f "$BR_SRC/Makefile" ] || die "extracted tree looks wrong: $BR_SRC"
    patch_kernel_headers_check
}

# Buildroot offers a "2.6.x" external-toolchain header option but cannot use it:
# BR2_TOOLCHAIN_EXTERNAL_HEADERS_REALLY_OLD selects no
# BR2_TOOLCHAIN_HEADERS_AT_LEAST, so helpers.mk passes an empty version and the
# script emits KERNEL_VERSION(,0,0) -- which does not compile. The toolchain
# genuinely has linux-2.6.35 headers, so the empty value should mean 2.6.
#
# This is the smallest repair that fixes Buildroot's own advertised option. It
# changes nothing unless the version is empty, and it refuses to run twice.
patch_kernel_headers_check() {
    local f="$BR_SRC/support/scripts/check-kernel-headers.sh"
    local before after

    [ -f "$f" ] || die "no check-kernel-headers.sh in $BR_SRC"

    if grep -q 'HDR_VER="${3:-2.6}.0.0"' "$f"; then
        return 0
    fi

    grep -q 'HDR_VER="${3}.0.0"' "$f" \
        || die "check-kernel-headers.sh is not the expected version; refusing to patch"

    before=$(sha256sum "$f" | awk '{print $1}')
    sed -i 's|HDR_VER="${3}\.0\.0"|HDR_VER="${3:-2.6}.0.0"|' "$f"
    after=$(sha256sum "$f" | awk '{print $1}')

    grep -q 'HDR_VER="${3:-2.6}.0.0"' "$f" \
        || die "failed to patch check-kernel-headers.sh"

    say "patched check-kernel-headers.sh for the 2.6.x headers option"
    say "  $before -> $after"
}

# ---------------------------------------------------------------------------
# 2. Toolchain: already-pinned Bootlin external toolchain, never built here
# ---------------------------------------------------------------------------
TC_DIR="${MOGGIE_TOOLCHAIN_DIR:-${TOOLCHAIN26_PATH:-$REPO/toolchain/$TOOLCHAIN_DIR}}"
TC_GCC="$TC_DIR/bin/${TOOLCHAIN_PREFIX}-gcc"

ensure_toolchain() {
    if [ ! -x "$TC_GCC" ]; then
        say "toolchain missing at $TC_DIR"
        say "fetching it (pinned in toolchain.lock)"
        ./scripts/fetch-toolchain.sh || die "toolchain fetch failed"
    fi
    [ -x "$TC_GCC" ] || die "no cross gcc at $TC_GCC"
    local ver sysroot abi
    ver=$("$TC_GCC" -dumpversion)
    sysroot=$("$TC_GCC" -print-sysroot)
    say "toolchain $TC_DIR (gcc $ver)"

    # The whole point of this toolchain is its kernel floor. Read it out of the
    # libc's .note.ABI-tag instead of trusting the path it came from.
    abi=$("${CROSS_READELF:-readelf}" -n "$sysroot/lib/libc.so.6" 2>/dev/null \
          | sed -n 's/.*ABI: \([0-9.]*\).*/\1/p' | head -1)
    say "libc kernel floor: ${abi:-unknown} (target kernel $TARGET_KERNEL)"
    [ -n "$abi" ] || die "could not read the ABI tag from $sysroot/lib/libc.so.6"
    [ "$abi" = "${TOOLCHAIN26_ABI_TAG:-2.6.35}" ] \
        || say "WARNING: libc floor $abi is not ${TOOLCHAIN26_ABI_TAG:-2.6.35}; \
this image will not run on the target kernel" 
}

# ---------------------------------------------------------------------------
# 3. Build
# ---------------------------------------------------------------------------
build() {
    [ "$MODE" = clean ] && { say "removing $BUILD"; rm -rf "$BUILD"; }

    mkdir -p "$OUT" "$REPO/dl"
    export PATH="$TC_DIR/bin:$PATH"
    export SOURCE_DATE_EPOCH
    export MOGGIE_BUILDROOT_VERSION="$BUILDROOT_VERSION"
    export MOGGIE_BUILDROOT_SHA256="$BUILDROOT_SHA256"
    export MOGGIE_TOOLCHAIN="$TOOLCHAIN_DIR"
    # Allow the caller to pin the commit (a bind-mounted build inside a
    # container cannot read the worktree's .git, which points outside).
    if [ -z "${MOGGIE_GIT_COMMIT:-}" ]; then
        MOGGIE_GIT_COMMIT=$(git rev-parse HEAD 2>/dev/null || echo unknown)
    fi
    export MOGGIE_GIT_COMMIT

    local common=(
        -C "$BR_SRC" "O=$BUILD"
        "BR2_EXTERNAL=$BR2_EXTERNAL"
        "BR2_DL_DIR=$REPO/dl"
        "BR2_TOOLCHAIN_EXTERNAL_PATH=$TC_DIR"
        "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
    )

    say "configuring ($DEFCONFIG)"
    make "${common[@]}" "$DEFCONFIG"

    say "building with -j$JOBS"
    make "${common[@]}" -j"$JOBS"
}

# ---------------------------------------------------------------------------
# 4. Measure
# ---------------------------------------------------------------------------
measure() {
    local img="$BUILD/images/rootfs.squashfs"
    [ -f "$img" ] || die "no squashfs at $img (build first)"

    local unsq="$BUILD/host/bin/unsquashfs"
    [ -x "$unsq" ] || unsq=$(command -v unsquashfs) || die "no unsquashfs"

    local raw padded files inodes
    raw=$(stat -c%s "$img")
    # Round up to the NAND eraseblock: the flashable artifact must not straddle
    # an eraseblock boundary, and the padded size is what has to fit the slot.
    padded=$(( (raw + NAND_ERASEBLOCK - 1) / NAND_ERASEBLOCK * NAND_ERASEBLOCK ))

    # Entry/inode counts from the image itself, not from the staging dir.
    files=$("$unsq" -l "$img" | wc -l)
    inodes=$("$unsq" -s "$img" | awk '/^Number of inodes/ {print $NF}')

    local comp blocksize xattrs created
    comp=$("$unsq" -s "$img" | awk '/^Compression/ {print $2}')
    blocksize=$("$unsq" -s "$img" | awk '/^Block size/ {print $3}')
    xattrs=$("$unsq" -s "$img" | awk '/^Number of xattr ids/ {print $NF}')
    created=$("$unsq" -s "$img" | sed -n 's/^Creation or last append time //p')

    # Artifacts: raw image, padded NAND image, sizes report.
    mkdir -p "$OUT"
    cp -f "$img" "$OUT/rootfs.squashfs"
    cp -f "$img" "$OUT/rootfs.nand.img"
    truncate -s "$padded" "$OUT/rootfs.nand.img"

    {
        echo "# Moggie rootfs measurements -- generated by scripts/build-rootfs.sh"
        echo "buildroot_version=$BUILDROOT_VERSION"
        echo "buildroot_sha256=$BUILDROOT_SHA256"
        echo "toolchain=$TOOLCHAIN_DIR"
        echo "source_date_epoch=$SOURCE_DATE_EPOCH"
        echo "compression=$comp"
        echo "block_size=$blocksize"
        echo "xattr_ids=$xattrs"
        echo "image_creation_time=$created"
        echo "raw_bytes=$raw"
        echo "padded_bytes=$padded"
        echo "nand_eraseblock=$NAND_ERASEBLOCK"
        echo "slot_bytes=$ROOTFS_SLOT_BYTES"
        echo "max_bytes=$ROOTFS_MAX_BYTES"
        echo "headroom_raw=$((ROOTFS_MAX_BYTES - raw))"
        echo "headroom_padded=$((ROOTFS_MAX_BYTES - padded))"
        echo "entries=$files"
        echo "inodes=$inodes"
        echo "squashfs_sha256=$(sha256sum "$OUT/rootfs.squashfs" | awk '{print $1}')"
        echo "nand_sha256=$(sha256sum "$OUT/rootfs.nand.img" | awk '{print $1}')"
    } > "$OUT/rootfs.sizes.txt"

    echo
    echo "== squashfs =========================================================="
    printf '  raw            %10d B  (%.2f MiB)\n' "$raw" "$(awk "BEGIN{print $raw/1048576}")"
    printf '  padded 128 KiB %10d B  (%.2f MiB)\n' "$padded" "$(awk "BEGIN{print $padded/1048576}")"
    printf '  budget         %10d B  (%.2f MiB)\n' "$ROOTFS_MAX_BYTES" "$(awk "BEGIN{print $ROOTFS_MAX_BYTES/1048576}")"
    printf '  headroom       %10d B  (raw), %d B (padded)\n' \
        "$((ROOTFS_MAX_BYTES - raw))" "$((ROOTFS_MAX_BYTES - padded))"
    printf '  compression    %s, block size %s, xattr ids %s\n' "$comp" "$blocksize" "$xattrs"
    printf '  image time     %s\n' "$created"
    printf '  entries        %s (%s inodes)\n' "$files" "$inodes"
    printf '  sha256         %s\n' "$(sha256sum "$OUT/rootfs.squashfs" | awk '{print $1}')"
    echo
    echo "  ten largest files (from the staged target dir):"
    find "$BUILD/target" -xdev -type f -printf '%s %p\n' 2>/dev/null \
        | sort -rn | head -10 \
        | awk '{printf "    %8d  %s\n", $1, $2}'
    echo

    local rc=0
    if [ "$raw" -gt "$ROOTFS_MAX_BYTES" ]; then
        echo "FAIL: raw image $raw B exceeds $ROOTFS_MAX_BYTES B" >&2
        rc=1
    fi
    if [ "$padded" -gt "$ROOTFS_MAX_BYTES" ]; then
        echo "FAIL: padded image $padded B exceeds $ROOTFS_MAX_BYTES B" >&2
        rc=1
    fi
    [ "$rc" -eq 0 ] && echo "PASS: fits the slot (padded), $((ROOTFS_MAX_BYTES - padded)) B spare"
    echo "artifacts: out/rootfs.squashfs out/rootfs.nand.img out/rootfs.sizes.txt"
    return "$rc"
}

# ---------------------------------------------------------------------------
case "$MODE" in
    measure)
        measure
        ;;
    *)
        ensure_buildroot
        ensure_toolchain
        build
        measure
        ;;
esac
