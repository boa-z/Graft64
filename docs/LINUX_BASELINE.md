# Native Linux ARM64 baseline evidence

Date: 2026-09-08. Tested source: `45f1bf0296fae058582fe94b411502f0668e7f41`.

[GitHub Actions run 34240690510](https://github.com/boa-z/Graft64/actions/runs/34240690510) completed successfully on the native `ubuntu-24.04-arm` runner, with an explicit `uname -m = aarch64` check and the ARM64 builder container.

Verified scope:

- Pinned Wine 11.0, FEX 2607 (including its pinned MinGW submodules), and LLVM-MinGW 20260616 / LLVM 22.1.8.
- Wine, FEX ARM64EC and WoW64 DLLs, and Linux UnixLib companions built and installed.
- Runtime manifests verified source/toolchain metadata, patch hashes, file/symlink hashes, the AArch64 ELF loader, and PE/CHPE architecture structures.
- Windows ARM64 `hello-arm64` exited 0 and emitted `GRAFT64_HELLO_ARM64`.
- Windows AMD64 `hello-amd64`, with Wine explicitly configured to use `libarm64ecfex.dll`, exited 0 and emitted `GRAFT64_HELLO_AMD64`.
- The runtime tar archive retained symlinks, manifests, patch records, build fingerprint, sample hashes, and test logs; CI extracted and reverified it before publishing.

Artifacts for this tested commit:

- [Runtime archive and SHA-256](https://github.com/boa-z/Graft64/actions/runs/34240690510/artifacts/10062488357).
- [Build and sample diagnostics](https://github.com/boa-z/Graft64/actions/runs/34240690510/artifacts/10062489435).
- [Unsigned GraftHost iPhoneOS IPA](https://github.com/boa-z/Graft64/actions/runs/34240688777/artifacts/10061853640), from the separately successful host/IPA CI. Its embedded source commit is `45f1bf0296fa`; package structure and checksum were also reverified after download.

This is a two-console-sample Linux smoke baseline, not full G1 acceptance, general Windows application compatibility, 32-bit x86 execution, or evidence of iOS Wine/FEX operation. Building the WoW64 module is not a runtime test of that module. The archive's extraction check proves integrity, not execution from arbitrary relocated installation paths.

The ARM64 sample log retains Wine prefix-initialization warnings about missing `syswow64` and `sysarm32` `rundll32.exe` (`c0000135`). Both tested 64-bit samples still exited 0 with their exact markers. These warnings are not evidence that 32-bit initialization or execution works.

G0 remains **UNVERIFIED** until the user exports real iPhone reports under LiveContainer/StikDebug. Follow [device testing](DEVICE_TESTING.md), including separate JIT-disabled/enabled runs, lifecycle re-entry, and process cleanup verification. No device result is inferred from this Linux run, macOS tests, simulator builds, or the unsigned IPA.
