#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="${GRAFT_DEPS_LOCK:-$ROOT/third_party/manifest/deps.lock}"
OUT="${1:-${GRAFT_RUNTIME_OUT:-$ROOT/out/runtime-linux-arm64}}"

test -f "$LOCK" || { echo "deps.lock is required before runtime artifacts" >&2; exit 1; }
PATCH_ROOT="${GRAFT_PATCH_ROOT:-$ROOT}"
MANIFEST_TOOL="$ROOT/scripts/generate-runtime-manifest.py"
python3 "$MANIFEST_TOOL" --lock "$LOCK" --validate-lock-only

if [[ ! -d "$OUT" ]]; then
  printf '%s\n' "Dependency lock verified; no runtime artifact directory exists yet ($OUT)."
  exit 0
fi

test -d "$OUT/root" || { echo "runtime root missing: $OUT/root" >&2; exit 1; }
test -s "$OUT/runtime-manifest.sha256" || { echo "runtime manifest missing or empty: $OUT/runtime-manifest.sha256" >&2; exit 1; }
test -s "$OUT/runtime-manifest.json" || { echo "JSON runtime manifest missing or empty: $OUT/runtime-manifest.json" >&2; exit 1; }
python3 "$MANIFEST_TOOL" "$OUT" --lock "$LOCK" --patch-root "$PATCH_ROOT" --verify-existing
