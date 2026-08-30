# Upstream gaps

## Wine ARM64 page-size requirement

The G1 lock now pins the Wine 11.0 stable tag to commit
`db11d0fe6a169c457e23d007e20404643d067aa8` (released 2026-01-13). Wine
11.0's ARM64 release notes add a 4K-page emulation layer for larger host
pages, typically 16K or 64K, but state that it is reliable only for simple
applications and still strongly recommend a 4K-page kernel. iOS devices
commonly expose 16K host pages, so a green G0 `page_model` result does not by
itself prove that the Wine ARM64/ARM64EC target is viable. Before G2, run the
ARM64 Linux baseline and a device-side allocation experiment against this
Wine commit; do not emulate 4K semantics by adding checks to every FEX memory
access. Track the result here with the exact runtime manifest and report.

Evidence: `ANNOUNCE.md` in the Wine 11.0 source (lines 78-82) describes the
4K-on-larger-host-page support and its limitations. The commit-addressed
GitHub archive used by `deps.lock` has SHA-256
`18aaee150ad540885b9706ae73ccf6febca904049de2792199a9dc18a2772e6a`.

## FEX UnixLib versus iOS

The lock currently builds FEX 2607. A source-only comparison against the next
upstream release, FEX 2608 (commit
`e869aa644a16e4332cdc15c1ea0b4d13d482385d`, archive SHA-256
`1d19c5342d1437e4c9cc0b722005f63361d583b842d10988677c770a68e6c223`), shows
that `Source/Windows/UnixLib/FEXUnixLib.cpp` and its header are byte-identical
between FEX 2607 and 2608. The current UnixLib implementation is therefore
still Linux-specific and cannot be linked into the iOS host unchanged:

* `FEXUnixLib.cpp` includes Linux `<sys/prctl.h>` and uses
  `PR_GET/SET_MEM_MODEL`, `PR_ARM64_SET_UNALIGN_ATOMIC`, and
  `PR_SET_VMA`; these controls have no iOS equivalent. Hardware TSO,
  unaligned-atomic control, and VMA naming must return a documented
  not-supported result or become no-ops in an iOS adapter.
* Its fallback in `Source/Windows/Common/FEXUnixLib.cpp` emits ARM64 `svc`
  instructions with Linux syscall numbers (`prctl` 167, `madvise` 233,
  `getpid` 172). Darwin/iOS syscall ABI and numbers differ; this path must
  never be compiled for iOS.
* Stats allocation relies on Linux `/dev/shm` semantics (`shm_open`,
  `shm_unlink`, `MAP_NORESERVE`, `MAP_FIXED`) and reserves a growable mapping.
  The iOS sandbox and 16K pages require a file-backed, bounded shared mapping
  adapter with explicit lifetime and alignment checks; do not assume
  `/dev/shm` exists.
* The FEX CMake project explicitly supports only Linux and Windows, and the
  UnixLib is loaded through Wine's UnixLib dispatcher. Graft64 must first
  expose the corresponding Wine dispatcher entry points on iOS, then provide
  a thin Darwin implementation for the six enum slots; this is an integration
  task, not a decoder/JIT-core patch.

Conclusion: FEX 2608 is a useful compatibility check but does not remove the
iOS UnixLib gap. Keep FEX pinned at 2607 until an iOS adapter is implemented
and covered by an ARM64 Linux baseline plus device evidence; upgrading the
lock to 2608 independently would not make the iOS path buildable.

## FEX ARM64EC integration

The G1 lock pins the upstream FEX release and builds its ARM64EC and AArch64 MinGW modules without local patches. Any missing Wine external-emulator entry point, loader ABI mismatch, or toolchain incompatibility must be recorded here with an upstream issue and a minimal patch proposal before changing `patches/`.

G1 may be started only after a real arm64 LiveContainer report demonstrates that the G0 stop conditions are green. No private fork is selected silently.
