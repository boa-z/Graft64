# ADR 0004: Page model

The probe records the host page size and tests aligned versus deliberately unaligned `mprotect` calls. Graft64 does not emulate 4 KiB permissions by rewriting every memory access; a red result blocks runtime porting until an alternative mapping design is proven.
