#ifndef GRAFT_JIT_H
#define GRAFT_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct graft_jit_region {
    void *base;
    size_t size;
    int backend;
} graft_jit_region;

/* Backend capabilities are intentionally independent of the allocation
 * mechanism, so a future FEX/Wine backend need not expose MAP_JIT. */
typedef enum graft_jit_capability {
    GRAFT_JIT_CAP_ALLOCATE = 1u << 0,
    GRAFT_JIT_CAP_WRITE = 1u << 1,
    GRAFT_JIT_CAP_EXECUTE = 1u << 2,
    GRAFT_JIT_CAP_ICACHE_INVALIDATE = 1u << 3,
} graft_jit_capability;

/* A lightweight, read-only indication of whether the current process can use
 * externally-provided JIT execution.  This is intentionally separate from
 * the executable jit_basic probe: allocation can succeed before a complete
 * write/execute transition has been exercised. */
typedef enum graft_jit_status {
    GRAFT_JIT_STATUS_UNKNOWN = 0,
    GRAFT_JIT_STATUS_ENABLED = 1,
    GRAFT_JIT_STATUS_DISABLED = 2,
    GRAFT_JIT_STATUS_UNAVAILABLE = 3,
} graft_jit_status;

graft_jit_status graft_jit_check(void);
const char *graft_jit_status_name(graft_jit_status status);

int graft_jit_alloc(size_t size, graft_jit_region *out_region);
int graft_jit_begin_write(graft_jit_region *region);
/* Make the current contents executable. This is the backend-neutral commit
 * operation; it may be a no-op for dual-mapped or translator-backed JITs. */
int graft_jit_commit(graft_jit_region *region);
int graft_jit_invalidate(graft_jit_region *region, size_t offset, size_t size);
unsigned int graft_jit_capabilities(void);
/* Source compatibility for early GRAFT-0001 clients. */
int graft_jit_end_write(graft_jit_region *region);
int graft_jit_invalidate_icache(graft_jit_region *region, size_t offset, size_t size);
void graft_jit_free(graft_jit_region *region);

#ifdef __cplusplus
}
#endif

#endif
