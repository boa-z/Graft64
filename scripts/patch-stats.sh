#!/usr/bin/env bash
set -euo pipefail
for dir in patches/wine patches/fex; do
  count=$(find "$dir" -type f -name '*.patch' | wc -l | tr -d ' ')
  printf '%s patches: %s\n' "$dir" "$count"
done
