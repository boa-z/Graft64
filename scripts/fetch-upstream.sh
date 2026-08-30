#!/usr/bin/env bash
set -euo pipefail
echo "GRAFT-0001 does not fetch Wine/FEX. Add pinned refs to third_party/manifest/deps.lock before G1." >&2
exit 2
