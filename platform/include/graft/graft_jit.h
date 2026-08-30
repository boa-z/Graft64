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
int graft_jit_end_write(graft_jit_region *region);
int graft_jit_invalidate_icache(graft_jit_region *region,
                                size_t offset,
                                size_t size);
void graft_jit_free(graft_jit_region *region);

#ifdef __cplusplus
}
#endif

#endif
