#!/usr/bin/env bash
set -euo pipefail
echo "G1 runtime build is gated on a green G0 device report; Wine/FEX are not fetched by GRAFT-0001." >&2
exit 2
