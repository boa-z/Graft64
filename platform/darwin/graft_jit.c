#include "graft/graft_jit.h"
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif

typedef struct graft_jit_copy_context { graft_jit_region *region; size_t offset; const void *data; size_t size; } graft_jit_copy_context;
#if defined(__APPLE__) && defined(TARGET_OS_IPHONE)
static int graft_jit_copy_callback(void *opaque) {
    graft_jit_copy_context *copy = opaque;
    if (!copy || !copy->region || !copy->region->base || !copy->data || copy->offset > copy->region->size || copy->size > copy->region->size - copy->offset) return EINVAL;
    memcpy((char *)copy->region->base + copy->offset, copy->data, copy->size);
    return 0;
}
PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP(graft_jit_copy_callback);
#endif

#endif

static size_t page_round(size_t size) {
    const size_t page = (size_t)getpagesize();
    return (size + page - 1u) & ~(page - 1u);
}

graft_jit_status graft_jit_check(void) {
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
    /* MAP_JIT uses per-thread W^X on iOS.  A successful mmap alone is not
     * enough: without this facility callers cannot safely publish code. */
    if (!pthread_jit_write_protect_supported_np()) {
        errno = ENOTSUP;
        return GRAFT_JIT_STATUS_UNAVAILABLE;
    }
#endif
    graft_jit_region region = {0};
    if (graft_jit_alloc(4096, &region) == 0) {
        int commit_result = graft_jit_commit(&region);
        int commit_error = errno;
        graft_jit_free(&region);
        if (commit_result == 0) return GRAFT_JIT_STATUS_ENABLED;
        errno = commit_error;
    }
    switch (errno) {
        case EACCES:
        case EPERM:
            return GRAFT_JIT_STATUS_DISABLED;
        case ENOTSUP:
        case ENOSYS:
            return GRAFT_JIT_STATUS_UNAVAILABLE;
        default:
            return GRAFT_JIT_STATUS_UNKNOWN;
    }
#else
    return GRAFT_JIT_STATUS_UNAVAILABLE;
#endif
}

const char *graft_jit_status_name(graft_jit_status status) {
    switch (status) {
        case GRAFT_JIT_STATUS_ENABLED: return "enabled";
        case GRAFT_JIT_STATUS_DISABLED: return "disabled";
        case GRAFT_JIT_STATUS_UNAVAILABLE: return "unavailable";
        case GRAFT_JIT_STATUS_UNKNOWN:
        default: return "unknown";
    }
}

int graft_jit_alloc(size_t size, graft_jit_region *out_region) {
    if (!out_region || size == 0) { errno = EINVAL; return -1; }
    size = page_round(size);
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__)
    flags |= MAP_JIT;
#endif
    /* iOS requires executable permission on the initial MAP_JIT mapping;
     * writes are still constrained by begin_write/commit W^X transitions. */
    int protection = PROT_READ | PROT_WRITE;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    protection |= PROT_EXEC;
#endif
    void *base = mmap(NULL, size, protection, flags, -1, 0);
    int backend = 1; /* MAP_JIT */
#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* LiveContainer may re-sign a guest without the allow-jit entitlement.
     * Fall back to a private anonymous RW mapping and let commit() perform the
     * public RW-to-RX capability check. An ordinary sandboxed process can
     * allocate RW memory but will be rejected at the RX transition, so no
     * private process-state API is required. */
    if (base == MAP_FAILED) {
        flags = MAP_PRIVATE | MAP_ANON;
        base = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
        backend = 2; /* debugged anonymous RW -> RX */
    }
#endif
    if (base == MAP_FAILED) { return -1; }
    out_region->base = base;
    out_region->size = size;
    out_region->backend = backend;
    return 0;
}

int graft_jit_begin_write(graft_jit_region *region) {
    if (!region || !region->base) { errno = EINVAL; return -1; }
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(0);
#endif
    return mprotect(region->base, region->size, PROT_READ | PROT_WRITE);
}

int graft_jit_write(graft_jit_region *region, size_t offset, const void *data, size_t size) {
    if (!region || !region->base || !data || offset > region->size || size > region->size - offset) { errno = EINVAL; return -1; }
    graft_jit_copy_context copy = {region, offset, data, size};
#if defined(__APPLE__) && TARGET_OS_IPHONE
    if (region->backend == 1) return pthread_jit_write_with_callback_np(graft_jit_copy_callback, &copy);
#else
    (void)copy;
#endif
    if (graft_jit_begin_write(region) != 0) return -1;
    memcpy((char *)region->base + offset, data, size);
    return 0;
}

int graft_jit_commit(graft_jit_region *region) {
    if (!region || !region->base) { errno = EINVAL; return -1; }
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(1);
#endif
    return mprotect(region->base, region->size, PROT_READ | PROT_EXEC);
}

int graft_jit_end_write(graft_jit_region *region) {
    return graft_jit_commit(region);
}

unsigned int graft_jit_capabilities(void) {
#if defined(__APPLE__)
    return GRAFT_JIT_CAP_ALLOCATE | GRAFT_JIT_CAP_WRITE |
           GRAFT_JIT_CAP_EXECUTE | GRAFT_JIT_CAP_ICACHE_INVALIDATE;
#else
    return GRAFT_JIT_CAP_ALLOCATE | GRAFT_JIT_CAP_WRITE |
           GRAFT_JIT_CAP_EXECUTE | GRAFT_JIT_CAP_ICACHE_INVALIDATE;
#endif
}

int graft_jit_invalidate(graft_jit_region *region, size_t offset, size_t size) {
    if (!region || !region->base || offset > region->size || size > region->size - offset) {
        errno = EINVAL; return -1;
    }
#if defined(__APPLE__)
    sys_icache_invalidate((char *)region->base + offset, size);
#else
    __builtin___clear_cache((char *)region->base + offset,
                            (char *)region->base + offset + size);
#endif
    return 0;
}

int graft_jit_invalidate_icache(graft_jit_region *region, size_t offset, size_t size) {
    return graft_jit_invalidate(region, offset, size);
}

void graft_jit_free(graft_jit_region *region) {
    if (region && region->base) {
        munmap(region->base, region->size);
        region->base = NULL; region->size = 0; region->backend = 0;
    }
}
