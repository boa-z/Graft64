# Architecture

`GraftHost` is a SwiftUI shell. Swift calls only the stable C ABI in `platform/include/graft`. The C layer owns JIT allocation/protection details, probe execution, and the versioned Unix IPC header. A future Wine/FEX runtime will consume the same platform layer without putting LiveContainer-specific paths in core code.

The JIT implementation uses public `mmap(MAP_JIT)`, `pthread_jit_write_protect_np` where available, `mprotect`, and `__builtin___clear_cache`. The approach was cross-checked against the public syscall sequence used by Amethyst-iOS; no Amethyst source is vendored.
