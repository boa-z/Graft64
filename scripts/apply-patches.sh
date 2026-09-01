#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
UPSTREAM="${GRAFT_UPSTREAM_DIR:-$ROOT/out/upstream}"
OUT="${GRAFT_RUNTIME_OUT:-$ROOT/out/runtime-linux-arm64}"
PATCH_ROOT="${GRAFT_PATCH_ROOT:-$ROOT}"
LOCK="${GRAFT_DEPS_LOCK:-$ROOT/third_party/manifest/deps.lock}"

die() { echo "apply-patches: $*" >&2; exit 1; }

for tool in git python3 tar; do
  command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done

UPSTREAM="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$UPSTREAM")"
OUT="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$OUT")"
PATCH_ROOT="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$PATCH_ROOT")"
LOCK="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$LOCK")"
[[ "$OUT" != / && "$OUT" != "$ROOT" ]] || die "refusing unsafe runtime output directory: $OUT"
python3 "$ROOT/scripts/prepare-runtime-output.py" "$OUT"
FETCHED_MANIFEST="${GRAFT_FETCHED_MANIFEST:-$UPSTREAM/fetched-manifest.tsv}"
WORK_ROOT="$OUT/build/sources"
PREPARED_SOURCES="$OUT/build/prepared-sources.tsv"
APPLIED_PATCHES="$OUT/applied-patches.json"

test -f "$FETCHED_MANIFEST" || die "fetched manifest not found: $FETCHED_MANIFEST"
test -f "$LOCK" || die "dependency lock not found: $LOCK"
for dependency in wine fex; do
  test -d "$PATCH_ROOT/patches/$dependency" || \
    die "patch directory not found: $PATCH_ROOT/patches/$dependency"
done

mkdir -p "$OUT" "$WORK_ROOT"
temporary_dir="$(mktemp -d "$OUT/.apply-patches.XXXXXX")"
trap 'rm -rf -- "$temporary_dir"' EXIT
expected_patches="$temporary_dir/applied-patches.json"
expected_sources="$temporary_dir/prepared-sources.tsv"

python3 - \
  "$FETCHED_MANIFEST" \
  "$UPSTREAM" \
  "$WORK_ROOT" \
  "$PATCH_ROOT" \
  "$LOCK" \
  "$expected_sources" \
  "$expected_patches" <<'PY'
import csv
import hashlib
import json
import re
import sys
from pathlib import Path

(
    manifest_path_raw,
    upstream_raw,
    work_root_raw,
    patch_root_raw,
    lock_raw,
    sources_output_raw,
    patches_output_raw,
) = sys.argv[1:]
manifest_path = Path(manifest_path_raw)
upstream = Path(upstream_raw).resolve()
work_root = Path(work_root_raw).resolve()
patch_root = Path(patch_root_raw).resolve()
lock_path = Path(lock_raw).resolve()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

with manifest_path.open("r", encoding="utf-8", newline="") as stream:
    rows = list(csv.reader(stream, delimiter="\t"))

expected_header = ["name", "ref", "commit", "archive_sha256", "archive"]
if not rows or rows[0] != expected_header:
    raise SystemExit("fetched-manifest.tsv has an invalid header")

dependencies = {}
for line_number, row in enumerate(rows[1:], 2):
    if len(row) != len(expected_header):
        raise SystemExit(f"fetched-manifest.tsv line {line_number} must have five fields")
    record = dict(zip(expected_header, row))
    name = record["name"]
    if name not in {"wine", "fex"}:
        raise SystemExit(f"fetched-manifest.tsv line {line_number} has unexpected dependency: {name!r}")
    if name in dependencies:
        raise SystemExit(f"fetched-manifest.tsv has duplicate dependency: {name}")
    if not record["ref"] or not record["archive"]:
        raise SystemExit(f"fetched-manifest.tsv line {line_number} has an empty ref or archive")
    if not re.fullmatch(r"[0-9a-f]{40}", record["commit"]):
        raise SystemExit(f"fetched-manifest.tsv line {line_number} has an invalid commit")
    if not re.fullmatch(r"[0-9a-f]{64}", record["archive_sha256"]):
        raise SystemExit(f"fetched-manifest.tsv line {line_number} has an invalid archive SHA-256")
    dependencies[name] = record

