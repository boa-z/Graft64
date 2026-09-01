# Building

## G0 host probe and IPA

On an Apple Silicon Mac with Xcode, run:

```sh
./scripts/run-host-tests.sh
GRAFT_SDK=iphoneos GRAFT_CONFIGURATION=Release \
  GRAFT_CODE_SIGNING_ALLOWED=NO ./scripts/build-probes.sh
./scripts/verify-package.sh
```

The resulting `out/GraftHost.ipa` is an unsigned arm64 device package suitable for import into a JIT-enabled LiveContainer. `package-ipa.sh` writes an integrity sidecar at `out/GraftHost.ipa.sha256`; `verify-package.sh` checks a private snapshot of those exact bytes before inspecting the archive and writes a relative-path `package-manifest.sha256`. This checksum is an integrity check for one build, not a claim that separate IPA builds are byte-for-byte reproducible. The package must not be described as device-validated until GraftHost exports a report from a real device.

## G1 Linux ARM64 baseline

G1 runs on a native Linux `aarch64` machine or an ARM64 container engine. The dependency lock records exact Wine, FEX, and LLVM-MinGW sources and archive hashes:

```sh
./scripts/fetch-upstream.sh
./scripts/build-runtime-linux-arm64.sh
./scripts/run-linux-baseline.sh
./scripts/verify-artifacts.sh
```

On Apple Silicon with an ARM64 Docker engine, build the pinned builder image and run through Docker:

```sh
GRAFT_RUNTIME_USE_CONTAINER=1 ./scripts/build-runtime-linux-arm64.sh
```

The script refuses to run a formal build on x86-64 or macOS directly. It verifies the exact Wine/FEX archives selected by `fetched-manifest.tsv`, re-extracts them into `out/runtime-linux-arm64/build/sources`, replays the sorted patch series there, and records the result in `applied-patches.json`. It never trusts or patches a previously extracted upstream tree. A marker prevents cleanup in an unmarked non-empty output directory, while a fingerprint covering the lock, patches, builder definition, and runtime build scripts invalidates only generated build/root/prefix directories when an input changes.

Configure/build logs remain under `out/runtime-linux-arm64/logs`, installed files go under `out/runtime-linux-arm64/root`, and the build writes both `runtime-manifest.sha256` and `runtime-manifest.json` with relative paths. `verify-artifacts.sh` checks the complete dependency/toolchain lock and applied patch state, recomputes file and symlink hashes, validates the AArch64 ELF Wine loader and ARM64EC/ARM64 PE modules, and binds baseline log hashes when tests exist. Passing this integrity contract does not claim that the G1 execution baseline passed; `run-linux-baseline.sh` must actually run on Linux ARM64 and retain its logs/test entries. G1 is a baseline only; no iOS runtime artifact is produced by this path.
