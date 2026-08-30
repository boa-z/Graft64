# Device testing

1. Build and sign with Xcode for an arm64 iOS 17.4+ device.
2. Import the IPA into LiveContainer; enable JIT with the host tooling (StikDebug is external and not bundled).
3. Run `runtime_paths`, `page_model`, `jit_basic`, `jit_write_protect`, `jit_multithread`, `unix_socket`, and `shared_mapping`.
4. Run `signal_resume`, `dlopen_bundle`, `helper_spawn`, `helper_ipc`, and `lifecycle_jit`; follow any manual instructions shown by the app.
5. Export JSON and attach it to the device test record. Never include UDID, pairing records, signing private data, or user file contents.
