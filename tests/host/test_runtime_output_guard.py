import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).parents[2]
GUARD = ROOT / "scripts" / "prepare-runtime-output.py"


def run(output, expected_success, needle=None):
    completed = subprocess.run(
        ["python3", str(GUARD), str(output)],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert (completed.returncode == 0) is expected_success, (
        completed.stdout + completed.stderr
    )
    if needle:
        assert needle in completed.stdout + completed.stderr


with tempfile.TemporaryDirectory(prefix="graft64-runtime-output-") as temporary:
    fixture = Path(temporary)
    output = fixture / "runtime"
    marker = output / ".graft64-runtime-output"

    run(output, True)
    assert marker.read_text(encoding="utf-8") == "Graft64 generated runtime output v1\n"
    (output / "build").mkdir()
    run(output, True)

    marker.write_text("wrong marker\n", encoding="utf-8")
    run(output, False, "marker is invalid")

    marker.unlink()
    run(output, False, "unmarked non-empty")

    unmarked = fixture / "unmarked"
    unmarked.mkdir()
    (unmarked / "user-file").write_text("preserve\n", encoding="utf-8")
    run(unmarked, False, "unmarked non-empty")
    assert (unmarked / "user-file").read_text(encoding="utf-8") == "preserve\n"

    symlink_marker_output = fixture / "symlink-marker"
    symlink_marker_output.mkdir()
    marker_target = fixture / "marker-target"
    marker_target.write_text("Graft64 generated runtime output v1\n", encoding="utf-8")
    (symlink_marker_output / ".graft64-runtime-output").symlink_to(marker_target)
    run(symlink_marker_output, False, "marker must be a regular file")

    real_output = fixture / "real-output"
    real_output.mkdir()
    output_symlink = fixture / "output-symlink"
    output_symlink.symlink_to(real_output, target_is_directory=True)
    run(output_symlink, False, "must not be a symlink")

print("Runtime output marker and non-empty directory guard cases passed")
