# Building

## G0 host probe and IPA

On an Apple Silicon Mac with Xcode, run:

```sh
./scripts/run-host-tests.sh
GRAFT_SDK=iphoneos GRAFT_CONFIGURATION=Release \
  GRAFT_CODE_SIGNING_ALLOWED=NO ./scripts/build-probes.sh
./scripts/verify-package.sh
```

The resulting `out/GraftHost.ipa` is an unsigned arm64 device package suitable for import into a JIT-enabled LiveContainer. The package must not be described as device-validated until GraftHost exports a report from a real device.

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

The script refuses to run a formal build on x86-64 or macOS directly. It records configure/build logs under `out/runtime-linux-arm64/logs`, installs into `out/runtime-linux-arm64/root`, and writes both `runtime-manifest.sha256` and the schema-checked `runtime-manifest.json`. G1 is a baseline only; no iOS runtime artifact is produced by this path.
