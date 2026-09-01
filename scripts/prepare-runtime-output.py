#!/usr/bin/env python3
"""Mark and validate a dedicated generated runtime output directory."""

from __future__ import annotations

import argparse
from pathlib import Path


MARKER_NAME = ".graft64-runtime-output"
MARKER_TEXT = "Graft64 generated runtime output v1\n"


def prepare_output(output: Path) -> None:
    if output.is_symlink():
        raise ValueError(f"runtime output must not be a symlink: {output}")
    output = output.resolve()
    marker = output / MARKER_NAME
    if output == Path.home().resolve():
        raise ValueError(f"refusing to use the home directory as runtime output: {output}")
    if output.exists() and (output.is_symlink() or not output.is_dir()):
        raise ValueError(f"runtime output must be a real directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    if marker.exists():
        if marker.is_symlink() or not marker.is_file():
            raise ValueError(f"runtime output marker must be a regular file: {marker}")
        if marker.read_text(encoding="utf-8") != MARKER_TEXT:
            raise ValueError(f"runtime output marker is invalid: {marker}")
        return
    unexpected = sorted(path.name for path in output.iterdir())
    if unexpected:
        raise ValueError(
            f"refusing unmarked non-empty runtime output directory {output}: {unexpected}"
        )
    marker.write_text(MARKER_TEXT, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        prepare_output(args.output)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
