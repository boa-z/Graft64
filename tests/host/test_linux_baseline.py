#!/usr/bin/env python3
"""Offline orchestration tests; these are not Linux ARM64 runtime evidence."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BaselineTests(unittest.TestCase):
    def run_baseline(self, mode):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            runtime = base / "runtime"
            binaries = base / "bin"
            binaries.mkdir()
            (runtime / "root/bin").mkdir(parents=True)
            modules = runtime / "root/lib/wine/aarch64-windows"
            modules.mkdir(parents=True)
            (modules / "libarm64ecfex.dll").write_bytes(b"fixture")
            (runtime / "prefix/drive_c/windows/system32").mkdir(parents=True)
            report = runtime / "g1-baseline-tests.json"
            report.write_text('[{"status":"PASS","stale":true}]')

            def executable(path, body):
                path.write_text("#!/bin/bash\nset -eu\n" + body)
                path.chmod(0o755)

            executable(binaries / "uname", 'if [[ "$1" == -s ]]; then echo Linux; else echo aarch64; fi\n')
            executable(binaries / "timeout", 'shift 2\nexec "$@"\n')
            executable(binaries / "python3",
                       'if [[ "$1" == */generate-runtime-manifest.py ]]; then exit 0; fi\n'
                       f'exec {shlex.quote(sys.executable)} "$@"\n')
            for compiler in ("aarch64-w64-mingw32-clang", "x86_64-w64-mingw32-clang"):
                executable(binaries / compiler, 'for last; do :; done\n: > "$last"\n')
            executable(runtime / "root/bin/wine", r'''
if [[ "$1" == reg ]]; then
  [[ "$2" == add && "$3" == 'HKLM\Software\Microsoft\Wow64\amd64' ]]
  [[ "$4" == /ve && "$5" == /t && "$6" == REG_SZ ]]
  [[ "$7" == /d && "$8" == libarm64ecfex.dll && "$9" == /f ]]
  [[ "$TEST_MODE" != registry_failure ]]
  exit
fi
[[ "$TEST_MODE" != exit_failure ]] || exit 7
if [[ "$TEST_MODE" == bad_marker ]]; then echo NOT_GRAFT64_HELLO_ARM64; exit; fi
case "$1" in
  *hello-arm64.exe) printf 'GRAFT64_HELLO_ARM64\r\n' ;;
  *hello-amd64.exe) printf 'GRAFT64_HELLO_AMD64\r\n' ;;
  *) exit 8 ;;
esac
''')
            result = subprocess.run(
                ["bash", str(ROOT / "scripts/run-linux-baseline.sh")],
                env={**os.environ, "PATH": f"{binaries}:{os.environ['PATH']}",
                     "GRAFT_RUNTIME_OUT": str(runtime), "TEST_MODE": mode},
                capture_output=True, text=True,
            )
            return result, json.loads(report.read_text()) if report.exists() else None

    def test_crlf_and_explicit_fex_selection(self):
        result, report = self.run_baseline("success")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([entry["name"] for entry in report], ["hello-arm64", "hello-amd64"])

    def test_failures_remove_stale_success(self):
        for mode in ("bad_marker", "exit_failure", "registry_failure"):
            with self.subTest(mode=mode):
                result, report = self.run_baseline(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(report)


if __name__ == "__main__":
    unittest.main()
