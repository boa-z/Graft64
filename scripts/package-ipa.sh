#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${GRAFT_PACKAGE_OUT:-$ROOT/out}"
APP="$OUT/Payload/GraftHost.app"
IPA="$OUT/GraftHost.ipa"
IPA_CHECKSUM="$OUT/GraftHost.ipa.sha256"
test -d "$APP" || { echo "expected $APP; run build-probes.sh first" >&2; exit 1; }
rm -f "$IPA"
(cd "$OUT" && zip -qr GraftHost.ipa Payload)
CHECKSUM_TMP="$(mktemp "$OUT/.GraftHost.ipa.sha256.XXXXXX")"
trap 'rm -f -- "$CHECKSUM_TMP"' EXIT
(cd "$OUT" && shasum -a 256 GraftHost.ipa) > "$CHECKSUM_TMP"
rm -f "$IPA_CHECKSUM"
mv "$CHECKSUM_TMP" "$IPA_CHECKSUM"
trap - EXIT
printf '%s\n' "Created $IPA"
