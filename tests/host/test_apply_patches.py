import hashlib
import io
import json
import os
import re
import subprocess
import tarfile
import tempfile
from pathlib import Path


ROOT = Path(__file__).parents[2]
APPLY_PATCHES = ROOT / "scripts" / "apply-patches.sh"
HEADER = "name\tref\tcommit\tarchive_sha256\tarchive\n"


def run_apply(environment):
    return subprocess.run(
        [str(APPLY_PATCHES)],
        cwd=ROOT,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )


def expect_failure(environment, needle):
    completed = subprocess.run(
        [str(APPLY_PATCHES)],
        cwd=ROOT,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
    )
    assert completed.returncode != 0, completed.stdout
    output = completed.stdout + completed.stderr
    assert needle in output, output


def write_manifest(path, wine_commit, fex_commit, wine_archive_sha, fex_archive_sha):
    # Deliberately list FEX first; apply-patches owns the canonical Wine/FEX order.
    path.write_text(
        HEADER
        + f"fex\tfex-ref\t{fex_commit}\t{fex_archive_sha}\thttps://example.invalid/fex.tar.gz\n"
        + f"wine\twine-ref\t{wine_commit}\t{wine_archive_sha}\thttps://example.invalid/wine.tar.gz\n",
        encoding="utf-8",
    )


def write_lock(path, wine_commit, fex_commit, wine_archive_sha, fex_archive_sha):
    path.write_text(
        "schema_version: 1\n"
        "stage: GRAFT-0002\n"
        "policy: upstream-first\n"
        "dependencies:\n"
        f"  - name: wine\n    ref: wine-ref\n    commit: {wine_commit}\n"
        f"    archive: https://example.invalid/wine.tar.gz\n    archive_sha256: {wine_archive_sha}\n"
        f"  - name: fex\n    ref: fex-ref\n    commit: {fex_commit}\n"
        f"    archive: https://example.invalid/fex.tar.gz\n    archive_sha256: {fex_archive_sha}\n",
        encoding="utf-8",
    )


def write_archive(path, dependency, contents="base\n"):
    payload = contents.encode("utf-8")
    with tarfile.open(path, "w:gz") as archive:
        member = tarfile.TarInfo(f"{dependency}-source/{dependency}.txt")
        member.size = len(payload)
        member.mode = 0o644
        member.mtime = 0
        archive.addfile(member, io.BytesIO(payload))


def write_patch(path, dependency, before, after):
    filename = f"{dependency}.txt"
    path.write_text(
        f"diff --git a/{filename} b/{filename}\n"
        f"--- a/{filename}\n"
        f"+++ b/{filename}\n"
        "@@ -1 +1 @@\n"
        f"-{before}\n"
        f"+{after}\n",
        encoding="utf-8",
    )


