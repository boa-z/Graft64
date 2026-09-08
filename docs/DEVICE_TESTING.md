# Device testing

1. For LiveContainer, download `GraftHost-unsigned-iphoneos` from a successful `graft64-ci` run for the intended commit. Extract the artifact ZIP and import `GraftHost.ipa` (not the ZIP). The IPA is unsigned; LiveContainer handles its installation/signing workflow. Alternatively, build and sign with Xcode for an arm64 iOS 17.4+ device.
2. Import the IPA into LiveContainer; enable JIT with the host tooling (StikDebug is external and not bundled). The Environment section reports lightweight JIT availability by testing `MAP_JIT` first and then an anonymous RW-to-RX transition using public memory APIs; the executable probes provide stronger evidence.
3. Run `runtime_paths`, `page_model`, `jit_basic`, `jit_write_protect`, `jit_multithread`, `unix_socket`, and `shared_mapping`.
4. Run `signal_resume`, `dlopen_bundle`, `helper_spawn`, and `helper_ipc`. To validate lifecycle JIT, send GraftHost to the background and return to the foreground; the app automatically reruns `lifecycle_jit` and replaces its earlier manual `SKIP` result.
5. Export JSON and attach it to the device test record. Never include UDID, pairing records, signing private data, or user file contents.

The Settings screen contains the automatic lifecycle probe toggle and displays the embedded app version, build number, and source commit. The commit is supplied at build time by `scripts/build-probes.sh`; a direct Xcode build without that override reports `unknown`.

Record the iPhone model, iOS, LiveContainer and StikDebug versions with the exported JSON. First export a run without JIT enabled, then enable JIT and export a separate run of all probes. Keep automatic lifecycle probing enabled, background and reopen the app, and export the updated report. Record crashes or missing reports rather than treating them as PASS. Finally close GraftHost/its container and confirm it is no longer running using the available device tooling; if process cleanup cannot be verified, explicitly record it as unverified.

## Linux preparation (not device evidence)

The manually dispatched `Linux ARM64 runtime baseline` workflow uses a native `ubuntu-24.04-arm` runner and the locked builder toolchain. It builds upstream sources only under `out/`, then tests Windows ARM64 and translated AMD64 hello samples. Failure diagnostics are uploaded separately; a runtime archive is published only after both tests pass. This is preparatory Linux evidence, not G0 clearance or proof that Wine/FEX runs on iOS. GraftHost remains a host probe only.

## LiveContainer mapping diagnostics

If LiveContainer logs a path ending in `/(null)` while importing the app, it did not resolve the bundle executable. Use a freshly built IPA and verify that `Payload/GraftHost.app/Info.plist` contains:

```xml
<key>CFBundleExecutable</key>
<string>GraftHost</string>
```

The repository's `scripts/verify-package.sh` checks this field before an IPA is uploaded. Remove the stale LiveContainer import, re-import the new `GraftHost-unsigned-iphoneos` artifact, and confirm that the app's executable is named `GraftHost` (not `GraftHost.app` or an empty value). This mapping error occurs before GraftHost starts, so no probe result is generated until the bundle is resolved.
