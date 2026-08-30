#!/usr/bin/env python3
"""Generate a deterministic, hash-addressed Graft64 runtime manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def lock_dependencies(lock: Path) -> tuple[list[dict[str, str]], str | None]:
    text = lock.read_text(encoding="utf-8")
    blocks = re.findall(r"(?ms)^  - name:\s*([^\n]+)\n(.*?)(?=^  - name:|\Z)", text)
    dependencies: list[dict[str, str]] = []
    for name, body in blocks:
        fields = dict(re.findall(r"^    ([A-Za-z0-9_]+):\s*(.*?)\s*$", body, re.M))
        dependencies.append(
            {
                "name": name.strip(),
                "ref": fields["ref"].strip('"'),
                "commit": fields["commit"].strip('"'),
                "archive_sha256": fields["archive_sha256"].strip('"'),
            }
        )
    toolchain = re.search(r"^    version:\s*([^\n]+)$", text, re.M)
    return dependencies, toolchain.group(1).strip('"') if toolchain else None


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_role(relative: str) -> str:
    if relative.endswith("/bin/wine") or relative == "root/bin/wine":
        return "wine-loader"
    if "libarm64ecfex" in relative:
        return "fex-arm64ec-module"
    if "libwow64fex" in relative:
        return "fex-wow64-module"
    return "runtime-file"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime_dir", type=Path)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--test-report", type=Path)
    args = parser.parse_args()

    runtime_dir = args.runtime_dir.resolve()
    root = runtime_dir / "root"
    if not root.is_dir():
        raise SystemExit(f"runtime root missing: {root}")
    dependencies, llvm_mingw_version = lock_dependencies(args.lock)
    artifacts = []
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        relative = path.relative_to(runtime_dir).as_posix()
        artifacts.append({"path": relative, "sha256": sha256(path), "role": artifact_role(relative)})

    tests = []
    if args.test_report:
        tests = json.loads(args.test_report.read_text(encoding="utf-8"))
        if not isinstance(tests, list):
            raise SystemExit("test report must contain a JSON array")

    manifest = {
        "schema_version": 1,
        "stage": "G1",
        "platform": {"os": "linux", "architecture": "aarch64"},
        "toolchain": {"llvm_mingw_version": llvm_mingw_version},
        "dependencies": dependencies,
        "artifacts": artifacts,
        "tests": tests,
    }
    output = runtime_dir / "runtime-manifest.json"
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
