#!/usr/bin/env bash
# Build libmosquitto into the repo's dl/ tree.
#
# Why: sonosd's real MQTT backend was never compiled against the actual library
# because there is no system-wide libmosquitto on the build host and no root to
# install one. This builds the client library only (no broker, no apps, no TLS)
# into dl/ so both the host tests and the target build can link it.
#
#   ./scripts/build-mosquitto.sh host    # x86-64, for tests/mqtt
#   ./scripts/build-mosquitto.sh ppc     # PowerPC, for the device build
#
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION=2.0.20
SHA256=ebd07d89d2a446a7f74100ad51272e4a8bf300b61634a7812e19f068f2759de8
URL="https://mosquitto.org/files/source/mosquitto-$VERSION.tar.gz"

TARGET="${1:-host}"
case "$TARGET" in
    host) PREFIX="$PWD/dl/mosq-host/usr" ;;
    ppc)  PREFIX="$PWD/dl/mosq-ppc/usr" ;;
    *)    echo "usage: $0 [host|ppc]" >&2; exit 2 ;;
esac

SRC="dl/mosquitto-$VERSION"
TARBALL="dl/mosquitto-$VERSION.tar.gz"

mkdir -p dl

if [ ! -f "$TARBALL" ]; then
    echo "==> downloading mosquitto $VERSION"
    curl -fsSL --max-time 180 -o "$TARBALL" "$URL"
fi

echo "==> verifying tarball"
echo "$SHA256  $TARBALL" | sha256sum -c - >/dev/null \
    || { echo "FAIL: checksum mismatch for $TARBALL" >&2; exit 1; }

[ -d "$SRC" ] || tar xzf "$TARBALL" -C dl

make -C "$SRC/lib" clean >/dev/null 2>&1 || true

if [ "$TARGET" = ppc ]; then
    # shellcheck disable=SC1091
    source ./toolchain.lock
    export PATH="$PWD/toolchain/$TOOLCHAIN_DIR/bin:$PATH"
    # The mosquitto Makefile derives CC by prefixing CROSS_COMPILE, so setting
    # CROSS_COMPILE and CC both results in "<prefix>-<prefix>-gcc" and fails.
    BUILD_ENV=(env CROSS_COMPILE=powerpc-buildroot-linux-gnu-)
    CC_DESC=powerpc-buildroot-linux-gnu-cc
else
    BUILD_ENV=(env -u CROSS_COMPILE -u ARCH CC=gcc CXX=g++)
    CC_DESC=gcc
fi

echo "==> building libmosquitto for $TARGET with $CC_DESC"
"${BUILD_ENV[@]}" make -C "$SRC/lib" \
    WITH_TLS=no WITH_SRV=no WITH_WEBSOCKETS=no \
    -j"$(nproc)" >/dev/null

echo "==> installing to $PREFIX"
"${BUILD_ENV[@]}" make -C "$SRC/lib" install \
    WITH_TLS=no prefix="$PREFIX" >/dev/null

LIB="$PREFIX/lib/libmosquitto.so.1"
file "$LIB" | sed 's/^/    /'
echo
echo "libmosquitto $VERSION built for $TARGET"
echo "  headers: $PREFIX/include"
echo "  library: $PREFIX/lib"
echo
case "$TARGET" in
    host) echo "Now run: make mqtt-test" ;;
    ppc)  echo "Link the device build with -I$PREFIX/include -L$PREFIX/lib -lmosquitto" ;;
esac
