#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out"
APP="$OUT/Payload/GraftHost.app"
test -d "$APP" || { echo "expected $APP; run build-probes.sh first" >&2; exit 1; }
rm -f "$OUT/GraftHost.ipa"
(cd "$OUT" && zip -qr GraftHost.ipa Payload)
shasum -a 256 "$OUT/GraftHost.ipa" > "$OUT/GraftHost.ipa.sha256"
printf '%s\n' "Created $OUT/GraftHost.ipa"
