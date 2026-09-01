#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
command -v python3 >/dev/null 2>&1 || {
  echo "build-runtime-linux-arm64: required tool not found: python3" >&2
  exit 1
}
LOCK="${GRAFT_DEPS_LOCK:-$ROOT/third_party/manifest/deps.lock}"
PATCH_ROOT="${GRAFT_PATCH_ROOT:-$ROOT}"
OUT="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "${GRAFT_RUNTIME_OUT:-$ROOT/out/runtime-linux-arm64}")"
UPSTREAM="${GRAFT_UPSTREAM_DIR:-$ROOT/out/upstream}"
PREFIX="$OUT/root"
INPUT_FINGERPRINT_FILE="$OUT/build-input.sha256"
DOCKERFILE="$ROOT/containers/runtime-builder/Dockerfile"
WINE_SOURCE=""
FEX_SOURCE=""

die() { echo "build-runtime-linux-arm64: $*" >&2; exit 1; }

[[ "$OUT" != / && "$OUT" != "$ROOT" ]] || die "refusing unsafe runtime output directory: $OUT"
test -f "$LOCK" || die "dependency lock not found: $LOCK"
test -f "$DOCKERFILE" || die "runtime builder Dockerfile not found: $DOCKERFILE"

BUILD_INPUT_FINGERPRINT="$(python3 - "$ROOT" "$LOCK" "$PATCH_ROOT" <<'PY'
import hashlib
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
lock = Path(sys.argv[2]).resolve()
patch_root = Path(sys.argv[3]).resolve()
inputs = [
    ("third_party/manifest/deps.lock", lock),
    ("containers/runtime-builder/Dockerfile", root / "containers/runtime-builder/Dockerfile"),
    ("scripts/fetch-upstream.sh", root / "scripts/fetch-upstream.sh"),
    ("scripts/apply-patches.sh", root / "scripts/apply-patches.sh"),
    ("scripts/build-runtime-linux-arm64.sh", root / "scripts/build-runtime-linux-arm64.sh"),
]
patches_root = patch_root / "patches"
if patches_root.is_symlink() or not patches_root.is_dir():
    raise SystemExit(f"patches/ must be a real directory: {patches_root}")
resolved_patches_root = patches_root.resolve()
for dependency in ("wine", "fex"):
    patch_dir = patches_root / dependency
    if patch_dir.is_symlink() or not patch_dir.is_dir():
        raise SystemExit(f"patch directory must be real: {patch_dir}")
    if patch_dir.resolve().parent != resolved_patches_root:
        raise SystemExit(f"patch directory escapes patches/: {patch_dir}")
    for patch in sorted(patch_dir.glob("*.patch")):
        if not patch.is_file() or patch.is_symlink():
            raise SystemExit(f"patch must be a regular file: {patch}")
        inputs.append((f"patches/{dependency}/{patch.name}", patch))

digest = hashlib.sha256(b"Graft64 build inputs v1\0")
for logical_path, path in inputs:
    if not path.is_file():
        raise SystemExit(f"build input not found: {path}")
    name = logical_path.encode("utf-8")
    contents = path.read_bytes()
    digest.update(struct.pack(">Q", len(name)))
    digest.update(name)
    digest.update(struct.pack(">Q", len(contents)))
    digest.update(contents)
print(digest.hexdigest())
PY
)"
[[ "$BUILD_INPUT_FINGERPRINT" =~ ^[0-9a-f]{64}$ ]] || \
  die "failed to compute the build input fingerprint"

container_lock="$(python3 - "$LOCK" "$DOCKERFILE" <<'PY'
import re
import sys
from pathlib import Path

lock_text = Path(sys.argv[1]).read_text(encoding="utf-8")
dockerfile_text = Path(sys.argv[2]).read_text(encoding="utf-8")

def scalar(pattern, owner):
    match = re.search(pattern, lock_text, re.MULTILINE)
    if not match:
        raise SystemExit(f"deps.lock is missing {owner}")
    value = match.group(1).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {'\"', "'"}:
        value = value[1:-1]
    if not value or "\t" in value or "\n" in value:
        raise SystemExit(f"deps.lock has an invalid {owner}")
    return value

