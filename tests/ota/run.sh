#!/usr/bin/env bash
# OTA negative tests: every one of these must fail safely.
set -euo pipefail

echo "NOT IMPLEMENTED YET (Phase 6)."
echo
echo "Cases to implement:"
echo "  1. corrupt image mid-stream      -> abort, inactive slot marked bad"
echo "  2. truncated image               -> abort, size check"
echo "  3. bad signature                 -> abort before any write"
echo "  4. wrong model in manifest       -> abort before any write"
echo "  5. simulated power cut mid-write -> device still boots the old slot"
echo "  6. missing boot_ok after reboot  -> chooser falls back after 2 attempts"
exit 1
