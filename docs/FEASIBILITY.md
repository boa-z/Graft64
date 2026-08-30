# G0 feasibility

Status: **Yellow / UNVERIFIED**.

The host probe compiles on the available Apple toolchain and exercises non-device-safe primitives (page model, Unix sockets, and file mappings) on macOS. JIT execution, `ucontext_t` signal recovery, signed bundled dylib loading, helper spawn/IPC, and background/foreground JIT persistence require a real arm64 iOS device running under LiveContainer with JIT enabled. Until an exported report is attached, these are intentionally reported as `SKIP`/manual rather than `PASS`.

## Stop conditions

Do not start Wine/FEX work if `jit_basic`, controlled signal recovery, or helper IPC is red on a JIT-enabled device. Record the API, errno, and next experiment in the exported report and update this document.