container_base = scalar(r"^  container_base:\s*([^\n]+)$", "toolchain.container_base")
llvm_url = scalar(r"^    archive:\s*([^\n]+)$", "toolchain.llvm_mingw.archive")
llvm_sha = scalar(r"^    archive_sha256:\s*([^\n]+)$", "toolchain.llvm_mingw.archive_sha256")
if not re.fullmatch(r"[0-9a-f]{64}", llvm_sha):
    raise SystemExit("deps.lock has an invalid toolchain.llvm_mingw.archive_sha256")
from_match = re.search(r"^FROM(?:\s+--platform=\S+)?\s+(\S+)", dockerfile_text, re.MULTILINE)
if not from_match or from_match.group(1) != container_base:
    actual = from_match.group(1) if from_match else "<missing>"
    raise SystemExit(
        f"builder base does not match deps.lock: Dockerfile={actual}, deps.lock={container_base}"
    )
print("\t".join((llvm_url, llvm_sha)))
PY
)"
IFS=$'\t' read -r LLVM_MINGW_URL LLVM_MINGW_SHA256 extra <<< "$container_lock"
[[ -n "$LLVM_MINGW_URL" && -n "$LLVM_MINGW_SHA256" && -z "${extra:-}" ]] || \
  die "failed to read the pinned LLVM-MinGW inputs"

if [[ "${GRAFT_RUNTIME_IN_CONTAINER:-0}" != 1 ]]; then
  host_os="$(uname -s)"
  host_arch="$(uname -m)"
  if [[ "$host_os" != Linux || "$host_arch" != aarch64 ]]; then
    if [[ "${GRAFT_RUNTIME_USE_CONTAINER:-0}" == 1 ]] && command -v docker >/dev/null 2>&1; then
      image="${GRAFT_RUNTIME_IMAGE:-graft64/runtime-builder:${BUILD_INPUT_FINGERPRINT:0:16}}"
      if ! docker image inspect "$image" >/dev/null 2>&1; then
        docker build --platform linux/arm64 \
          --build-arg "LLVM_MINGW_URL=$LLVM_MINGW_URL" \
          --build-arg "LLVM_MINGW_SHA256=$LLVM_MINGW_SHA256" \
          -t "$image" -f "$DOCKERFILE" "$ROOT"
      fi
      exec docker run --rm --platform linux/arm64 \
        -e GRAFT_RUNTIME_IN_CONTAINER=1 \
        -e GRAFT_RUNTIME_OUT=/workspace/out/runtime-linux-arm64 \
        -v "$ROOT:/workspace" \
        "$image" \
        /workspace/scripts/build-runtime-linux-arm64.sh "$@"
    fi
    die "G1 requires a native Linux arm64 host; set GRAFT_RUNTIME_USE_CONTAINER=1 with an arm64 Docker engine"
  fi
fi

for tool in clang cmake git make ninja python3; do
  command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done

python3 "$ROOT/scripts/prepare-runtime-output.py" "$OUT"

GRAFT_DEPS_LOCK="$LOCK" \
GRAFT_UPSTREAM_DIR="$UPSTREAM" \
  "$ROOT/scripts/fetch-upstream.sh"

command -v arm64ec-w64-mingw32-clang >/dev/null 2>&1 || \
  die "arm64ec-w64-mingw32-clang not found; install the pinned LLVM-MinGW ARM64EC toolchain and put it first in PATH"
command -v aarch64-w64-mingw32-clang >/dev/null 2>&1 || \
  die "aarch64-w64-mingw32-clang not found; install the pinned LLVM-MinGW AArch64 toolchain and put it first in PATH"

if [[ "${GRAFT_RUNTIME_PREFLIGHT:-0}" == 1 ]]; then
  printf '%s\n' "G1 preflight passed: Linux arm64, clang/cmake/ninja, pinned Wine/FEX sources, and ARM64 toolchains are available."
  exit 0
fi

mkdir -p "$OUT" "$OUT/logs"
previous_fingerprint=""
if [[ -f "$INPUT_FINGERPRINT_FILE" ]]; then
  previous_fingerprint="$(tr -d '\n' < "$INPUT_FINGERPRINT_FILE")"
fi
if [[ "${GRAFT_RUNTIME_CLEAN:-0}" == 1 || "$previous_fingerprint" != "$BUILD_INPUT_FINGERPRINT" ]]; then
  rm -rf -- "$OUT/build" "$OUT/root" "$OUT/prefix"
fi
mkdir -p "$PREFIX" "$OUT/build"

