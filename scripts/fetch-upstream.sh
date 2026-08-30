#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="${GRAFT_DEPS_LOCK:-$ROOT/third_party/manifest/deps.lock}"
OUT="${GRAFT_UPSTREAM_DIR:-$ROOT/out/upstream}"
DOWNLOADS="$OUT/downloads"

for tool in curl shasum tar python3; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "required tool not found: $tool" >&2
    exit 1
  }
done
test -f "$LOCK" || { echo "missing dependency lock: $LOCK" >&2; exit 1; }

mkdir -p "$DOWNLOADS"

# Keep parsing dependency metadata dependency-free. The lock file deliberately
# uses a small, stable YAML subset; this parser rejects missing or duplicate
# required fields before any source is written.
DEPS=()
while IFS= read -r record; do
  [[ -n "$record" ]] && DEPS+=("$record")
done < <(python3 - "$LOCK" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
blocks = re.findall(r"(?ms)^  - name:\s*([^\n]+)\n(.*?)(?=^  - name:|\Z)", text)
required = ("repository", "ref", "commit", "archive", "archive_sha256")
if not blocks:
    raise SystemExit("deps.lock has no dependencies")
seen = set()
for name, body in blocks:
    name = name.strip()
    if name in seen:
        raise SystemExit(f"duplicate dependency: {name}")
    seen.add(name)
    fields = dict(re.findall(r"^    ([A-Za-z0-9_]+):\s*(.*?)\s*$", body, re.M))
    missing = [field for field in required if not fields.get(field)]
    if missing:
        raise SystemExit(f"{name}: missing fields: {', '.join(missing)}")
    print("\t".join([name, fields["ref"].strip('"'), fields["commit"].strip('"'),
                       fields["archive"].strip('"'), fields["archive_sha256"].strip('"')]))
PY
)

MANIFEST="$OUT/fetched-manifest.tsv"
printf 'name\tref\tcommit\tarchive_sha256\tarchive\n' > "$MANIFEST"
for record in "${DEPS[@]}"; do
  IFS=$'\t' read -r name ref commit archive expected_sha <<< "$record"
  archive_file="$DOWNLOADS/$name-$commit.tar.gz"
  source_dir="$OUT/$name-$commit"
  if [[ ! -f "$archive_file" ]]; then
    curl --fail --location --silent --show-error --retry 3 "$archive" --output "$archive_file"
  fi
  actual_sha="$(shasum -a 256 "$archive_file" | awk '{print $1}')"
  [[ "$actual_sha" == "$expected_sha" ]] || {
    echo "$name archive hash mismatch: expected $expected_sha, got $actual_sha" >&2
    exit 1
  }
  if [[ ! -d "$source_dir" ]]; then
    mkdir -p "$source_dir"
    tar --extract --gzip --strip-components=1 --file "$archive_file" --directory "$source_dir"
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$ref" "$commit" "$actual_sha" "$archive" >> "$MANIFEST"
done

printf '%s\n' "Fetched and verified ${#DEPS[@]} pinned upstream sources under $OUT"
