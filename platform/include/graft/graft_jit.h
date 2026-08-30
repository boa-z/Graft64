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
