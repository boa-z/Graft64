#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out/host-tests"
mkdir -p "$OUT"
clang -std=c11 -Wall -Wextra -Werror -I"$ROOT/platform/include" "$ROOT/tests/host/test_ipc.c" "$ROOT/platform/darwin/graft_ipc_protocol.c" -o "$OUT/test_ipc"
"$OUT/test_ipc"
clang -std=c11 -Wall -Wextra -Werror -I"$ROOT/platform/include" \
  "$ROOT/tests/host/test_probe.c" "$ROOT/platform/darwin/"*.c -o "$OUT/test_probe"
clang -std=c11 -Wall -Wextra -Werror -I"$ROOT/platform/include" \
  "$ROOT/probes/helper/main.c" "$ROOT/platform/darwin/graft_ipc_protocol.c" -o "$OUT/GraftProbeHelper"
clang -std=c11 -Wall -Wextra -Werror -dynamiclib "$ROOT/probes/dylib/GraftProbeTest.c" -o "$OUT/GraftProbeTest.dylib"
"$OUT/test_probe" "$OUT/GraftProbeHelper" "$OUT/GraftProbeTest.dylib"
python3 "$ROOT/tests/host/test_report_schema.py"
for script in "$ROOT"/scripts/*.sh; do shellcheck "$script"; done
