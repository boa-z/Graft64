#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out"
DERIVED="$OUT/DerivedData"
mkdir -p "$OUT"
"$ROOT/scripts/bootstrap-macos.sh"
xcodebuild -project "$ROOT/app/GraftHost/GraftHost.xcodeproj" -target GraftHost -sdk iphonesimulator -configuration Debug -derivedDataPath "$DERIVED" CODE_SIGNING_ALLOWED=NO build
APP="$DERIVED/Build/Products/Debug-iphonesimulator/GraftHost.app"
test -d "$APP" || { echo "built app not found: $APP" >&2; exit 1; }
rm -rf "$OUT/Payload"
mkdir -p "$OUT/Payload"
cp -R "$APP" "$OUT/Payload/GraftHost.app"
"$ROOT/scripts/package-ipa.sh"
