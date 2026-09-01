import copy
import hashlib
import json
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).parents[2]
GENERATOR = ROOT / "scripts" / "generate-runtime-manifest.py"
VERIFIER = ROOT / "scripts" / "verify-artifacts.sh"
LOCK = ROOT / "third_party" / "manifest" / "deps.lock"
SCHEMA = ROOT / "runtime" / "packaging" / "runtime-manifest.schema.json"


def completed(command, *, environment=None):
    return subprocess.run(
        [str(item) for item in command],
        cwd=ROOT,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
    )


def run(command, *, environment=None):
    result = completed(command, environment=environment)
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def expect_failure(command, *, environment=None):
    result = completed(command, environment=environment)
    assert result.returncode != 0, result.stdout + result.stderr
    return result.stdout + result.stderr


def expect_generation_and_verification_failure(generate, verify, *, environment=None):
    result = completed(generate, environment=environment)
    assert result.returncode != 0, result.stdout + result.stderr
    expect_failure(verify, environment=environment)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sha256_text(value):
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_elf64_aarch64(path, machine=183):
    image = bytearray(64 + 56 + 16)
    image[0:4] = b"\x7fELF"
    image[4] = 2  # ELFCLASS64
    image[5] = 1  # ELFDATA2LSB
    image[6] = 1  # EV_CURRENT
    struct.pack_into(
        "<HHIQQQIHHHHHH",
        image,
        16,
        2,
        machine,
        1,
        0,
        64,
        0,
        0,
        64,
        56,
        1,
        0,
        0,
        0,
    )
    struct.pack_into(
        "<IIQQQQQQ",
        image,
        64,
        1,
        5,
        0,
        0,
        0,
        len(image),
        len(image),
        0x1000,
    )
    path.write_bytes(image)
    os.chmod(path, 0o755)


