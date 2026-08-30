# Architecture

`GraftHost` is a SwiftUI shell. Swift calls only the stable C ABI in `platform/include/graft`. The C layer owns JIT allocation/protection details, probe execution, and the versioned Unix IPC header. A future Wine/FEX runtime will consume the same platform layer without putting LiveContainer-specific paths in core code.

The JIT implementation exposes backend-neutral `alloc`, `begin_write`, `commit`, `invalidate`, and `capabilities` operations. The current Darwin backend uses public `mmap(MAP_JIT)`, `pthread_jit_write_protect_np` where available, `mprotect`, and Apple's `sys_icache_invalidate` (with the compiler builtin fallback on non-Apple hosts). On LiveContainer builds where the re-signed app lacks the `allow-jit` entitlement, allocation falls back to a private anonymous RW mapping and the capability check performs an actual public RW-to-RX transition. Probe details identify that diagnostic backend separately from `MAP_JIT`; it is not proof of a general JIT entitlement and may still be rejected by newer TXM enforcement. No private process-state API is used. The approach was cross-checked against the public memory transitions used by Amethyst-iOS; no Amethyst source is vendored. A future FEX backend can replace these operations without changing probe callers.

Lifecycle testing allocates and commits a small code cache before suspension; after foreground resume it executes the same mapping and reports `cache_reused=true`. It does not silently create a new mapping during the resume phase.

Runtime paths are supplied by the host through an explicit `graft_path_context` (`guest_bundle_root`, `runtime_root`, `data_root`, and `cache_root`). The C probe layer no longer infers them from the executable path, current process name, or LiveContainer string matching.

Helper probes inherit both a Unix socket and a file-backed `MAP_SHARED` mapping. Multiple requests are exchanged while the helper remains alive; the helper heartbeat and shared marker are validated before an explicit shutdown. Probe callbacks expose structured `reason_code`, `graft_error`, and `os_error` fields so callers do not need to infer failures from a single mixed error value.
