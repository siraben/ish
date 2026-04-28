#!/usr/bin/env bash
set -euo pipefail

UDID="${UDID:-84DABE0E-DF3E-44E1-9CF5-E3CB9B0E2EC1}"
BUNDLE="${BUNDLE:-app.ish.iSH}"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<EOF
Usage: $(basename "$0")

Environment overrides:
  UDID=<simulator-udid>       default: $UDID
  BUNDLE=<app-bundle-id>      default: $BUNDLE

To find the bundle id:
  xcrun simctl listapps "\$UDID" | grep -i ish
EOF
  exit 0
fi

xcrun simctl boot "$UDID" 2>/dev/null || true
open -a Simulator --args -CurrentDeviceUDID "$UDID"
xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
xcrun simctl launch "$UDID" "$BUNDLE"
