# Upstream gaps

## Wine ARM64 page-size requirement

Wine 10.0's ARM64/ARM64EC release notes state that the ARM64 support currently requires a 4K system page size. iOS devices commonly expose 16K host pages, so a green G0 `page_model` result does not by itself prove that the Wine ARM64 target is viable. Before G2, run an ARM64 Linux baseline and a device-side allocation experiment; do not emulate 4K semantics by adding checks to every FEX memory access. If upstream gains 16K support, update this entry with the release/commit and a reproducible test.

## FEX ARM64EC integration

The G1 lock pins the upstream FEX release and builds its ARM64EC and AArch64 MinGW modules without local patches. Any missing Wine external-emulator entry point, loader ABI mismatch, or toolchain incompatibility must be recorded here with an upstream issue and a minimal patch proposal before changing `patches/`.

G1 may be started only after a real arm64 LiveContainer report demonstrates that the G0 stop conditions are green. No private fork is selected silently.
