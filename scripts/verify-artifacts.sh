#!/usr/bin/env bash
set -euo pipefail
test -f third_party/manifest/deps.lock || { echo "deps.lock is required before runtime artifacts" >&2; exit 1; }
echo "No runtime artifacts are part of GRAFT-0001."
