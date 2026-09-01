#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${GRAFT_PACKAGE_OUT:-$ROOT/out}"
IPA="$OUT/GraftHost.ipa"
IPA_CHECKSUM="$OUT/GraftHost.ipa.sha256"
test -f "$IPA" || { echo "missing $IPA" >&2; exit 1; }
test -f "$IPA_CHECKSUM" || {
  echo "missing IPA checksum: $IPA_CHECKSUM (run package-ipa.sh first)" >&2
  exit 1
}

# package-ipa.sh emits a companion shasum file.  Verify it before inspecting
# the archive so a stale or replaced IPA cannot pass structural checks.
if ! awk '
  NR == 1 {
    if (NF != 2) exit 1
    next
  }
  { exit 1 }
  END {
    if (NR != 1) exit 1
  }
' "$IPA_CHECKSUM"; then
  echo "IPA checksum must contain exactly one digest/path line: $IPA_CHECKSUM" >&2
  exit 1
fi
CHECKSUM_LINE="$(awk 'NR == 1 { print; exit }' "$IPA_CHECKSUM")"
EXPECTED_DIGEST="${CHECKSUM_LINE%%[[:space:]]*}"
CHECKSUM_PATH="$(awk 'NR == 1 { print $2; exit }' "$IPA_CHECKSUM")"
if [[ ! "$EXPECTED_DIGEST" =~ ^[0-9a-f]{64}$ ]]; then
  echo "IPA checksum has an invalid SHA-256 digest: $EXPECTED_DIGEST" >&2
  exit 1
fi
if [[ "$CHECKSUM_PATH" != "GraftHost.ipa" ]]; then
  echo "IPA checksum path must be GraftHost.ipa (got: $CHECKSUM_PATH)" >&2
  exit 1
fi
TMP="$(mktemp -d)"
ENTITLEMENTS_TMP=""
PACKAGE_MANIFEST_TMP=""
cleanup_package_verification() {
  rm -rf -- "$TMP"
  if [[ -n "$ENTITLEMENTS_TMP" ]]; then
    rm -f -- "$ENTITLEMENTS_TMP"
  fi
  if [[ -n "$PACKAGE_MANIFEST_TMP" ]]; then
    rm -f -- "$PACKAGE_MANIFEST_TMP"
  fi
}
trap cleanup_package_verification EXIT
IPA_SNAPSHOT="$TMP/GraftHost.ipa"
cp "$IPA" "$IPA_SNAPSHOT"
ACTUAL_DIGEST="$(shasum -a 256 "$IPA_SNAPSHOT" | awk '{ print $1; exit }')"
if [[ "$ACTUAL_DIGEST" != "$EXPECTED_DIGEST" ]]; then
  echo "IPA checksum mismatch for $IPA (expected $EXPECTED_DIGEST, got $ACTUAL_DIGEST)" >&2
  exit 1
fi
printf '%s\n' "IPA SHA-256 verified: $ACTUAL_DIGEST"

unzip -q "$IPA_SNAPSHOT" -d "$TMP"
APP="$TMP/Payload/GraftHost.app"
test -x "$APP/GraftHost" || { echo "main executable missing or not executable" >&2; exit 1; }
test -x "$APP/GraftProbeHelper" || { echo "helper missing or not executable" >&2; exit 1; }
test -f "$APP/GraftProbeTest.dylib" || { echo "test dylib missing" >&2; exit 1; }
test -f "$APP/Info.plist" || { echo "Info.plist missing" >&2; exit 1; }
plutil -lint "$APP/Info.plist" >/dev/null
EXECUTABLE_NAME_IN_PLIST="$(plutil -extract CFBundleExecutable raw "$APP/Info.plist")"
test "$EXECUTABLE_NAME_IN_PLIST" = "GraftHost" || {
  echo "Info.plist CFBundleExecutable must resolve to GraftHost (got: $EXECUTABLE_NAME_IN_PLIST)" >&2
  exit 1
}
ENTITLEMENTS="$OUT/entitlements.plist"
ENTITLEMENTS_TMP="$(mktemp "$OUT/.entitlements.plist.XXXXXX")"
if codesign -d --entitlements :- "$APP" > "$ENTITLEMENTS_TMP" 2>&1 && grep -q '<plist' "$ENTITLEMENTS_TMP"; then
  printf '%s\n' "Signed entitlements extracted to $OUT/entitlements.plist"
else
  printf '%s\n' "UNSIGNED: codesign entitlements unavailable for this local build; supply a signed device IPA for final entitlement verification." > "$ENTITLEMENTS_TMP"
fi
rm -f "$ENTITLEMENTS"
mv "$ENTITLEMENTS_TMP" "$ENTITLEMENTS"
ENTITLEMENTS_TMP=""
file "$APP/GraftHost"
otool -hv "$APP/GraftHost" | head -3
for binary in "$APP/GraftHost" "$APP/GraftProbeHelper" "$APP/GraftProbeTest.dylib"; do
  file -b "$binary" | grep -Eq 'Mach-O.*arm64' || {
    echo "expected arm64 Mach-O: $binary" >&2
    exit 1
  }
done
PACKAGE_MANIFEST="$OUT/package-manifest.sha256"
PACKAGE_MANIFEST_TMP="$(mktemp "$OUT/.package-manifest.sha256.XXXXXX")"
(
  cd "$TMP"
  shasum -a 256 \
    Payload/GraftHost.app/GraftHost \
    Payload/GraftHost.app/GraftProbeHelper \
    Payload/GraftHost.app/GraftProbeTest.dylib \
    Payload/GraftHost.app/Info.plist
) > "$PACKAGE_MANIFEST_TMP"
rm -f "$PACKAGE_MANIFEST"
mv "$PACKAGE_MANIFEST_TMP" "$PACKAGE_MANIFEST"
PACKAGE_MANIFEST_TMP=""
printf '%s\n' "Package structure verified; entitlements are reported by codesign when a signed device build is supplied."
