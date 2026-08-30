#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out"
IPA="$OUT/GraftHost.ipa"
test -f "$IPA" || { echo "missing $IPA" >&2; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -q "$IPA" -d "$TMP"
APP="$TMP/Payload/GraftHost.app"
test -x "$APP/GraftHost" || { echo "main executable missing or not executable" >&2; exit 1; }
test -x "$APP/GraftProbeHelper" || { echo "helper missing or not executable" >&2; exit 1; }
test -f "$APP/GraftProbeTest.dylib" || { echo "test dylib missing" >&2; exit 1; }
test -f "$APP/Info.plist" || { echo "Info.plist missing" >&2; exit 1; }
plutil -lint "$APP/Info.plist" >/dev/null
file "$APP/GraftHost"
otool -hv "$APP/GraftHost" | head -3
for binary in "$APP/GraftProbeHelper" "$APP/GraftProbeTest.dylib"; do
  file "$binary" | grep -q 'arm64' || { echo "expected arm64 Mach-O: $binary" >&2; exit 1; }
done
shasum -a 256 "$APP/GraftHost" "$APP/GraftProbeHelper" "$APP/GraftProbeTest.dylib" "$APP/Info.plist" > "$OUT/package-manifest.sha256"
printf '%s\n' "Package structure verified; entitlements are reported by codesign when a signed device build is supplied."
