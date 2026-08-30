#include "graft/graft_jit.h"
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
/* csops is a public Darwin syscall used here only to observe the
 * CS_DEBUGGED bit set by an external JIT host such as StikDebug. */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define GRAFT_CS_DEBUGGED 0x10000000u

static int graft_process_is_debugged(void) {
    int flags = 0;
    return csops(getpid(), 0u, &flags, sizeof(flags)) == 0 &&
           ((unsigned int)flags & GRAFT_CS_DEBUGGED) != 0u;
}
#endif
#endif

static size_t page_round(size_t size) {
    const size_t page = (size_t)getpagesize();
    return (size + page - 1u) & ~(page - 1u);
}

graft_jit_status graft_jit_check(void) {
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
    if (graft_process_is_debugged()) return GRAFT_JIT_STATUS_ENABLED;
#endif
    graft_jit_region region = {0};
    if (graft_jit_alloc(4096, &region) == 0) {
        graft_jit_free(&region);
        return GRAFT_JIT_STATUS_ENABLED;
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
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) { return -1; }
    out_region->base = base;
    out_region->size = size;
    out_region->backend = 1;
    return 0;
}

int graft_jit_begin_write(graft_jit_region *region) {
    if (!region || !region->base) { errno = EINVAL; return -1; }
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(0);
#endif
    return mprotect(region->base, region->size, PROT_READ | PROT_WRITE);
}

int graft_jit_end_write(graft_jit_region *region) {
    if (!region || !region->base) { errno = EINVAL; return -1; }
    int result = mprotect(region->base, region->size, PROT_READ | PROT_EXEC);
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(1);
#endif
    return result;
}

int graft_jit_invalidate_icache(graft_jit_region *region, size_t offset, size_t size) {
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

void graft_jit_free(graft_jit_region *region) {
    if (region && region->base) {
        munmap(region->base, region->size);
        region->base = NULL; region->size = 0; region->backend = 0;
    }
}
