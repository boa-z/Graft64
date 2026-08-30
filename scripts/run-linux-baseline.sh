#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
RUNTIME="${GRAFT_RUNTIME_OUT:-$ROOT/out/runtime-linux-arm64}"
SAMPLES="${GRAFT_SAMPLES_OUT:-$RUNTIME/samples}"
PREFIX="$RUNTIME/root"
LOGS="$RUNTIME/logs"

die() { echo "run-linux-baseline: $*" >&2; exit 1; }
[[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || \
  die "G1 baseline must run on a native Linux arm64 host"
test -x "$PREFIX/bin/wine" || die "Wine runtime missing: run scripts/build-runtime-linux-arm64.sh first"

for tool in aarch64-w64-mingw32-clang x86_64-w64-mingw32-clang; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool not found in PATH"
done

mkdir -p "$SAMPLES" "$LOGS"
aarch64-w64-mingw32-clang -O2 "$ROOT/samples/windows-arm64/hello-arm64.c" -o "$SAMPLES/hello-arm64.exe"
x86_64-w64-mingw32-clang -O2 "$ROOT/samples/windows-amd64/hello-amd64.c" -o "$SAMPLES/hello-amd64.exe"
shasum -a 256 "$SAMPLES/hello-arm64.exe" "$SAMPLES/hello-amd64.exe" > "$SAMPLES/sample-manifest.sha256"

WINEPREFIX="$RUNTIME/prefix"
mkdir -p "$WINEPREFIX"
run_sample() {
  local name="$1"
  local expected="$2"
  local output="$LOGS/$name.log"
  set +e
  WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all "$PREFIX/bin/wine" "$SAMPLES/$name.exe" >"$output" 2>&1
  local status=$?
  set -e
  [[ "$status" -eq 0 ]] || die "$name exited with $status; see $output"
  grep -Fqx "$expected" "$output" || die "$name output did not contain expected marker; see $output"
  printf '%s\n' "PASS $name"
}

run_sample hello-arm64 GRAFT64_HELLO_ARM64
run_sample hello-amd64 GRAFT64_HELLO_AMD64
python3 - "$RUNTIME/g1-baseline-tests.json" <<'PY'
import json
import sys

tests = [
    {"name": "hello-arm64", "status": "PASS", "log": "logs/hello-arm64.log"},
    {"name": "hello-amd64", "status": "PASS", "log": "logs/hello-amd64.log"},
]
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(tests, stream, indent=2)
    stream.write("\n")
PY
python3 "$ROOT/scripts/generate-runtime-manifest.py" "$RUNTIME" \
  --lock "$ROOT/third_party/manifest/deps.lock" \
  --test-report "$RUNTIME/g1-baseline-tests.json"
printf '%s\n' "G1 Linux ARM64 baseline passed; logs and sample hashes are under $RUNTIME"