GRAFT_UPSTREAM_DIR="$UPSTREAM" \
GRAFT_RUNTIME_OUT="$OUT" \
GRAFT_PATCH_ROOT="$PATCH_ROOT" \
GRAFT_DEPS_LOCK="$LOCK" \
GRAFT_FETCHED_MANIFEST="$UPSTREAM/fetched-manifest.tsv" \
  "$ROOT/scripts/apply-patches.sh"

prepared_sources="$OUT/build/prepared-sources.tsv"
test -f "$prepared_sources" || die "prepared source map not found: $prepared_sources"
header="$(sed -n '1p' "$prepared_sources")"
[[ "$header" == $'dependency\tcommit\tarchive\tprepared_source' ]] || \
  die "prepared source map has an invalid header"
while IFS=$'\t' read -r dependency commit _archive prepared_source extra; do
  [[ "$dependency" != dependency ]] || continue
  [[ -z "${extra:-}" ]] || die "prepared source map has extra fields for $dependency"
  [[ "$prepared_source" == "$OUT/build/sources/$dependency-$commit" ]] || \
    die "prepared source path is outside the runtime build directory: $prepared_source"
  test -d "$prepared_source" || die "prepared source directory not found: $prepared_source"
  case "$dependency" in
    wine) [[ -z "$WINE_SOURCE" ]] || die "duplicate prepared Wine source"; WINE_SOURCE="$prepared_source" ;;
    fex) [[ -z "$FEX_SOURCE" ]] || die "duplicate prepared FEX source"; FEX_SOURCE="$prepared_source" ;;
    *) die "unexpected prepared dependency: $dependency" ;;
  esac
done < "$prepared_sources"
test -n "$WINE_SOURCE" || die "prepared Wine source not found"
test -n "$FEX_SOURCE" || die "prepared FEX source not found"

WINE_BUILD="$OUT/build/wine"
mkdir -p "$WINE_BUILD"
pushd "$WINE_BUILD" >/dev/null
if [[ ! -f config.status ]]; then
  "$WINE_SOURCE/configure" \
    --enable-archs=arm64ec,aarch64 \
    --with-mingw=clang \
    --disable-tests \
    --disable-win16 \
    --without-x \
    --without-wayland \
    --without-vulkan \
    --prefix="$PREFIX" \
    2>&1 | tee "$OUT/logs/wine-configure.log"
fi
make -j"${GRAFT_JOBS:-$(nproc)}" 2>&1 | tee "$OUT/logs/wine-build.log"
make install 2>&1 | tee "$OUT/logs/wine-install.log"
popd >/dev/null

build_fex() {
  local build_dir="$1"
  local mingw_triple="$2"
  mkdir -p "$build_dir"
  cmake -S "$FEX_SOURCE" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$FEX_SOURCE/Data/CMake/toolchain_mingw.cmake" \
    -DCMAKE_INSTALL_LIBDIR=lib/wine/aarch64-windows \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DMINGW_TRIPLE="$mingw_triple" \
    -DBUILD_TESTING=False \
    -DENABLE_LTO=False \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
    -DTUNE_CPU=none \
    2>&1 | tee "$OUT/logs/fex-${mingw_triple}-configure.log"
  cmake --build "$build_dir" --parallel "${GRAFT_JOBS:-$(nproc)}" 2>&1 | tee "$OUT/logs/fex-${mingw_triple}-build.log"
  cmake --install "$build_dir" 2>&1 | tee "$OUT/logs/fex-${mingw_triple}-install.log"
}

build_fex "$OUT/build/fex-arm64ec" arm64ec-w64-mingw32
build_fex "$OUT/build/fex-wow64" aarch64-w64-mingw32

test -x "$PREFIX/bin/wine" || die "Wine install completed without $PREFIX/bin/wine"
python3 "$ROOT/scripts/generate-runtime-manifest.py" "$OUT" \
  --lock "$LOCK" \
  --patch-root "$PATCH_ROOT"
GRAFT_DEPS_LOCK="$LOCK" GRAFT_PATCH_ROOT="$PATCH_ROOT" \
  "$ROOT/scripts/verify-artifacts.sh" "$OUT"
fingerprint_temporary="$(mktemp "$OUT/.build-input.XXXXXX")"
printf '%s\n' "$BUILD_INPUT_FINGERPRINT" > "$fingerprint_temporary"
mv -f -- "$fingerprint_temporary" "$INPUT_FINGERPRINT_FILE"
printf '%s\n' "G1 runtime build completed under $OUT"
