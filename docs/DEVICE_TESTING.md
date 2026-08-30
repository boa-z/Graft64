# Device testing

1. Build and sign with Xcode for an arm64 iOS 17.4+ device.
2. Import the IPA into LiveContainer; enable JIT with the host tooling (StikDebug is external and not bundled).
3. Run `runtime_paths`, `page_model`, `jit_basic`, `jit_write_protect`, `jit_multithread`, `unix_socket`, and `shared_mapping`.
4. Run `signal_resume`, `dlopen_bundle`, `helper_spawn`, `helper_ipc`, and `lifecycle_jit`; follow any manual instructions shown by the app.
5. Export JSON and attach it to the device test record. Never include UDID, pairing records, signing private data, or user file contents.

## LiveContainer mapping diagnostics

If LiveContainer logs a path ending in `/(null)` while importing the app, it did not resolve the bundle executable. Use a freshly built IPA and verify that `Payload/GraftHost.app/Info.plist` contains:

```xml
<key>CFBundleExecutable</key>
<string>GraftHost</string>
```

The repository's `scripts/verify-package.sh` checks this field before an IPA is uploaded. Remove the stale LiveContainer import, re-import the new `GraftHost-unsigned-iphoneos` artifact, and confirm that the app's executable is named `GraftHost` (not `GraftHost.app` or an empty value). This mapping error occurs before GraftHost starts, so no probe result is generated until the bundle is resolved.
