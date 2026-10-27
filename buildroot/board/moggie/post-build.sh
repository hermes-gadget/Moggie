#!/bin/sh
#
# Moggie post-build -- runs inside Buildroot's fakeroot, $1 = target dir.
#
# Goals, in order: deterministic ownership/modes, one init entry point,
# no world-writable anything, and a provenance file that says exactly what
# produced this rootfs.

set -eu

TARGET_DIR="$1"

fail() {
    echo "post-build: FAIL: $*" >&2
    exit 1
}

[ -d "$TARGET_DIR" ] || fail "target dir '$TARGET_DIR' does not exist"

# --- ownership and modes --------------------------------------------------
# Post-build scripts run *outside* fakeroot, so a real chown here only works
# if the build happens to run as root. Ownership of the image is applied by
# Buildroot's own fakeroot script (fs/common.mk: "chown -h -R 0:0"), which
# runs after this. Only chown when we actually can.
if [ "$(id -u)" = 0 ]; then
    chown -R 0:0 "$TARGET_DIR"
fi

find "$TARGET_DIR" -xdev -type d -exec chmod 0755 {} +
find "$TARGET_DIR" -xdev -type f -exec chmod go-w {} +

chmod 0700 "$TARGET_DIR/root"
chmod 0600 "$TARGET_DIR/etc/shadow"
chmod 0644 "$TARGET_DIR/etc/passwd" "$TARGET_DIR/etc/group"
chmod 0755 "$TARGET_DIR/etc/init.d/rcS"
chmod 0755 "$TARGET_DIR/usr/share/udhcpc/default.script"

# Buildroot installs busybox 4755 unconditionally, even with
# CONFIG_FEATURE_SUID off. Nothing on this device runs as a non-root user,
# so a setuid-root busybox is pure escalation surface.
chmod 0755 "$TARGET_DIR/bin/busybox"

for d in /tmp /run /var/log /var/tmp /var/lock /jffs; do
    [ -d "$TARGET_DIR$d" ] || mkdir -p "$TARGET_DIR$d"
    chmod 0755 "$TARGET_DIR$d"
done

# --- writable-state indirections -----------------------------------------
# Everything writable lives on tmpfs or jffs2. Dangling symlinks are fine:
# sonosd falls back to its compiled-in defaults when /etc/sonosd.conf is
# not readable.
ln -sfn /run/resolv.conf  "$TARGET_DIR/etc/resolv.conf"
ln -sfn /run              "$TARGET_DIR/var/run"
ln -sfn /jffs/sonosd.conf "$TARGET_DIR/etc/sonosd.conf"

# dropbear's package installs /etc/dropbear as a symlink to
# /var/run/dropbear, which resolves onto the /run tmpfs. That is exactly
# right for a read-only rootfs, so it is left alone.

# --- one init entry point -------------------------------------------------
# rcS starts everything explicitly and conditionally. Package S* scripts
# would start sshd (and anything else) unconditionally, so they are not
# shipped. Nothing in this rootfs needs them.
rm -f "$TARGET_DIR"/etc/init.d/S*

# --- deterministic timestamps --------------------------------------------
# mksquashfs records each inode's mtime. Buildroot's squashfs args do not
# expose -all-time (the COMP_OPTS symbol is promptless), so normalise the
# tree here instead. SOURCE_DATE_EPOCH is exported by Buildroot.
EPOCH="${SOURCE_DATE_EPOCH:-0}"
if [ "$EPOCH" -gt 0 ] 2>/dev/null; then
    find "$TARGET_DIR" -xdev -exec touch -h -d "@$EPOCH" {} +
fi

# --- provenance -----------------------------------------------------------
RELEASE="$TARGET_DIR/etc/moggie-release"
{
    echo "MOGGIE_BUILDROOT=${MOGGIE_BUILDROOT_VERSION:-unknown}"
    echo "MOGGIE_BUILDROOT_SHA256=${MOGGIE_BUILDROOT_SHA256:-unknown}"
    echo "MOGGIE_TOOLCHAIN=${MOGGIE_TOOLCHAIN:-unknown}"
    echo "MOGGIE_SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}"
    echo "MOGGIE_GIT_COMMIT=${MOGGIE_GIT_COMMIT:-unknown}"
    echo "MOGGIE_TARGET=powerpc-e300c3-linux-gnu (big endian, hard float)"
} > "$RELEASE"
chmod 0644 "$RELEASE"

# --- sanity checks --------------------------------------------------------
for f in /sbin/init /bin/busybox /etc/inittab /etc/init.d/rcS /usr/sbin/sonosd; do
    [ -e "$TARGET_DIR$f" ] || fail "missing $f"
done

[ -x "$TARGET_DIR/sbin/init" ] || fail "/sbin/init is not executable"
[ -x "$TARGET_DIR/etc/init.d/rcS" ] || fail "rcS is not executable"

# No world-writable files, directories or device nodes in the image.
# Symlinks are excluded: their mode is always 0777 and carries no meaning.
WW=$(find "$TARGET_DIR" -xdev \
        \( -type f -o -type d -o -type c -o -type b \) -perm -0002 -print)
if [ -n "$WW" ]; then
    echo "$WW" >&2
    fail "world-writable entries in the image"
fi

echo "post-build: ok ($(find "$TARGET_DIR" -xdev | wc -l) entries)"
