#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out"
SDK_NAME="${GRAFT_SDK:-iphonesimulator}"
CONFIGURATION="${GRAFT_CONFIGURATION:-Debug}"
mkdir -p "$OUT"
"$ROOT/scripts/bootstrap-macos.sh"
xcodebuild -project "$ROOT/app/GraftHost/GraftHost.xcodeproj" -target GraftHost -sdk "$SDK_NAME" -configuration "$CONFIGURATION" \
  CODE_SIGNING_ALLOWED=NO ONLY_ACTIVE_ARCH=NO SYMROOT="$OUT/build" OBJROOT="$OUT/obj" build
APP="$OUT/build/${CONFIGURATION}-${SDK_NAME}/GraftHost.app"
test -d "$APP" || { echo "built app not found: $APP" >&2; exit 1; }
rm -rf "$OUT/Payload"
mkdir -p "$OUT/Payload"
cp -R "$APP" "$OUT/Payload/GraftHost.app"
SDKROOT_DEVICE="$(xcrun --sdk iphoneos --show-sdk-path)"
HELPER_TARGET="arm64-apple-ios17.4"
if [[ "$SDK_NAME" == iphonesimulator* ]]; then HELPER_TARGET="arm64-apple-ios17.4-simulator"; fi
clang -target "$HELPER_TARGET" -isysroot "$SDKROOT_DEVICE" -I"$ROOT/platform/include" \
  "$ROOT/probes/helper/main.c" "$ROOT/platform/darwin/graft_ipc_protocol.c" \
  -o "$OUT/Payload/GraftHost.app/GraftProbeHelper"
clang -target "$HELPER_TARGET" -isysroot "$SDKROOT_DEVICE" -dynamiclib \
  "$ROOT/probes/dylib/GraftProbeTest.c" -o "$OUT/Payload/GraftHost.app/GraftProbeTest.dylib"
chmod +x "$OUT/Payload/GraftHost.app/GraftProbeHelper"
"$ROOT/scripts/package-ipa.sh"
