#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="${GRAFT_DEPS_LOCK:-$ROOT/third_party/manifest/deps.lock}"
OUT="${1:-${GRAFT_RUNTIME_OUT:-$ROOT/out/runtime-linux-arm64}}"

test -f "$LOCK" || { echo "deps.lock is required before runtime artifacts" >&2; exit 1; }
python3 - "$LOCK" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
if not re.search(r"^stage:\s*GRAFT-0002\s*$", text, re.M):
    raise SystemExit("deps.lock must declare stage GRAFT-0002")
blocks = re.findall(r"(?ms)^  - name:\s*([^\n]+)\n(.*?)(?=^  - name:|\Z)", text)
if {name.strip() for name, _ in blocks} != {"wine", "fex"}:
    raise SystemExit("deps.lock must pin exactly wine and fex for G1")
for name, body in blocks:
    fields = dict(re.findall(r"^    ([A-Za-z0-9_]+):\s*(.*?)\s*$", body, re.M))
    for key in ("repository", "ref", "commit", "archive", "archive_sha256"):
        if not fields.get(key):
            raise SystemExit(f"{name.strip()}: missing {key}")
    if not re.fullmatch(r"[0-9a-f]{40}", fields["commit"].strip('"')):
        raise SystemExit(f"{name.strip()}: commit must be a 40-character SHA")
    if not re.fullmatch(r"[0-9a-f]{64}", fields["archive_sha256"].strip('"')):
        raise SystemExit(f"{name.strip()}: archive_sha256 must be a 64-character SHA-256")
PY

if [[ ! -d "$OUT" ]]; then
  printf '%s\n' "Dependency lock verified; no runtime artifact directory exists yet ($OUT)."
  exit 0
fi

test -d "$OUT/root" || { echo "runtime root missing: $OUT/root" >&2; exit 1; }
test -s "$OUT/runtime-manifest.sha256" || { echo "runtime manifest missing or empty: $OUT/runtime-manifest.sha256" >&2; exit 1; }
test -s "$OUT/runtime-manifest.json" || { echo "JSON runtime manifest missing or empty: $OUT/runtime-manifest.json" >&2; exit 1; }
python3 - "$OUT/runtime-manifest.json" <<'PY'
import json
import re
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
if manifest.get("schema_version") != 1 or manifest.get("stage") not in {"G1", "G2", "G3"}:
    raise SystemExit("runtime manifest has an invalid schema_version or stage")
if manifest.get("platform") != {"os": "linux", "architecture": "aarch64"}:
    raise SystemExit("runtime manifest platform must be linux/aarch64")
for artifact in manifest.get("artifacts", []):
    if not re.fullmatch(r"[0-9a-f]{64}", artifact.get("sha256", "")):
        raise SystemExit(f"invalid artifact hash: {artifact.get('path')}")
for test in manifest.get("tests", []):
    if test.get("status") not in {"PASS", "FAIL", "BLOCKED", "UNVERIFIED"}:
        raise SystemExit(f"invalid test status: {test.get('name')}")
PY
grep -q '/bin/wine$' "$OUT/runtime-manifest.sha256" || { echo "runtime manifest does not contain bin/wine" >&2; exit 1; }
printf '%s\n' "Runtime artifact structure verified: $OUT"
