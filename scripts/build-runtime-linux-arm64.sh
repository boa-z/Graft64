#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${GRAFT_RUNTIME_OUT:-$ROOT/out/runtime-linux-arm64}"
UPSTREAM="${GRAFT_UPSTREAM_DIR:-$ROOT/out/upstream}"
PREFIX="$OUT/root"
WINE_SOURCE=""
FEX_SOURCE=""

die() { echo "build-runtime-linux-arm64: $*" >&2; exit 1; }

if [[ "${GRAFT_RUNTIME_IN_CONTAINER:-0}" != 1 ]]; then
  host_os="$(uname -s)"
  host_arch="$(uname -m)"
  if [[ "$host_os" != Linux || "$host_arch" != aarch64 ]]; then
    if [[ "${GRAFT_RUNTIME_USE_CONTAINER:-0}" == 1 ]] && command -v docker >/dev/null 2>&1; then
      image="${GRAFT_RUNTIME_IMAGE:-graft64/runtime-builder:local}"
      if ! docker image inspect "$image" >/dev/null 2>&1; then
        docker build --platform linux/arm64 -t "$image" -f "$ROOT/containers/runtime-builder/Dockerfile" "$ROOT"
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

"$ROOT/scripts/fetch-upstream.sh"
for source in "$UPSTREAM"/wine-* "$UPSTREAM"/fex-*; do
  test -d "$source" || die "missing fetched source: $source"
done
WINE_SOURCE="$(find "$UPSTREAM" -mindepth 1 -maxdepth 1 -type d -name 'wine-*' -print -quit)"
FEX_SOURCE="$(find "$UPSTREAM" -mindepth 1 -maxdepth 1 -type d -name 'fex-*' -print -quit)"
test -n "$WINE_SOURCE" || die "Wine source not found"
test -n "$FEX_SOURCE" || die "FEX source not found"

command -v arm64ec-w64-mingw32-clang >/dev/null 2>&1 || \
  die "arm64ec-w64-mingw32-clang not found; install the pinned LLVM-MinGW ARM64EC toolchain and put it first in PATH"
command -v aarch64-w64-mingw32-clang >/dev/null 2>&1 || \
  die "aarch64-w64-mingw32-clang not found; install the pinned LLVM-MinGW AArch64 toolchain and put it first in PATH"

if [[ "${GRAFT_RUNTIME_PREFLIGHT:-0}" == 1 ]]; then
  printf '%s\n' "G1 preflight passed: Linux arm64, clang/cmake/ninja, pinned Wine/FEX sources, and ARM64 toolchains are available."
  exit 0
fi

mkdir -p "$OUT" "$PREFIX" "$OUT/logs" "$OUT/build"
if [[ "${GRAFT_RUNTIME_CLEAN:-0}" == 1 ]]; then
  rm -rf "$OUT/build/wine" "$OUT/build/fex-arm64ec" "$OUT/build/fex-wow64" "$PREFIX"
  mkdir -p "$PREFIX"
fi

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
find "$PREFIX" -type f -print | sort | while IFS= read -r file; do
  shasum -a 256 "$file"
done > "$OUT/runtime-manifest.sha256"
"$ROOT/scripts/verify-artifacts.sh" "$OUT"
printf '%s\n' "G1 runtime build completed under $OUT"
