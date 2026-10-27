#!/usr/bin/env bash
# Verify the host has what the build needs.
set -euo pipefail

missing=0

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "MISSING: $1 ($2)"
        missing=1
    else
        printf 'ok      %-12s %s\n' "$1" "$(command -v "$1")"
    fi
}

echo "Host tools:"
need git        "repo management"
need curl       "downloads"
need tar        "archive extraction"
need xz         "archive extraction"
need make       "build driver"
need sha256sum  "integrity checks"
need python3    "tooling"

echo
if command -v docker >/dev/null 2>&1; then
    printf 'ok      %-12s %s\n' docker "$(command -v docker)"
else
    echo "note    docker not found — containerised builds will be skipped"
fi

echo
if [ "$missing" -ne 0 ]; then
    echo "Some required tools are missing."
    exit 1
fi
echo "All required tools present."