with tempfile.TemporaryDirectory(prefix="graft64-apply-patches-") as temporary:
    fixture = Path(temporary)
    upstream = fixture / "upstream"
    runtime = fixture / "runtime"
    patch_root = fixture / "repository"
    wine_commit = "1" * 40
    fex_commit = "2" * 40
    upstream.mkdir()
    downloads = upstream / "downloads"
    downloads.mkdir()

    for dependency, commit in (("wine", wine_commit), ("fex", fex_commit)):
        source = upstream / f"{dependency}-{commit}"
        source.mkdir()
        (source / f"{dependency}.txt").write_text(
            "tampered extracted source\n", encoding="utf-8"
        )
        write_archive(downloads / f"{dependency}-{commit}.tar.gz", dependency)
        (patch_root / "patches" / dependency).mkdir(parents=True)

    # Extra versioned directories prove the source mapping does not use a glob/find first match.
    (upstream / f"wine-{'0' * 40}").mkdir()
    (upstream / f"fex-{'0' * 40}").mkdir()
    manifest_path = upstream / "fetched-manifest.tsv"
    lock_path = fixture / "deps.lock"
    wine_archive = downloads / f"wine-{wine_commit}.tar.gz"
    fex_archive = downloads / f"fex-{fex_commit}.tar.gz"
    wine_archive_sha = hashlib.sha256(wine_archive.read_bytes()).hexdigest()
    fex_archive_sha = hashlib.sha256(fex_archive.read_bytes()).hexdigest()
    write_manifest(
        manifest_path,
        wine_commit,
        fex_commit,
        wine_archive_sha,
        fex_archive_sha,
    )
    write_lock(lock_path, wine_commit, fex_commit, wine_archive_sha, fex_archive_sha)

    environment = os.environ.copy()
    environment.update(
        {
            "GRAFT_UPSTREAM_DIR": str(upstream),
            "GRAFT_RUNTIME_OUT": str(runtime),
            "GRAFT_PATCH_ROOT": str(patch_root),
            "GRAFT_DEPS_LOCK": str(lock_path),
        }
    )

    run_apply(environment)
    state_path = runtime / "applied-patches.json"
    assert json.loads(state_path.read_text(encoding="utf-8")) == []
    prepared_wine = runtime / "build" / "sources" / f"wine-{wine_commit}" / "wine.txt"
    prepared_fex = runtime / "build" / "sources" / f"fex-{fex_commit}" / "fex.txt"
    assert prepared_wine.read_text(encoding="utf-8") == "base\n"
    assert prepared_fex.read_text(encoding="utf-8") == "base\n"

    wine_first = patch_root / "patches" / "wine" / "0001-first.patch"
    wine_second = patch_root / "patches" / "wine" / "0002-second.patch"
    fex_patch = patch_root / "patches" / "fex" / "0001-fex.patch"
    # Create these out of order; filenames define replay and manifest order.
    write_patch(wine_second, "wine", "first", "second")
    write_patch(fex_patch, "fex", "base", "fex-patched")
    write_patch(wine_first, "wine", "base", "first")

    run_apply(environment)
    assert prepared_wine.read_text(encoding="utf-8") == "second\n"
    assert prepared_fex.read_text(encoding="utf-8") == "fex-patched\n"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    assert [(item["dependency"], item["path"]) for item in state] == [
        ("wine", "patches/wine/0001-first.patch"),
        ("wine", "patches/wine/0002-second.patch"),
        ("fex", "patches/fex/0001-fex.patch"),
    ]
    for item in state:
        assert set(item) == {"dependency", "path", "sha256"}
        assert re.fullmatch(r"[0-9a-f]{64}", item["sha256"])
        patch = patch_root / item["path"]
        assert item["sha256"] == hashlib.sha256(patch.read_bytes()).hexdigest()

    run_apply(environment)
    assert prepared_wine.read_text(encoding="utf-8") == "second\n"

    prepared_wine.write_text("tampered prepared source\n", encoding="utf-8")
    run_apply(environment)
    assert prepared_wine.read_text(encoding="utf-8") == "second\n"

    # A changed patch state must discard the old prepared copy and replay from fetched input.
    (prepared_wine.parent / "stale-marker").write_text("stale\n", encoding="utf-8")
    write_patch(wine_second, "wine", "first", "replacement")
    run_apply(environment)
    assert prepared_wine.read_text(encoding="utf-8") == "replacement\n"
    assert not (prepared_wine.parent / "stale-marker").exists()

    wine_patch_dir = patch_root / "patches" / "wine"
    saved_wine_patch_dir = patch_root / "patches" / "wine-saved"
    external_patch_dir = fixture / "external-wine-patches"
    external_patch_dir.mkdir()
    write_patch(
        external_patch_dir / "0001-external.patch",
        "wine",
        "base",
        "external",
    )
    wine_patch_dir.rename(saved_wine_patch_dir)
    wine_patch_dir.symlink_to(external_patch_dir, target_is_directory=True)
    expect_failure(environment, "patch directory must be real")
    wine_patch_dir.unlink()
    saved_wine_patch_dir.rename(wine_patch_dir)

    original_archive = wine_archive.read_bytes()
    wine_archive.write_bytes(b"tampered archive\n")
    expect_failure(environment, "fetched archive hash mismatch for wine")
    wine_archive.write_bytes(original_archive)

    valid_manifest = manifest_path.read_text(encoding="utf-8")
    manifest_path.write_text(valid_manifest + valid_manifest.splitlines()[2] + "\n", encoding="utf-8")
    expect_failure(environment, "duplicate dependency: wine")

    missing_commit = "3" * 40
    write_manifest(
        manifest_path,
        missing_commit,
        fex_commit,
        wine_archive_sha,
        fex_archive_sha,
    )
    expect_failure(environment, "does not match dependency lock")

print("Patch replay, source selection, canonical state, and negative cases passed")