if set(dependencies) != {"wine", "fex"}:
    raise SystemExit("fetched-manifest.tsv must contain exactly wine and fex")

lock_text = lock_path.read_text(encoding="utf-8")
lock_blocks = re.findall(
    r"(?ms)^  - name:\s*([^\n]+)\n(.*?)(?=^  - name:|^notes:|\Z)",
    lock_text,
)
locked_dependencies = {}
for raw_name, body in lock_blocks:
    name = raw_name.strip().strip('"\'')
    fields = dict(
        re.findall(r"^    ([A-Za-z0-9_]+):\s*(.*?)\s*$", body, re.MULTILINE)
    )
    if name in locked_dependencies:
        raise SystemExit(f"dependency lock has duplicate dependency: {name}")
    locked_dependencies[name] = {
        field: fields.get(field, "").strip().strip('"\'')
        for field in ("ref", "commit", "archive_sha256", "archive")
    }
if set(locked_dependencies) != {"wine", "fex"}:
    raise SystemExit("dependency lock must contain exactly wine and fex")
for name in ("wine", "fex"):
    expected = locked_dependencies[name]
    actual = dependencies[name]
    for field in ("ref", "commit", "archive_sha256", "archive"):
        if actual[field] != expected[field]:
            raise SystemExit(
                f"fetched-manifest.tsv {name}.{field} does not match dependency lock"
            )

prepared_rows = []
for name in ("wine", "fex"):
    commit = dependencies[name]["commit"]
    archive = upstream / "downloads" / f"{name}-{commit}.tar.gz"
    if archive.is_symlink() or not archive.is_file():
        raise SystemExit(f"fetched archive must be a regular file: {archive}")
    actual_archive_sha = sha256_file(archive)
    if actual_archive_sha != dependencies[name]["archive_sha256"]:
        raise SystemExit(
            f"fetched archive hash mismatch for {name}: "
            f"expected {dependencies[name]['archive_sha256']}, got {actual_archive_sha}"
        )
    prepared = work_root / f"{name}-{commit}"
    if prepared.parent != work_root:
        raise SystemExit(f"prepared source escapes build directory: {prepared}")
    prepared_rows.append((name, commit, str(archive), str(prepared)))

with Path(sources_output_raw).open("w", encoding="utf-8", newline="") as stream:
    writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
    writer.writerow(["dependency", "commit", "archive", "prepared_source"])
    writer.writerows(prepared_rows)

patch_entries = []
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
        relative = patch.relative_to(patch_root).as_posix()
        patch_entries.append(
            {
                "dependency": dependency,
                "path": relative,
                "sha256": sha256_file(patch),
            }
        )

with Path(patches_output_raw).open("w", encoding="utf-8") as stream:
    json.dump(patch_entries, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY

while IFS=$'\t' read -r dependency commit archive prepared_source extra; do
  if [[ "$dependency" == dependency ]]; then
    continue
  fi
  [[ -z "${extra:-}" ]] || die "invalid prepared source record for $dependency"
  case "$dependency" in
    fex|wine) ;;
    *) die "unexpected prepared dependency: $dependency" ;;
  esac
  [[ "$prepared_source" == "$WORK_ROOT/$dependency-$commit" ]] || \
    die "prepared source path is outside the build source root: $prepared_source"

  rm -rf -- "$prepared_source"
  mkdir -p "$prepared_source"
  tar --extract --gzip --strip-components=1 \
    --file "$archive" \
    --directory "$prepared_source"

  patches=()
  for patch in "$PATCH_ROOT/patches/$dependency"/*.patch; do
    [[ -e "$patch" ]] || continue
    patches+=("$patch")
  done
  if (( ${#patches[@]} > 0 )); then
    for patch in "${patches[@]}"; do
      git -C "$prepared_source" apply --no-index --check --whitespace=nowarn "$patch"
      git -C "$prepared_source" apply --no-index --whitespace=nowarn "$patch"
    done
  fi
done < "$expected_sources"

mv -f -- "$expected_sources" "$PREPARED_SOURCES"
mv -f -- "$expected_patches" "$APPLIED_PATCHES"
printf '%s\n' "Prepared pinned Wine/FEX sources and recorded applied patches in $APPLIED_PATCHES"
