#!/usr/bin/env bash
set -euo pipefail
for tool in xcrun xcodebuild clang swift shellcheck; do
  command -v "$tool" >/dev/null 2>&1 || { echo "missing required tool: $tool" >&2; exit 1; }
done
SDKROOT_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"
SWIFT_VERSION="$(swift --version | head -1)"
printf '%s\n' "iPhoneOS SDK: $SDKROOT_PATH" "Swift: $SWIFT_VERSION"