def write_pe(path, machine):
    image = bytearray(0x210)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, machine, 1, 0, 0, 0, 0xF0, 0x2022)
    struct.pack_into("<H", image, 0x98, 0x20B)
    section_offset = 0x98 + 0xF0
    image[section_offset : section_offset + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", image, section_offset + 8, 0x10, 0x1000, 0x10, 0x200)
    struct.pack_into("<I", image, section_offset + 36, 0x60000020)
    path.write_bytes(image)


class SchemaValidationError(AssertionError):
    pass


def schema_matches(instance, schema):
    try:
        validate_schema(instance, schema)
    except SchemaValidationError:
        return False
    return True


def validate_schema(instance, schema, path="$"):
    """Validate the dependency-free Draft 2020-12 subset used by this schema."""

    expected_type = schema.get("type")
    type_matches = {
        "object": lambda value: isinstance(value, dict),
        "array": lambda value: isinstance(value, list),
        "string": lambda value: isinstance(value, str),
        "null": lambda value: value is None,
    }
    if expected_type is not None:
        predicate = type_matches[expected_type]
        if not predicate(instance):
            raise SchemaValidationError(f"{path}: expected {expected_type}")

    if "const" in schema and instance != schema["const"]:
        raise SchemaValidationError(f"{path}: value does not match const")
    if "enum" in schema and instance not in schema["enum"]:
        raise SchemaValidationError(f"{path}: value is not in enum")

    if isinstance(instance, str):
        if len(instance) < schema.get("minLength", 0):
            raise SchemaValidationError(f"{path}: string is too short")
        if "pattern" in schema and re.search(schema["pattern"], instance) is None:
            raise SchemaValidationError(f"{path}: string does not match pattern")

    if isinstance(instance, dict):
        properties = schema.get("properties", {})
        missing = set(schema.get("required", [])) - set(instance)
        if missing:
            raise SchemaValidationError(f"{path}: missing fields {sorted(missing)}")
        for name, property_schema in properties.items():
            if name in instance:
                validate_schema(instance[name], property_schema, f"{path}.{name}")
        if schema.get("additionalProperties") is False:
            unexpected = set(instance) - set(properties)
            if unexpected:
                raise SchemaValidationError(
                    f"{path}: unexpected fields {sorted(unexpected)}"
                )

    if isinstance(instance, list):
        if len(instance) < schema.get("minItems", 0):
            raise SchemaValidationError(f"{path}: array has too few items")
        if "items" in schema:
            for index, item in enumerate(instance):
                validate_schema(item, schema["items"], f"{path}[{index}]")

    for index, member_schema in enumerate(schema.get("allOf", [])):
        validate_schema(instance, member_schema, f"{path}.allOf[{index}]")

    if "if" in schema and schema_matches(instance, schema["if"]):
        validate_schema(instance, schema.get("then", {}), f"{path}.then")

    if "contains" in schema:
        if not isinstance(instance, list):
            raise SchemaValidationError(f"{path}: contains requires an array")
        match_count = sum(
            schema_matches(item, schema["contains"]) for item in instance
        )
        minimum = schema.get("minContains", 1)
        maximum = schema.get("maxContains")
        if match_count < minimum or (maximum is not None and match_count > maximum):
            raise SchemaValidationError(
                f"{path}: contains matched {match_count}, expected {minimum}..{maximum}"
            )


def assert_schema_rejects(instance, schema):
    try:
        validate_schema(instance, schema)
    except SchemaValidationError:
        return
    raise AssertionError("schema unexpectedly accepted an invalid manifest")


with tempfile.TemporaryDirectory(prefix="graft64-runtime-manifest-") as temporary:
    fixture_root = Path(temporary)
    runtime = fixture_root / "runtime"
    patch_root = fixture_root / "repository"
    wine = runtime / "root" / "bin" / "wine"
    arm64ec = runtime / "root" / "lib" / "wine" / "libarm64ecfex.dll"
    wow64 = runtime / "root" / "lib" / "wine" / "libwow64fex.dll"
    for path in (wine, arm64ec, wow64):
        path.parent.mkdir(parents=True, exist_ok=True)
    write_elf64_aarch64(wine)
    write_pe(arm64ec, 0xA641)
    write_pe(wow64, 0xAA64)

    alias = runtime / "root" / "lib" / "wine" / "current-arm64ec.dll"
    alias.symlink_to("libarm64ecfex.dll")

    log = runtime / "logs" / "hello-arm64.log"
    log.parent.mkdir(parents=True)
    log.write_text("GRAFT64_HELLO_ARM64\n", encoding="utf-8")
    test_report = runtime / "g1-baseline-tests.json"
    write_json(
        test_report,
        [{"name": "hello-arm64", "status": "PASS", "log": "logs/hello-arm64.log"}],
    )

    patch = patch_root / "patches" / "wine" / "0001-fixture.patch"
    patch.parent.mkdir(parents=True)
    patch.write_text("fixture patch\n", encoding="utf-8")
    (patch_root / "patches" / "fex").mkdir(parents=True)
    applied_patches = runtime / "applied-patches.json"
    expected_patch_state = [
        {
            "dependency": "wine",
            "path": "patches/wine/0001-fixture.patch",
            "sha256": sha256(patch),
        }
    ]

    generate = [
        "python3",
        GENERATOR,
        runtime,
        "--lock",
        LOCK,
        "--test-report",
        test_report,
        "--patch-root",
        patch_root,
    ]
    environment = os.environ.copy()
    environment["GRAFT_DEPS_LOCK"] = str(LOCK)
    environment["GRAFT_PATCH_ROOT"] = str(patch_root)
    verify = [VERIFIER, runtime]

    # A checked-in patch is not evidence that the build actually applied it.
    expect_failure(generate, environment=environment)
    write_json(applied_patches, expected_patch_state)

    run(generate, environment=environment)
    manifest_path = runtime / "runtime-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["stage"] == "G1"
    assert manifest["source_lock"] == {
        "schema_version": 1,
        "stage": "GRAFT-0002",
        "policy": "upstream-first",
    }
    assert manifest["toolchain"]["architecture"] == "linux/arm64"
    assert manifest["toolchain"]["llvm_mingw"]["archive_sha256"]
    assert {item["name"] for item in manifest["dependencies"]} == {"wine", "fex"}
    assert all(
        item["repository"].startswith("https://") for item in manifest["dependencies"]
    )
    assert manifest["patches"] == expected_patch_state
    assert manifest["tests"] == [
        {
            "name": "hello-arm64",
            "status": "PASS",
            "log": "logs/hello-arm64.log",
            "sha256": sha256(log),
        }
    ]

    artifacts_by_path = {item["path"]: item for item in manifest["artifacts"]}
    assert set(artifacts_by_path["root/bin/wine"]) == {
        "path",
        "sha256",
        "role",
        "kind",
        "link_target",
    }
    assert artifacts_by_path["root/bin/wine"]["role"] == "wine-loader"
    assert artifacts_by_path["root/bin/wine"]["kind"] == "file"
    assert artifacts_by_path["root/bin/wine"]["link_target"] is None
    assert artifacts_by_path["root/lib/wine/current-arm64ec.dll"] == {
        "path": "root/lib/wine/current-arm64ec.dll",
        "sha256": sha256_text("libarm64ecfex.dll"),
        "role": "runtime-file",
        "kind": "symlink",
        "link_target": "libarm64ecfex.dll",
    }
    assert {item["role"] for item in manifest["artifacts"]} >= {
        "wine-loader",
        "fex-arm64ec-module",
        "fex-wow64-module",
    }

    checksum_lines = (runtime / "runtime-manifest.sha256").read_text(
        encoding="utf-8"
    ).splitlines()
    assert checksum_lines
    assert all("  root/" in line and temporary not in line for line in checksum_lines)

    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    validate_schema(manifest, schema)

    invalid = copy.deepcopy(manifest)
    invalid["artifacts"][0].pop("kind")
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    next(item for item in invalid["artifacts"] if item["kind"] == "file")[
        "link_target"
    ] = "not-null"
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    next(item for item in invalid["artifacts"] if item["kind"] == "symlink")[
        "link_target"
    ] = None
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    next(
        item
        for item in invalid["artifacts"]
        if item["role"] == "fex-wow64-module"
    )["role"] = "runtime-file"
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    invalid["tests"][0].pop("sha256")
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    invalid["tests"][0]["log"] = "/tmp/not-relative.log"
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    invalid["artifacts"][0]["path"] = "root/../outside"
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    invalid["artifacts"][0]["path"] = "root/lib/odd\\name"
    assert_schema_rejects(invalid, schema)
    invalid = copy.deepcopy(manifest)
    invalid["patches"][0]["component"] = invalid["patches"][0].pop("dependency")
    assert_schema_rejects(invalid, schema)

    run(verify, environment=environment)

    original_wine = wine.read_bytes()
    tampered_wine = bytearray(original_wine)
    tampered_wine[-1] ^= 0x01
    wine.write_bytes(tampered_wine)
    os.chmod(wine, 0o755)
    expect_failure(verify, environment=environment)
    wine.write_bytes(original_wine)
    os.chmod(wine, 0o755)
    run(generate, environment=environment)

    log.write_text("tampered baseline log\n", encoding="utf-8")
    expect_failure(verify, environment=environment)
    log.write_text("GRAFT64_HELLO_ARM64\n", encoding="utf-8")
    run(generate, environment=environment)

    for path in (wine, arm64ec, wow64):
        original = path.read_bytes()
        path.write_bytes(b"text pretending to be a runtime binary\n")
        if path == wine:
            os.chmod(path, 0o755)
        expect_generation_and_verification_failure(
            generate, verify, environment=environment
        )
        path.write_bytes(original)
        if path == wine:
            os.chmod(path, 0o755)
        run(generate, environment=environment)

    for path, truncated_size in ((wine, 64), (arm64ec, 0x98), (wow64, 0x98)):
        original = path.read_bytes()
        path.write_bytes(original[:truncated_size])
        if path == wine:
            os.chmod(path, 0o755)
        expect_generation_and_verification_failure(
            generate, verify, environment=environment
        )
        path.write_bytes(original)
        if path == wine:
            os.chmod(path, 0o755)
        run(generate, environment=environment)

    wrong_machine_fixtures = (
        (wine, lambda: write_elf64_aarch64(wine, machine=62)),
        (arm64ec, lambda: write_pe(arm64ec, 0xAA64)),
        (wow64, lambda: write_pe(wow64, 0x8664)),
    )
    for path, write_wrong_machine in wrong_machine_fixtures:
        original = path.read_bytes()
        write_wrong_machine()
        expect_generation_and_verification_failure(
            generate, verify, environment=environment
        )
        path.write_bytes(original)
        if path == wine:
            os.chmod(path, 0o755)
        run(generate, environment=environment)

    os.chmod(wine, 0o644)
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    os.chmod(wine, 0o755)
    run(generate, environment=environment)

    uppercase_wine = wine.with_name("WINE")
    wine.rename(uppercase_wine)
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    uppercase_wine.rename(wine)
    run(generate, environment=environment)

    backslash_artifact = runtime / "root" / "lib" / "odd\\name"
    backslash_artifact.write_bytes(b"runtime file\n")
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    backslash_artifact.unlink()
    run(generate, environment=environment)

    absolute_link = runtime / "root" / "lib" / "absolute-link"
    absolute_link.symlink_to("/tmp/graft64-absolute-link-target")
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    absolute_link.unlink()
    run(generate, environment=environment)

    broken_link = runtime / "root" / "lib" / "broken-link"
    broken_link.symlink_to("missing-target")
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    broken_link.unlink()
    run(generate, environment=environment)

    outside = runtime / "outside-root.bin"
    outside.write_bytes(b"outside\n")
    escaping_link = runtime / "root" / "lib" / "escaping-link"
    escaping_link.symlink_to("../../outside-root.bin")
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    escaping_link.unlink()
    outside.unlink()
    run(generate, environment=environment)

    wine_patch_dir = patch_root / "patches" / "wine"
    saved_wine_patch_dir = patch_root / "patches" / "wine-saved"
    external_patch_dir = fixture_root / "external-wine-patches"
    external_patch_dir.mkdir()
    (external_patch_dir / patch.name).write_text("external patch\n", encoding="utf-8")
    wine_patch_dir.rename(saved_wine_patch_dir)
    wine_patch_dir.symlink_to(external_patch_dir, target_is_directory=True)
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    wine_patch_dir.unlink()
    saved_wine_patch_dir.rename(wine_patch_dir)
    run(generate, environment=environment)

    applied_patches.unlink()
    expect_failure(generate, environment=environment)
    expect_failure(verify, environment=environment)
    write_json(applied_patches, expected_patch_state)
    run(generate, environment=environment)

    drifted_patch_state = copy.deepcopy(expected_patch_state)
    drifted_patch_state[0]["sha256"] = "0" * 64
    write_json(applied_patches, drifted_patch_state)
    expect_failure(generate, environment=environment)
    expect_failure(verify, environment=environment)
    write_json(applied_patches, expected_patch_state)
    run(generate, environment=environment)

    patch.write_text("tampered fixture patch\n", encoding="utf-8")
    expect_failure(generate, environment=environment)
    expect_failure(verify, environment=environment)
    patch.write_text("fixture patch\n", encoding="utf-8")
    write_json(applied_patches, expected_patch_state)
    run(generate, environment=environment)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["dependencies"][0]["commit"] = "0" * 40
    write_json(manifest_path, manifest)
    expect_failure(verify, environment=environment)
    run(generate, environment=environment)

    checksum_path = runtime / "runtime-manifest.sha256"
    checksum_lines = checksum_path.read_text(encoding="utf-8").splitlines()
    checksum_lines[0] = "0" * 64 + checksum_lines[0][64:]
    checksum_path.write_text("\n".join(checksum_lines) + "\n", encoding="utf-8")
    expect_failure(verify, environment=environment)
    run(generate, environment=environment)

    original_arm64ec = arm64ec.read_bytes()
    arm64ec.unlink()
    expect_generation_and_verification_failure(
        generate, verify, environment=environment
    )
    arm64ec.write_bytes(original_arm64ec)
    run(generate, environment=environment)
    run(verify, environment=environment)

print(
    "Runtime manifest schema, binary formats, links, patch state, and hashes passed"
)
