#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out/host-tests"
mkdir -p "$OUT"
clang -std=c11 -Wall -Wextra -Werror -I"$ROOT/platform/include" "$ROOT/tests/host/test_ipc.c" "$ROOT/platform/darwin/graft_ipc_protocol.c" -o "$OUT/test_ipc"
"$OUT/test_ipc"
for script in "$ROOT"/scripts/*.sh; do shellcheck "$script"; done
