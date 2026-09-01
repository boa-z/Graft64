#!/usr/bin/env python3
"""Generate and verify deterministic Graft64 G1 runtime manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path, PurePosixPath


DEPENDENCY_FIELDS = (
    "repository",
    "ref",
    "commit",
    "archive",
    "archive_sha256",
    "license",
    "role",
)
TOOLCHAIN_FIELDS = ("container_base", "architecture", "compiler", "cmake", "ninja")
TEST_STATUSES = {"PASS", "FAIL", "BLOCKED", "UNVERIFIED"}
REQUIRED_ARTIFACT_ROLES = {
    "wine-loader",
    "fex-arm64ec-module",
    "fex-wow64-module",
}


def scalar(raw: str) -> str:
    """Read one scalar from the deliberately small deps.lock YAML subset."""

    value = raw.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
        return value[1:-1]
    return value


def required_field(fields: dict[str, str], name: str, owner: str) -> str:
    value = scalar(fields.get(name, ""))
    if not value:
        raise ValueError(f"{owner}: missing {name}")
    return value


def unique_fields(matches: list[tuple[str, str]], owner: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for name, raw_value in matches:
        if name in fields:
            raise ValueError(f"{owner}: duplicate field {name}")
        fields[name] = scalar(raw_value)
    return fields


def parse_lock(lock_path: Path) -> dict[str, object]:
    """Parse and validate the checked-in dependency lock without PyYAML."""

    text = lock_path.read_text(encoding="utf-8")
    schema_version = re.search(r"^schema_version:\s*([^\n]+)$", text, re.M)
    stage = re.search(r"^stage:\s*([^\n]+)$", text, re.M)
    policy = re.search(r"^policy:\s*([^\n]+)$", text, re.M)
    if not schema_version or scalar(schema_version.group(1)) != "1":
        raise ValueError("deps.lock must declare schema_version 1")
    if not stage or scalar(stage.group(1)) != "GRAFT-0002":
        raise ValueError("deps.lock must declare stage GRAFT-0002")
    if not policy or scalar(policy.group(1)) != "upstream-first":
        raise ValueError("deps.lock must declare policy upstream-first")

    toolchain_block = re.search(
        r"(?ms)^toolchain:\s*\n(.*?)(?=^dependencies:)", text
    )
    if not toolchain_block:
        raise ValueError("deps.lock must contain a toolchain block")
    toolchain_body = toolchain_block.group(1)
    toolchain_scalars = unique_fields(
        re.findall(r"^  ([A-Za-z0-9_]+):\s*([^\n]+)$", toolchain_body, re.M),
        "toolchain",
    )
    toolchain: dict[str, object] = {
        name: required_field(toolchain_scalars, name, "toolchain")
        for name in TOOLCHAIN_FIELDS
    }
    if toolchain["architecture"] != "linux/arm64":
        raise ValueError("toolchain.architecture must be linux/arm64")

    llvm_block = re.search(
        r"(?ms)^  llvm_mingw:\s*\n((?:^    [A-Za-z0-9_]+:\s*[^\n]*\n?)+)",
        toolchain_body,
    )
    if not llvm_block:
        raise ValueError("toolchain: missing llvm_mingw block")
    llvm_fields = unique_fields(
        re.findall(
            r"^    ([A-Za-z0-9_]+):\s*([^\n]+)$", llvm_block.group(1), re.M
        ),
        "toolchain.llvm_mingw",
    )
    llvm_mingw = {
        name: required_field(llvm_fields, name, "toolchain.llvm_mingw")
        for name in ("version", "archive", "archive_sha256")
    }
    if not re.fullmatch(r"[0-9a-f]{64}", llvm_mingw["archive_sha256"]):
        raise ValueError("toolchain.llvm_mingw.archive_sha256 must be a SHA-256")
    toolchain["llvm_mingw"] = llvm_mingw

    dependency_blocks = re.findall(
        r"(?ms)^  - name:\s*([^\n]+)\n(.*?)(?=^  - name:|^notes:|\Z)",
        text,
    )
    if not dependency_blocks:
        raise ValueError("deps.lock has no dependencies")
    dependencies: list[dict[str, str]] = []
    seen: set[str] = set()
    for raw_name, body in dependency_blocks:
        name = scalar(raw_name)
        if name in seen:
            raise ValueError(f"duplicate dependency: {name}")
        seen.add(name)
        fields = unique_fields(
            re.findall(r"^    ([A-Za-z0-9_]+):\s*([^\n]+)$", body, re.M),
            name,
        )
        dependency = {"name": name}
        dependency.update(
            {field: required_field(fields, field, name) for field in DEPENDENCY_FIELDS}
        )
        if not re.fullmatch(r"[0-9a-f]{40}", dependency["commit"]):
            raise ValueError(f"{name}: commit must be a 40-character SHA")
        if not re.fullmatch(r"[0-9a-f]{64}", dependency["archive_sha256"]):
            raise ValueError(f"{name}: archive_sha256 must be a SHA-256")
        dependencies.append(dependency)

    if {item["name"] for item in dependencies} != {"wine", "fex"}:
        raise ValueError("deps.lock must pin exactly wine and fex for G1")
    return {
        "source_lock": {
            "schema_version": 1,
            "stage": "GRAFT-0002",
            "policy": "upstream-first",
        },
        "toolchain": toolchain,
        "dependencies": dependencies,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def artifact_role(relative: str) -> str:
    if relative == "root/bin/wine":
        return "wine-loader"
    if PurePosixPath(relative).name == "libarm64ecfex.dll":
        return "fex-arm64ec-module"
    if PurePosixPath(relative).name == "libwow64fex.dll":
        return "fex-wow64-module"
    return "runtime-file"


def safe_relative_path(value: str, owner: str, *, root_prefix: bool = False) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or ".." in path.parts
        or "\\" in value
        or value.startswith("./")
        or path.as_posix() != value
    ):
        raise ValueError(f"{owner}: path must be a normalized relative path: {value!r}")
    if root_prefix and (not path.parts or path.parts[0] != "root"):
        raise ValueError(f"{owner}: artifact path must be under root/: {value!r}")
    return value


def collect_artifacts(runtime_dir: Path) -> list[dict[str, object]]:
    root = runtime_dir / "root"
    if not root.is_dir():
        raise ValueError(f"runtime root missing: {root}")
    if root.is_symlink():
        raise ValueError(f"runtime root must be a real directory: {root}")
    resolved_root = root.resolve()
    artifacts: list[dict[str, object]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_symlink() and not path.is_file():
            continue
        relative = path.relative_to(runtime_dir).as_posix()
        safe_relative_path(relative, "runtime artifact", root_prefix=True)
        if path.is_symlink():
            link_target = os.readlink(path)
            if PurePosixPath(link_target).is_absolute():
                raise ValueError(f"runtime symlink must be relative: {relative} -> {link_target}")
            try:
                resolved_target = path.resolve(strict=True)
            except OSError as error:
                raise ValueError(f"broken runtime symlink: {relative} -> {link_target}") from error
            if not resolved_target.is_relative_to(resolved_root):
                raise ValueError(f"runtime symlink escapes root/: {relative} -> {link_target}")
            kind = "symlink"
            digest = sha256_text(link_target)
        else:
            link_target = None
            kind = "file"
            digest = sha256(path)
        artifacts.append(
            {
                "path": relative,
                "sha256": digest,
                "role": artifact_role(relative),
                "kind": kind,
                "link_target": link_target,
            }
        )
    return artifacts


def patch_entries(repository_root: Path) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    patches_root = repository_root / "patches"
    if patches_root.is_symlink() or not patches_root.is_dir():
        raise ValueError(f"patches/ must be a real directory: {patches_root}")
    resolved_patches_root = patches_root.resolve()
    for dependency in ("wine", "fex"):
        patch_dir = patches_root / dependency
        if patch_dir.is_symlink() or not patch_dir.is_dir():
            raise ValueError(f"patch directory must be real: {patch_dir}")
        if patch_dir.resolve().parent != resolved_patches_root:
            raise ValueError(f"patch directory escapes patches/: {patch_dir}")
        for path in sorted(patch_dir.glob("*.patch")):
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"patch must be a regular file: {path}")
            entries.append(
                {
                    "dependency": dependency,
                    "path": path.relative_to(repository_root).as_posix(),
                    "sha256": sha256(path),
                }
            )
    return entries


def applied_patch_entries(runtime_dir: Path, repository_root: Path) -> list[dict[str, str]]:
    expected = patch_entries(repository_root)
    state_path = runtime_dir / "applied-patches.json"
    if not state_path.is_file():
        if expected:
            raise ValueError("patches exist but applied-patches.json is missing")
        return []
    if state_path.is_symlink():
        raise ValueError("applied-patches.json must be a regular file")
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid applied-patches.json: {error}") from error
    if state != expected:
        raise ValueError("applied-patches.json does not match patches/")
    return expected


def resolve_file_inside(path: Path, root: Path, owner: str) -> Path:
    try:
        resolved_root = root.resolve(strict=True)
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise ValueError(f"{owner} does not exist: {path}") from error
    if not resolved.is_relative_to(resolved_root):
        raise ValueError(f"{owner} escapes {root}: {path}")
    if not resolved.is_file():
        raise ValueError(f"{owner} must resolve to a regular file: {path}")
    return resolved


def validate_test_report(raw_tests: object, runtime_dir: Path) -> list[dict[str, str]]:
    if not isinstance(raw_tests, list):
        raise ValueError("test report must contain a JSON array")
    tests: list[dict[str, str]] = []
    seen: set[str] = set()
    for index, raw_test in enumerate(raw_tests):
        if not isinstance(raw_test, dict) or set(raw_test) != {"name", "status", "log"}:
            raise ValueError(f"test[{index}] must contain exactly name, status, and log")
        if not all(isinstance(raw_test[field], str) for field in ("name", "status", "log")):
            raise ValueError(f"test[{index}] fields must be strings")
        name = raw_test["name"]
        if not name or name in seen:
            raise ValueError(f"test[{index}] has an empty or duplicate name: {name!r}")
        if raw_test["status"] not in TEST_STATUSES:
            raise ValueError(f"test[{index}] has invalid status: {raw_test['status']}")
        relative_log = safe_relative_path(raw_test["log"], f"test[{index}].log")
        log_path = resolve_file_inside(
            runtime_dir / relative_log,
            runtime_dir,
            f"test[{index}] log",
        )
        seen.add(name)
        tests.append(
            {
                "name": name,
                "status": raw_test["status"],
                "log": relative_log,
                "sha256": sha256(log_path),
            }
        )
    return tests


def load_tests(test_report: Path | None, runtime_dir: Path) -> list[dict[str, str]]:
    if test_report is None:
        return []
    try:
        raw_tests = json.loads(test_report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid test report: {error}") from error
    return validate_test_report(raw_tests, runtime_dir)


def validate_manifest_tests(raw_tests: object, runtime_dir: Path) -> list[dict[str, str]]:
    if not isinstance(raw_tests, list):
        raise ValueError("runtime manifest tests must be an array")
    tests: list[dict[str, str]] = []
    seen: set[str] = set()
    required_fields = {"name", "status", "log", "sha256"}
    for index, raw_test in enumerate(raw_tests):
        if not isinstance(raw_test, dict) or set(raw_test) != required_fields:
            raise ValueError(f"test[{index}] has invalid fields")
        if not all(isinstance(raw_test[field], str) for field in required_fields):
            raise ValueError(f"test[{index}] fields must be strings")
        name = raw_test["name"]
        if not name or name in seen:
            raise ValueError(f"test[{index}] has an empty or duplicate name: {name!r}")
        if raw_test["status"] not in TEST_STATUSES:
            raise ValueError(f"test[{index}] has invalid status: {raw_test['status']}")
        relative_log = safe_relative_path(raw_test["log"], f"test[{index}].log")
        if not re.fullmatch(r"[0-9a-f]{64}", raw_test["sha256"]):
            raise ValueError(f"test[{index}] has an invalid SHA-256")
        log_path = resolve_file_inside(
            runtime_dir / relative_log,
            runtime_dir,
            f"test[{index}] log",
        )
        if sha256(log_path) != raw_test["sha256"]:
            raise ValueError(f"test[{index}] log SHA-256 mismatch: {relative_log}")
        seen.add(name)
        tests.append({field: raw_test[field] for field in ("name", "status", "log", "sha256")})
    return tests


def validate_elf64_aarch64(path: Path, owner: str) -> None:
    with path.open("rb") as stream:
        header = stream.read(64)
        stream.seek(0, os.SEEK_END)
        file_size = stream.tell()
    if (
        len(header) < 64
        or header[:4] != b"\x7fELF"
        or header[4] != 2
        or header[5] != 1
        or header[6] != 1
    ):
        raise ValueError(f"{owner} must be an ELF64 little-endian AArch64 binary")
    (
        elf_type,
        machine,
        version,
        _entry,
        program_offset,
        _section_offset,
        _flags,
        header_size,
        program_entry_size,
        program_count,
        _section_entry_size,
        _section_count,
        _section_names,
    ) = struct.unpack_from("<HHIQQQIHHHHHH", header, 16)
    if (
        elf_type not in {2, 3}
        or machine != 183
        or version != 1
        or header_size != 64
        or program_entry_size != 56
        or program_count == 0
        or program_offset < 64
        or program_offset + program_entry_size * program_count > file_size
    ):
        raise ValueError(f"{owner} has an invalid AArch64 ELF64 header")
    has_load_segment = False
    with path.open("rb") as stream:
        stream.seek(program_offset)
        for _ in range(program_count):
            program_header = stream.read(program_entry_size)
            if len(program_header) != program_entry_size:
                raise ValueError(f"{owner} has a truncated ELF program table")
            (
                segment_type,
                _segment_flags,
                file_offset,
                _virtual_address,
                _physical_address,
                file_bytes,
                memory_bytes,
                _alignment,
            ) = struct.unpack_from("<IIQQQQQQ", program_header)
            if file_offset + file_bytes > file_size or memory_bytes < file_bytes:
                raise ValueError(f"{owner} has an invalid ELF program segment")
            has_load_segment = has_load_segment or segment_type == 1
    if not has_load_segment:
        raise ValueError(f"{owner} must contain an ELF PT_LOAD segment")


def validate_pe_machine(path: Path, owner: str, expected_machine: int) -> None:
    with path.open("rb") as stream:
        dos_header = stream.read(64)
        if len(dos_header) < 64 or dos_header[:2] != b"MZ":
            raise ValueError(f"{owner} must be a PE binary")
        pe_offset = struct.unpack_from("<I", dos_header, 0x3C)[0]
        if pe_offset < 64:
            raise ValueError(f"{owner} has an invalid PE header offset")
        stream.seek(pe_offset)
        pe_header = stream.read(24)
        stream.seek(0, os.SEEK_END)
        file_size = stream.tell()
    if len(pe_header) < 24 or pe_header[:4] != b"PE\0\0":
        raise ValueError(f"{owner} must be a PE binary")
    (
        machine,
        section_count,
        _timestamp,
        _symbol_table,
        _symbol_count,
        optional_header_size,
        _characteristics,
    ) = struct.unpack_from("<HHIIIHH", pe_header, 4)
    if machine != expected_machine:
        raise ValueError(
            f"{owner} has PE machine 0x{machine:04x}; expected 0x{expected_machine:04x}"
        )
    optional_offset = pe_offset + 24
    section_offset = optional_offset + optional_header_size
    if (
        section_count == 0
        or optional_header_size < 2
        or section_offset + section_count * 40 > file_size
    ):
        raise ValueError(f"{owner} has an invalid PE section table")
    with path.open("rb") as stream:
        stream.seek(optional_offset)
        optional_magic = stream.read(2)
        if optional_magic != b"\x0b\x02":
            raise ValueError(f"{owner} must use the PE32+ optional header")
        stream.seek(section_offset)
        for _ in range(section_count):
            section = stream.read(40)
            if len(section) != 40:
                raise ValueError(f"{owner} has a truncated PE section table")
            raw_size, raw_offset = struct.unpack_from("<II", section, 16)
            if raw_size and (raw_offset < section_offset + section_count * 40 or raw_offset + raw_size > file_size):
                raise ValueError(f"{owner} has an invalid PE section payload")


def validate_required_artifacts(
    runtime_dir: Path,
    artifacts: list[dict[str, object]],
) -> None:
    by_role = {
        role: [artifact for artifact in artifacts if artifact["role"] == role]
        for role in REQUIRED_ARTIFACT_ROLES
    }
    missing_roles = sorted(role for role, matches in by_role.items() if not matches)
    if missing_roles:
        raise ValueError(f"runtime is missing required artifact roles: {missing_roles}")
    duplicate_roles = sorted(role for role, matches in by_role.items() if len(matches) != 1)
    if duplicate_roles:
        raise ValueError(f"runtime has ambiguous required artifact roles: {duplicate_roles}")

    resolved_root = runtime_dir / "root"
    required_files = {
        role: resolve_file_inside(
            runtime_dir / str(matches[0]["path"]),
            resolved_root,
            role,
        )
        for role, matches in by_role.items()
    }
    wine_path = required_files["wine-loader"]
    if not os.access(wine_path, os.X_OK):
        raise ValueError("root/bin/wine must be executable")
    validate_elf64_aarch64(wine_path, "root/bin/wine")
    validate_pe_machine(
        required_files["fex-arm64ec-module"],
        "libarm64ecfex.dll",
        0xA641,
    )
    validate_pe_machine(
        required_files["fex-wow64-module"],
        "libwow64fex.dll",
        0xAA64,
    )


def write_checksum_manifest(runtime_dir: Path, artifacts: list[dict[str, object]]) -> None:
    output = runtime_dir / "runtime-manifest.sha256"
    lines = [f"{artifact['sha256']}  {artifact['path']}" for artifact in artifacts]
    output.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")


def generate_manifest(
    runtime_dir: Path,
    lock: dict[str, object],
    tests: list[dict[str, str]],
    repository_root: Path,
) -> Path:
    artifacts = collect_artifacts(runtime_dir)
    validate_required_artifacts(runtime_dir, artifacts)
    manifest = {
        "schema_version": 1,
        "stage": "G1",
        "platform": {"os": "linux", "architecture": "aarch64"},
        "source_lock": lock["source_lock"],
        "toolchain": lock["toolchain"],
        "dependencies": lock["dependencies"],
        "patches": applied_patch_entries(runtime_dir, repository_root),
        "artifacts": artifacts,
        "tests": tests,
    }
    output = runtime_dir / "runtime-manifest.json"
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_checksum_manifest(runtime_dir, artifacts)
    return output


def parse_checksum_manifest(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ValueError(f"cannot read checksum manifest: {error}") from error
    if not lines:
        raise ValueError("runtime checksum manifest is empty")
    entries: dict[str, str] = {}
    for index, line in enumerate(lines, 1):
        match = re.fullmatch(r"([0-9a-f]{64})\s+(.+)", line)
        if not match:
            raise ValueError(f"invalid runtime checksum line {index}")
        digest, relative = match.groups()
        safe_relative_path(relative, f"checksum line {index}", root_prefix=True)
        if relative in entries:
            raise ValueError(f"duplicate runtime checksum path: {relative}")
        entries[relative] = digest
    return entries


def verify_manifest(runtime_dir: Path, lock: dict[str, object], repository_root: Path) -> None:
    manifest_path = runtime_dir / "runtime-manifest.json"
    checksum_path = runtime_dir / "runtime-manifest.sha256"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid runtime manifest: {error}") from error
    required_keys = {
        "schema_version",
        "stage",
        "platform",
        "source_lock",
        "toolchain",
        "dependencies",
        "patches",
        "artifacts",
        "tests",
    }
    if not isinstance(manifest, dict) or set(manifest) != required_keys:
        raise ValueError("runtime manifest has missing or unexpected top-level fields")
    if manifest["schema_version"] != 1 or manifest["stage"] != "G1":
        raise ValueError("runtime manifest must declare schema_version 1 and stage G1")
    if manifest["platform"] != {"os": "linux", "architecture": "aarch64"}:
        raise ValueError("runtime manifest platform must be linux/aarch64")
    if manifest["source_lock"] != lock["source_lock"]:
        raise ValueError("runtime manifest source_lock does not match deps.lock")
    if manifest["toolchain"] != lock["toolchain"]:
        raise ValueError("runtime manifest toolchain does not match deps.lock")
    if manifest["dependencies"] != lock["dependencies"]:
        raise ValueError("runtime manifest dependencies do not match deps.lock")
    if manifest["patches"] != applied_patch_entries(runtime_dir, repository_root):
        raise ValueError("runtime manifest applied patch series does not match build state")

    raw_artifacts = manifest["artifacts"]
    if not isinstance(raw_artifacts, list):
        raise ValueError("runtime manifest artifacts must be an array")
    artifacts: list[dict[str, object]] = []
    seen_paths: set[str] = set()
    for index, artifact in enumerate(raw_artifacts):
        if not isinstance(artifact, dict) or set(artifact) != {
            "path",
            "sha256",
            "role",
            "kind",
            "link_target",
        }:
            raise ValueError(f"artifact[{index}] has invalid fields")
        if not all(
            isinstance(artifact[field], str)
            for field in ("path", "sha256", "role", "kind")
        ):
            raise ValueError(f"artifact[{index}] path, hash, role, and kind must be strings")
        relative = safe_relative_path(artifact["path"], f"artifact[{index}]", root_prefix=True)
        if relative in seen_paths:
            raise ValueError(f"duplicate artifact path: {relative}")
        if not re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]):
            raise ValueError(f"artifact[{index}] has an invalid SHA-256")
        if artifact["role"] != artifact_role(relative):
            raise ValueError(f"artifact[{index}] role does not match its path")
        if artifact["kind"] == "file":
            if artifact["link_target"] is not None:
                raise ValueError(f"artifact[{index}] regular file must have a null link_target")
        elif artifact["kind"] == "symlink":
            if not isinstance(artifact["link_target"], str) or not artifact["link_target"]:
                raise ValueError(f"artifact[{index}] symlink must have a link_target")
            if PurePosixPath(artifact["link_target"]).is_absolute():
                raise ValueError(f"artifact[{index}] symlink target must be relative")
        else:
            raise ValueError(f"artifact[{index}] has invalid kind: {artifact['kind']}")
        seen_paths.add(relative)
        artifacts.append(
            {
                field: artifact[field]
                for field in ("path", "sha256", "role", "kind", "link_target")
            }
        )

    actual_artifacts = collect_artifacts(runtime_dir)
    if artifacts != actual_artifacts:
        expected = {str(item["path"]): item for item in artifacts}
        actual = {str(item["path"]): item for item in actual_artifacts}
        changed = sorted(path for path in expected.keys() & actual.keys() if expected[path] != actual[path])
        missing = sorted(expected.keys() - actual.keys())
        unexpected = sorted(actual.keys() - expected.keys())
        raise ValueError(
            "runtime artifact set or SHA-256 mismatch "
            f"(changed={changed}, missing={missing}, unexpected={unexpected})"
        )

    validate_required_artifacts(runtime_dir, artifacts)

    checksum_entries = parse_checksum_manifest(checksum_path)
    artifact_entries = {
        str(artifact["path"]): str(artifact["sha256"])
        for artifact in artifacts
    }
    if checksum_entries != artifact_entries:
        raise ValueError("runtime checksum manifest does not match runtime-manifest.json")

    validate_manifest_tests(manifest["tests"], runtime_dir)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime_dir", nargs="?", type=Path)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--test-report", type=Path)
    parser.add_argument("--patch-root", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify-existing", action="store_true")
    mode.add_argument("--validate-lock-only", action="store_true")
    args = parser.parse_args()

    try:
        lock = parse_lock(args.lock)
        if args.validate_lock_only:
            print(f"Dependency lock verified: {args.lock}")
            return 0
        if args.runtime_dir is None:
            parser.error("runtime_dir is required unless --validate-lock-only is used")
        runtime_dir = args.runtime_dir.resolve()
        repository_root = (
            args.patch_root or Path(__file__).resolve().parents[1]
        ).resolve()
        if args.verify_existing:
            verify_manifest(runtime_dir, lock, repository_root)
            print(f"Runtime artifact structure and SHA-256 verified: {runtime_dir}")
            return 0
        output = generate_manifest(
            runtime_dir,
            lock,
            load_tests(args.test_report, runtime_dir),
            repository_root,
        )
        print(f"Wrote {output} and {runtime_dir / 'runtime-manifest.sha256'}")
        return 0
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    raise SystemExit(main())
