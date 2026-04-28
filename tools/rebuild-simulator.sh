#!/usr/bin/env bash
set -euo pipefail

UDID="${UDID:-84DABE0E-DF3E-44E1-9CF5-E3CB9B0E2EC1}"
BUNDLE="${BUNDLE:-app.ish.iSH}"
SCHEME="${SCHEME:-iSH}"
CONFIGURATION="${CONFIGURATION:-Debug-ApplePleaseFixFB19282108}"
SDK="${SDK:-iphonesimulator}"
PROJECT="${PROJECT:-iSH.xcodeproj}"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<EOF
Usage: $(basename "$0")

Builds iSH for the configured simulator, installs the resulting app, then
launches it.

Environment overrides:
  UDID=<simulator-udid>       default: $UDID
  BUNDLE=<app-bundle-id>      default: $BUNDLE
  SCHEME=<xcode-scheme>       default: $SCHEME
  CONFIGURATION=<config>      default: $CONFIGURATION
  SDK=<xcode-sdk>             default: $SDK
  PROJECT=<xcode-project>     default: $PROJECT
EOF
  exit 0
fi

xcodebuild \
  -project "$PROJECT" \
  -scheme "$SCHEME" \
  -configuration "$CONFIGURATION" \
  -sdk "$SDK" \
  -destination "id=$UDID" \
  build

APP="$(xcodebuild \
  -project "$PROJECT" \
  -scheme "$SCHEME" \
  -configuration "$CONFIGURATION" \
  -sdk "$SDK" \
  -destination "id=$UDID" \
  -showBuildSettings \
  | awk -F ' = ' '
      $1 ~ /^[[:space:]]*TARGET_BUILD_DIR$/ { build_dir = $2 }
      $1 ~ /^[[:space:]]*WRAPPER_NAME$/ { wrapper = $2 }
      END {
        if (build_dir == "" || wrapper == "")
          exit 1
        print build_dir "/" wrapper
      }')"

xcrun simctl boot "$UDID" 2>/dev/null || true
open -a Simulator --args -CurrentDeviceUDID "$UDID"
xcrun simctl install "$UDID" "$APP"
xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
xcrun simctl launch "$UDID" "$BUNDLE"
