import hashlib
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).parents[2]
SOURCE_OUT = Path(os.environ.get("GRAFT_PACKAGE_OUT", ROOT / "out"))
PACKAGER = ROOT / "scripts" / "package-ipa.sh"
VERIFIER = ROOT / "scripts" / "verify-package.sh"
SOURCE_PAYLOAD = SOURCE_OUT / "Payload"


def verify(output, expected_success, needle=None):
    environment = os.environ.copy()
    environment["GRAFT_PACKAGE_OUT"] = str(output)
    completed = subprocess.run(
        [VERIFIER],
        cwd=ROOT,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
    )
    assert (completed.returncode == 0) is expected_success, completed.stdout + completed.stderr
    if needle:
        assert needle in completed.stdout + completed.stderr


assert SOURCE_PAYLOAD.is_dir(), f"missing package fixture: {SOURCE_PAYLOAD}"

with tempfile.TemporaryDirectory(prefix="graft64-package-checksum-") as temporary:
    output = Path(temporary)
    shutil.copytree(SOURCE_PAYLOAD, output / "Payload", symlinks=True)
    ipa = output / "GraftHost.ipa"
    checksum = output / "GraftHost.ipa.sha256"
    checksum_target = output / "checksum-target"
    checksum_target.write_text("must not be overwritten\n", encoding="utf-8")
    checksum.symlink_to(checksum_target)

    package_environment = os.environ.copy()
    package_environment["GRAFT_PACKAGE_OUT"] = str(output)
    subprocess.run(
        [PACKAGER],
        cwd=ROOT,
        env=package_environment,
        check=True,
        capture_output=True,
        text=True,
    )
    assert checksum_target.read_text(encoding="utf-8") == "must not be overwritten\n"
    assert checksum.is_file() and not checksum.is_symlink()

    original = checksum.read_text(encoding="utf-8")
    package_manifest_path = output / "package-manifest.sha256"
    entitlements_path = output / "entitlements.plist"
    package_manifest_path.symlink_to(ipa.name)
    entitlements_path.symlink_to(ipa.name)
    ipa_digest = hashlib.sha256(ipa.read_bytes()).hexdigest()
    verify(output, True)
    assert hashlib.sha256(ipa.read_bytes()).hexdigest() == ipa_digest
    assert package_manifest_path.is_file() and not package_manifest_path.is_symlink()
    assert entitlements_path.is_file() and not entitlements_path.is_symlink()
    package_manifest_lines = package_manifest_path.read_text(encoding="utf-8").splitlines()
    expected_package_paths = [
        "Payload/GraftHost.app/GraftHost",
        "Payload/GraftHost.app/GraftProbeHelper",
        "Payload/GraftHost.app/GraftProbeTest.dylib",
        "Payload/GraftHost.app/Info.plist",
    ]
    assert len(package_manifest_lines) == len(expected_package_paths)
    for line, expected_path in zip(package_manifest_lines, expected_package_paths):
        digest, relative_path = line.split()
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert relative_path == expected_path

    checksum.write_text("0" * 64 + original[64:], encoding="utf-8")
    verify(output, False, "IPA checksum mismatch")

    checksum.write_text(original.replace("GraftHost.ipa", "Other.ipa"), encoding="utf-8")
    verify(output, False, "checksum path must be GraftHost.ipa")

    checksum.write_text(original + original, encoding="utf-8")
    verify(output, False, "exactly one digest/path line")

    checksum.unlink()
    verify(output, False, "missing IPA checksum")

print(
    "IPA checksum generation, relative package manifest, and rejection cases passed"
)
