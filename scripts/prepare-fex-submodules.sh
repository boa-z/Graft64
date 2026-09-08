#!/usr/bin/env bash
# Populate only the upstream gitlinks needed by the MinGW runtime build.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
SOURCE="${1:?prepared FEX source directory required}"
COMMIT="${2:?locked FEX commit required}"
CACHE="${GRAFT_UPSTREAM_DIR:-$ROOT/out/upstream}/fex-git"
[[ "$COMMIT" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid FEX commit" >&2; exit 1; }
test -f "$SOURCE/.gitmodules"
if [[ ! -d "$CACHE/.git" ]]; then
  mkdir -p "$CACHE"
  git -C "$CACHE" init
fi
# Fetch an exact object, never a moving branch or submodule --remote.
git -C "$CACHE" fetch --depth=1 https://github.com/FEX-Emu/FEX.git "$COMMIT"
git -C "$CACHE" checkout --detach "$COMMIT"
# The existing FEX lock names an annotated release tag object; peel that exact
# object, never resolve the release name from a remote ref.
test "$(git -C "$CACHE" rev-parse HEAD)" = "$(git -C "$CACHE" rev-parse "$COMMIT^{commit}")"
modules=(External/rpmalloc External/unordered_dense External/xxhash External/fmt External/range-v3 Source/Common/cpp-optparse)
git -C "$CACHE" submodule update --init --depth=1 -- "${modules[@]}"
for module in "${modules[@]}"; do
  expected="$(git -C "$CACHE" rev-parse "HEAD:$module")"
  actual="$(git -C "$CACHE/$module" rev-parse HEAD)"
  test "$actual" = "$expected"
  # SOURCE is a freshly prepared, verified archive. Refuse to overlay content.
  target="$SOURCE/$module"
  test -d "$target" && test ! -L "$target"
  test -z "$(ls -A "$target")"
  git -C "$CACHE/$module" archive "$expected" | tar -x -C "$target"
  printf 'FEX gitlink %s %s\n' "$module" "$expected"
done
