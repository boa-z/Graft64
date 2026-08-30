#include "graft/graft_probe.h"
#include "graft/graft_jit.h"
#include "graft/graft_ipc_protocol.h"
#include "graft/graft_process.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

static char g_helper_path[PATH_MAX];
static char g_dylib_path[PATH_MAX];
static char g_guest_bundle_root[PATH_MAX];
static char g_runtime_root[PATH_MAX];
static char g_data_root[PATH_MAX];
static char g_cache_root[PATH_MAX];
static char g_bundle_url[PATH_MAX];
static char g_bundle_executable_url[PATH_MAX];
static char g_argv0[PATH_MAX];
static char g_current_working_directory[PATH_MAX];
static char g_home_directory[PATH_MAX];
static char g_documents_directory[PATH_MAX];
static char g_library_directory[PATH_MAX];
static char g_temporary_directory[PATH_MAX];
static char g_app_version[128];
static char g_livecontainer_evidence[256];
static atomic_bool g_lifecycle_background_seen;
static atomic_bool g_lifecycle_foreground_seen;
static graft_jit_region g_lifecycle_code_cache;
static atomic_bool g_lifecycle_code_cache_ready;
static atomic_int g_lifecycle_prepare_error;
int graft_configure_helper(const char *path) {
    if (!path || strlen(path) >= sizeof(g_helper_path)) { errno = EINVAL; return -1; }
    strcpy(g_helper_path, path); return 0;
}
int graft_configure_dylib(const char *path) {
    if (!path || strlen(path) >= sizeof(g_dylib_path)) { errno = EINVAL; return -1; }
    strcpy(g_dylib_path, path); return 0;
}
int graft_configure_path_context(const graft_path_context *context) {
    if (!context || !context->guest_bundle_root || !context->runtime_root ||
        !context->data_root || !context->cache_root) { errno = EINVAL; return -1; }
    const char *values[] = {context->guest_bundle_root, context->runtime_root,
                            context->data_root, context->cache_root};
    char *destinations[] = {g_guest_bundle_root, g_runtime_root, g_data_root, g_cache_root};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (!values[i][0] || strlen(values[i]) >= PATH_MAX) { errno = EINVAL; return -1; }
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        strcpy(destinations[i], values[i]);
    return 0;
}
int graft_configure_runtime_observation(const graft_runtime_observation *observation) {
    if (!observation) { errno = EINVAL; return -1; }
    const char *values[] = {
        observation->bundle_url, observation->bundle_executable_url,
        observation->argv0, observation->current_working_directory,
        observation->home_directory, observation->documents_directory,
        observation->library_directory, observation->temporary_directory,
        observation->app_version, observation->livecontainer_evidence,
    };
    char *destinations[] = {
        g_bundle_url, g_bundle_executable_url, g_argv0,
        g_current_working_directory, g_home_directory, g_documents_directory,
        g_library_directory, g_temporary_directory, g_app_version,
        g_livecontainer_evidence,
    };
    const size_t capacities[] = {
        sizeof(g_bundle_url), sizeof(g_bundle_executable_url), sizeof(g_argv0),
        sizeof(g_current_working_directory), sizeof(g_home_directory),
        sizeof(g_documents_directory), sizeof(g_library_directory),
        sizeof(g_temporary_directory), sizeof(g_app_version),
        sizeof(g_livecontainer_evidence),
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (!values[i] || !values[i][0] || strlen(values[i]) >= capacities[i]) {
            errno = EINVAL;
            return -1;
        }
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        strcpy(destinations[i], values[i]);
    return 0;
}
int graft_lifecycle_note_background(void) {
    /* Prepare while the app is still running. The foreground phase must only
     * execute this existing cache, proving suspend/resume preservation. */
    if (!atomic_load_explicit(&g_lifecycle_code_cache_ready, memory_order_acquire)) {
        uint32_t code[] = { 0x52800540u, 0xD65F03C0u }; /* mov w0,#42; ret */
        if (graft_jit_alloc(4096, &g_lifecycle_code_cache) != 0) {
            int e = errno;
            atomic_store_explicit(&g_lifecycle_prepare_error, e, memory_order_release);
            atomic_store_explicit(&g_lifecycle_background_seen, true, memory_order_release);
            errno = e;
            return -1;
        }
        if (graft_jit_begin_write(&g_lifecycle_code_cache) != 0) {
            int e = errno;
            graft_jit_free(&g_lifecycle_code_cache);
            atomic_store_explicit(&g_lifecycle_prepare_error, e, memory_order_release);
            atomic_store_explicit(&g_lifecycle_background_seen, true, memory_order_release);
            errno = e;
            return -1;
        }
        memcpy(g_lifecycle_code_cache.base, code, sizeof(code));
        if (graft_jit_invalidate(&g_lifecycle_code_cache, 0, sizeof(code)) != 0 ||
            graft_jit_commit(&g_lifecycle_code_cache) != 0) {
            int e = errno;
            graft_jit_free(&g_lifecycle_code_cache);
            atomic_store_explicit(&g_lifecycle_prepare_error, e, memory_order_release);
            atomic_store_explicit(&g_lifecycle_background_seen, true, memory_order_release);
            errno = e;
            return -1;
        }
        atomic_store_explicit(&g_lifecycle_code_cache_ready, true, memory_order_release);
    }
    atomic_store_explicit(&g_lifecycle_prepare_error, 0, memory_order_release);
    atomic_store_explicit(&g_lifecycle_background_seen, true, memory_order_release);
    return 0;
}
int graft_lifecycle_note_foreground(void) {
    if (!atomic_load_explicit(&g_lifecycle_background_seen, memory_order_acquire)) {
        errno = EINVAL;
        return -1;
    }
    atomic_store_explicit(&g_lifecycle_foreground_seen, true, memory_order_release);
    return 0;
}

typedef int (*probe_fn)(char *summary, size_t summary_size, char *details, size_t details_size);
typedef struct probe_entry { const char *name; probe_fn fn; } probe_entry;
enum {
    PROBE_INTERNAL_SKIP = -2,
    PROBE_INTERNAL_BLOCKED = -3,
};

static void set_text(char *dst, size_t size, const char *fmt, ...) {
    va_list args; va_start(args, fmt); vsnprintf(dst, size, fmt, args); va_end(args);
}
static int wait_fd_deadline(int fd, short events, uint64_t deadline_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t current = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    if (current >= deadline_ms) { errno = ETIMEDOUT; return -1; }
    int timeout = (int)(deadline_ms - current);
    struct pollfd pfd = {.fd = fd, .events = events};
    int rc = poll(&pfd, 1, timeout);
    if (rc != 1 || (pfd.revents & (POLLERR | POLLNVAL)) ||
        ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN))) {
        errno = rc == 0 ? ETIMEDOUT : EPIPE; return -1;
    }
    return 0;
}
static int write_full_deadline(int fd, const void *buffer, size_t size, uint64_t deadline_ms) {
    const unsigned char *bytes = buffer;
    while (size) {
        if (wait_fd_deadline(fd, POLLOUT, deadline_ms) != 0) return -1;
        ssize_t n = write(fd, bytes, size);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        bytes += (size_t)n; size -= (size_t)n;
    }
    return 0;
}
static int read_full_deadline(int fd, void *buffer, size_t size, uint64_t deadline_ms) {
    unsigned char *bytes = buffer;
    while (size) {
        if (wait_fd_deadline(fd, POLLIN, deadline_ms) != 0) return -1;
        ssize_t n = read(fd, bytes, size);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; errno = EPIPE; return -1; }
        bytes += (size_t)n; size -= (size_t)n;
    }
    return 0;
}

static void binary_uuid(char output[37]) {
    strcpy(output, "unavailable");
#if defined(__APPLE__)
    const struct mach_header *header = _dyld_get_image_header(0);
    if (!header || header->magic != MH_MAGIC_64) return;
    const struct mach_header_64 *header64 = (const struct mach_header_64 *)header;
    const uint8_t *cursor = (const uint8_t *)(header64 + 1);
    const uint8_t *commands_end = cursor + header64->sizeofcmds;
    for (uint32_t index = 0; index < header64->ncmds; ++index) {
        if ((size_t)(commands_end - cursor) < sizeof(struct load_command)) return;
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(commands_end - cursor)) return;
        if (command->cmd == LC_UUID && command->cmdsize >= sizeof(struct uuid_command)) {
            const struct uuid_command *uuid = (const struct uuid_command *)command;
            snprintf(output, 37,
                     "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                     uuid->uuid[0], uuid->uuid[1], uuid->uuid[2], uuid->uuid[3],
                     uuid->uuid[4], uuid->uuid[5], uuid->uuid[6], uuid->uuid[7],
                     uuid->uuid[8], uuid->uuid[9], uuid->uuid[10], uuid->uuid[11],
                     uuid->uuid[12], uuid->uuid[13], uuid->uuid[14], uuid->uuid[15]);
            return;
        }
        cursor += command->cmdsize;
    }
#endif
}

static int runtime_paths(char *summary, size_t ss, char *details, size_t ds) {
    if (!g_guest_bundle_root[0] || !g_runtime_root[0] || !g_data_root[0] || !g_cache_root[0]) {
        set_text(summary, ss, "Explicit runtime path context is missing");
        set_text(details, ds, "{\"reason\":\"path_context_missing\",\"required\":[\"guest_bundle_root\",\"runtime_root\",\"data_root\",\"cache_root\"]}");
        return PROBE_INTERNAL_SKIP;
    }
    if (!g_bundle_url[0] || !g_bundle_executable_url[0] || !g_argv0[0] ||
        !g_current_working_directory[0] || !g_home_directory[0] ||
        !g_documents_directory[0] || !g_library_directory[0] ||
        !g_temporary_directory[0] || !g_app_version[0] ||
        !g_livecontainer_evidence[0]) {
        set_text(summary, ss, "Runtime observation metadata is missing");
        set_text(details, ds, "{\"reason\":\"runtime_observation_missing\"}");
        return PROBE_INTERNAL_SKIP;
    }
    char executable_path[PATH_MAX] = "unavailable";
#if defined(__APPLE__)
    uint32_t executable_path_size = (uint32_t)sizeof(executable_path);
    if (_NSGetExecutablePath(executable_path, &executable_path_size) != 0)
        strcpy(executable_path, "path-too-long");
#endif
    char uuid[37];
    binary_uuid(uuid);
    set_text(summary, ss, "Resolved explicit runtime path context");
    set_text(details, ds, "{\"source\":\"host_context\",\"guest_bundle_root\":\"%s\",\"runtime_root\":\"%s\",\"data_root\":\"%s\",\"cache_root\":\"%s\",\"ns_get_executable_path\":\"%s\",\"bundle_url\":\"%s\",\"bundle_executable_url\":\"%s\",\"argv0\":\"%s\",\"current_working_directory\":\"%s\",\"home_directory\":\"%s\",\"documents_directory\":\"%s\",\"library_directory\":\"%s\",\"temporary_directory\":\"%s\",\"binary_uuid\":\"%s\",\"app_version\":\"%s\",\"livecontainer_evidence\":\"%s\"}", g_guest_bundle_root, g_runtime_root, g_data_root, g_cache_root, executable_path, g_bundle_url, g_bundle_executable_url, g_argv0, g_current_working_directory, g_home_directory, g_documents_directory, g_library_directory, g_temporary_directory, uuid, g_app_version, g_livecontainer_evidence);
    return 0;
}

static int page_model(char *summary, size_t ss, char *details, size_t ds) {
    enum { logical_page_size = 4096, region_size = 64 * 1024,
           offset_count = region_size / logical_page_size };
    long page = getpagesize();
    long sys_page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || sys_page <= 0) {
        int e = errno ? errno : EINVAL;
        set_text(summary, ss, "Host page size query failed");
        set_text(details, ds, "{\"stage\":\"page_size\",\"os_error\":%d}", e);
        errno = e;
        return e;
    }
    size_t mapping_size = ((size_t)region_size + (size_t)page - 1u) /
                          (size_t)page * (size_t)page;
    void *map = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) {
        int e = errno;
        set_text(summary, ss, "64 KiB page-model mapping failed");
        set_text(details, ds,
                 "{\"stage\":\"mmap\",\"region_size\":%d,\"mapping_size\":%zu,\"os_error\":%d}",
                 region_size, mapping_size, e);
        errno = e;
        return e;
    }

    int results[offset_count];
    int errors[offset_count];
    int offset_successes = 0;
    for (size_t index = 0; index < offset_count; ++index) {
        size_t offset = index * (size_t)logical_page_size;
        errno = 0;
        results[index] = mprotect((char *)map + offset,
                                  (size_t)logical_page_size, PROT_READ);
        errors[index] = results[index] == 0 ? 0 : errno;
        if (results[index] == 0) ++offset_successes;
    }

    errno = 0;
    int first_permission_result = mprotect(map, (size_t)logical_page_size,
                                           PROT_READ);
    int first_permission_errno = first_permission_result == 0 ? 0 : errno;
    errno = 0;
    int second_permission_result = mprotect((char *)map + logical_page_size,
                                            (size_t)logical_page_size,
                                            PROT_READ | PROT_WRITE);
    int second_permission_errno = second_permission_result == 0 ? 0 : errno;
    bool same_host_page_test_applicable = page > logical_page_size;
    bool same_host_page_logical_permissions_separable =
        same_host_page_test_applicable && first_permission_result == 0 &&
        second_permission_result == 0;
    bool distinct_4k_permissions_supported =
        page <= logical_page_size || same_host_page_logical_permissions_separable;

    size_t used = (size_t)snprintf(
        details, ds,
        "{\"getpagesize\":%ld,\"sysconf_pagesize\":%ld,\"allocation_granularity\":%ld,"
        "\"logical_page_size\":%d,\"region_size\":%d,\"mapping_size\":%zu,"
        "\"mmap_flags\":\"MAP_PRIVATE|MAP_ANON\",\"offsets_tested\":%d,"
        "\"offset_successes\":%d,\"offset_results\":[",
        page, sys_page, page, logical_page_size, region_size, mapping_size,
        offset_count, offset_successes);
    bool details_fit = used < ds;
    for (size_t index = 0; details_fit && index < offset_count; ++index) {
        int written = snprintf(details + used, ds - used,
                               "%s{\"offset\":%zu,\"result\":%d,\"errno\":%d}",
                               index ? "," : "",
                               index * (size_t)logical_page_size,
                               results[index], errors[index]);
        if (written < 0 || (size_t)written >= ds - used) {
            details_fit = false;
        } else {
            used += (size_t)written;
        }
    }
    if (details_fit) {
        int written = snprintf(
            details + used, ds - used,
            "],\"same_host_page_test\":{\"applicable\":%s,\"first_offset\":0,"
            "\"first_result\":%d,\"first_errno\":%d,\"second_offset\":%d,"
            "\"second_result\":%d,\"second_errno\":%d},"
            "\"same_host_page_logical_permissions_separable\":%s,"
            "\"distinct_4k_permissions_supported\":%s}",
            same_host_page_test_applicable ? "true" : "false",
            first_permission_result, first_permission_errno, logical_page_size,
            second_permission_result, second_permission_errno,
            same_host_page_logical_permissions_separable ? "true" : "false",
            distinct_4k_permissions_supported ? "true" : "false");
        if (written < 0 || (size_t)written >= ds - used) details_fit = false;
    }
    munmap(map, mapping_size);
    if (!details_fit) {
        set_text(summary, ss, "Page-model evidence exceeded its JSON buffer");
        set_text(details, ds, "{\"stage\":\"serialize\",\"os_error\":%d}", EOVERFLOW);
        errno = EOVERFLOW;
        return EOVERFLOW;
    }
    set_text(summary, ss,
             distinct_4k_permissions_supported
                 ? "64 KiB page model measured; distinct 4 KiB permissions supported"
                 : "64 KiB page model measured; same-host-page 4 KiB permissions are not separable");
    return 0;
}

 #if defined(__aarch64__) || defined(__arm64__)
static int jit_call_42(void *base) {
    return ((int (*)(void))base)();
}
#endif

static int jit_basic(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return PROBE_INTERNAL_SKIP;
#else
    graft_jit_region region = {0}; if (graft_jit_alloc(4096, &region) != 0) { int e = errno; set_text(summary, ss, "JIT allocation failed"); set_text(details, ds, "{\"errno\":%d,\"jit_enabled\":false}", e); return (e == EACCES || e == EPERM) ? PROBE_INTERNAL_BLOCKED : e; }
    uint32_t code[] = { 0x52800540u, 0xD65F03C0u }; memcpy(region.base, code, sizeof(code));
    int result = graft_jit_invalidate(&region, 0, sizeof(code)); int protect = graft_jit_commit(&region); int value = protect == 0 ? jit_call_42(region.base) : -1;
    int backend_kind = region.backend;
    const char *backend = backend_kind == 2 ? "anonymous_rw_rx" : "MAP_JIT";
    int operation_error = errno;
    graft_jit_free(&region); if (result || protect || value != 42) { set_text(summary, ss, "JIT function did not return 42"); set_text(details, ds, "{\"invalidate\":%d,\"protect\":%d,\"value\":%d,\"errno\":%d,\"backend\":\"%s\"}", result, protect, value, operation_error, backend); errno = operation_error; return (protect == -1 && (operation_error == EACCES || operation_error == EPERM)) ? PROBE_INTERNAL_BLOCKED : EACCES; }
    set_text(summary, ss, "Executed ARM64 JIT function"); set_text(details, ds, "{\"return_value\":42,\"backend\":\"%s\"}", backend); return 0;
#endif
}

static int jit_write_protect(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return PROBE_INTERNAL_SKIP;
#else
    graft_jit_region region = {0};
    if (graft_jit_alloc(4096, &region) != 0) {
        int e = errno;
        set_text(summary, ss, "JIT write-protect allocation failed");
        set_text(details, ds, "{\"stage\":\"alloc\",\"os_error\":%d}", e);
        return (e == EACCES || e == EPERM) ? PROBE_INTERNAL_BLOCKED : e;
    }
    uint32_t code[] = { 0x52800020u, 0xD65F03C0u }; memcpy(region.base, code, sizeof(code)); graft_jit_invalidate(&region, 0, sizeof(code));
    int rx1 = graft_jit_commit(&region); int first = rx1 == 0 ? jit_call_42(region.base) : -1;
    int rw = graft_jit_begin_write(&region); code[0] = 0x52800140u; memcpy(region.base, code, sizeof(code)); graft_jit_invalidate(&region, 0, sizeof(code)); int rx2 = graft_jit_commit(&region); int second = rx2 == 0 ? jit_call_42(region.base) : -1;
    graft_jit_free(&region); set_text(summary, ss, "JIT write/execute transitions completed"); set_text(details, ds, "{\"first\":%d,\"second\":%d,\"begin_write\":%d}", first, second, rw); return (first == 1 && second == 10 && rw == 0) ? 0 : EACCES;
#endif
}

typedef struct thread_arg { graft_jit_region *region; int failures; } thread_arg;
#if defined(__aarch64__) || defined(__arm64__)
static void *jit_thread(void *opaque) { thread_arg *arg = (thread_arg *)opaque; for (int i = 0; i < 1000; ++i) { if (jit_call_42(arg->region->base) != 42) arg->failures++; } return NULL; }
#endif
static int jit_multithread(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return PROBE_INTERNAL_SKIP;
#else
    int basic_result = jit_basic(summary, ss, details, ds);
    if (basic_result != 0) return basic_result;
    graft_jit_region region = {0};
    if (graft_jit_alloc(4096, &region) != 0) {
        int e = errno;
        set_text(summary, ss, "Multithread JIT allocation failed");
        set_text(details, ds, "{\"stage\":\"alloc\",\"os_error\":%d}", e);
        return (e == EACCES || e == EPERM) ? PROBE_INTERNAL_BLOCKED : e;
    }
    uint32_t code[] = { 0x52800540u, 0xD65F03C0u }; memcpy(region.base, code, sizeof(code)); graft_jit_invalidate(&region, 0, sizeof(code)); if (graft_jit_commit(&region) != 0) { graft_jit_free(&region); return EACCES; }
    pthread_t threads[4]; thread_arg args[4] = {{0}}; for (int i = 0; i < 4; ++i) { args[i].region = &region; pthread_create(&threads[i], NULL, jit_thread, &args[i]); }
    int failures = 0; for (int i = 0; i < 4; ++i) { pthread_join(threads[i], NULL); failures += args[i].failures; } graft_jit_free(&region);
    set_text(summary, ss, "Four-thread JIT execution completed"); set_text(details, ds, "{\"threads\":4,\"iterations_per_thread\":1000,\"failures\":%d}", failures); return failures ? EIO : 0;
#endif
}

#if defined(__aarch64__) || defined(__arm64__)
static _Thread_local volatile sig_atomic_t g_fault_seen;
static _Thread_local uintptr_t g_recovery_pc;
static _Thread_local uintptr_t g_fault_pc;
static _Thread_local uintptr_t g_fault_address;
static _Thread_local int g_fault_signal;
static void signal_handler(int signal_number, siginfo_t *info, void *context) {
    g_fault_signal = signal_number;
    g_fault_address = (uintptr_t)(info ? info->si_addr : 0);
    g_fault_seen = 1;
#if defined(__aarch64__) || defined(__arm64__)
    ucontext_t *uc = (ucontext_t *)context;
#if defined(__APPLE__)
    g_fault_pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    uc->uc_mcontext->__ss.__pc = (uint64_t)g_recovery_pc;
#else
    g_fault_pc = (uintptr_t)uc->uc_mcontext.pc;
    uc->uc_mcontext.pc = g_recovery_pc;
#endif
#else
    (void)context;
#endif
}

static int signal_resume_once(int expected_signal) {
    size_t stack_size = (size_t)SIGSTKSZ;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED) return errno;
    stack_t alt = {.ss_sp = stack, .ss_size = stack_size, .ss_flags = 0};
    stack_t previous = {0};
    if (sigaltstack(&alt, &previous) != 0) { int e = errno; munmap(stack, stack_size); return e; }
    g_fault_seen = 0;
    g_fault_signal = 0; g_fault_pc = 0; g_fault_address = 0;
    g_recovery_pc = (uintptr_t)&&recovered;
    int result = EFAULT;
    if (expected_signal == SIGBUS) {
        char path[PATH_MAX];
        const char *signal_dir = (g_cache_root[0] && access(g_cache_root, W_OK) == 0) ? g_cache_root : "/tmp";
        if (snprintf(path, sizeof(path), "%s/.graft64-signal-XXXXXX", signal_dir) >= (int)sizeof(path)) {
            result = ENAMETOOLONG;
            goto cleanup;
        }
        int fd = mkstemp(path);
        if (fd < 0) { result = errno; goto cleanup; }
        unlink(path);
        size_t page = (size_t)getpagesize();
        void *mapped = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) { result = errno; goto cleanup; }
        volatile unsigned char value = *((volatile unsigned char *)mapped);
        (void)value;
        munmap(mapped, page);
    } else {
        volatile int *bad = (volatile int *)(uintptr_t)0x1;
        *bad = 7;
    }
recovered:
    result = g_fault_seen && g_fault_signal == expected_signal ? 0 : EFAULT;
cleanup:
    (void)sigaltstack(&previous, NULL);
    munmap(stack, stack_size);
    return result;
}

typedef struct signal_worker_result { int result; int segv; int bus; uintptr_t address[2]; uintptr_t fault_pc[2]; uintptr_t recovery_pc[2]; } signal_worker_result;
static void *signal_worker(void *opaque) {
    signal_worker_result *result = (signal_worker_result *)opaque;
    result->result = signal_resume_once(SIGSEGV);
    result->segv = g_fault_signal == SIGSEGV;
    result->address[0] = g_fault_address; result->fault_pc[0] = g_fault_pc; result->recovery_pc[0] = g_recovery_pc;
    int bus_result = signal_resume_once(SIGBUS);
    result->bus = g_fault_signal == SIGBUS;
    result->address[1] = g_fault_address; result->fault_pc[1] = g_fault_pc; result->recovery_pc[1] = g_recovery_pc;
    if (result->result == 0 && bus_result != 0) result->result = bus_result;
    return NULL;
}
#endif
static int signal_resume(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Run on arm64 device and capture ucontext\"}"); return PROBE_INTERNAL_SKIP;
#else
    struct sigaction action = {0}, old_action = {0};
    action.sa_sigaction = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    struct sigaction old_bus = {0};
    if (sigaction(SIGSEGV, &action, &old_action) != 0) return errno;
    if (sigaction(SIGBUS, &action, &old_bus) != 0) {
        int e = errno;
        (void)sigaction(SIGSEGV, &old_action, NULL);
        errno = e;
        return e;
    }
    signal_worker_result main = {0};
    main.result = signal_resume_once(SIGSEGV);
    main.segv = g_fault_signal == SIGSEGV; main.address[0] = g_fault_address; main.fault_pc[0] = g_fault_pc; main.recovery_pc[0] = g_recovery_pc;
    int main_bus = signal_resume_once(SIGBUS);
    main.bus = g_fault_signal == SIGBUS; main.address[1] = g_fault_address; main.fault_pc[1] = g_fault_pc; main.recovery_pc[1] = g_recovery_pc;
    if (main.result == 0 && main_bus != 0) main.result = main_bus;
    signal_worker_result worker = {.result = EFAULT};
    pthread_t worker_thread;
    int worker_create = pthread_create(&worker_thread, NULL, signal_worker, &worker);
    if (worker_create == 0) pthread_join(worker_thread, NULL);
    else worker.result = worker_create;
    int restore_segv = sigaction(SIGSEGV, &old_action, NULL);
    int restore_bus = sigaction(SIGBUS, &old_bus, NULL);
    int restore_result = restore_segv != 0 || restore_bus != 0;
    int ok = main.result == 0 && worker.result == 0 && restore_result == 0 && main.segv && main.bus && worker.segv && worker.bus;
    set_text(summary, ss, ok ? "SIGSEGV/SIGBUS recovered through TLS ucontext (main and worker)" : "Fault handler did not recover all signals on all threads");
    set_text(details, ds, "{\"signals\":[\"SIGSEGV\",\"SIGBUS\"],\"main\":{\"segv\":%s,\"bus\":%s,\"fault_address_segv\":%llu,\"fault_address_bus\":%llu,\"original_pc_segv\":%llu,\"original_pc_bus\":%llu,\"recovery_pc_segv\":%llu,\"recovery_pc_bus\":%llu},\"worker\":{\"segv\":%s,\"bus\":%s,\"fault_address_segv\":%llu,\"fault_address_bus\":%llu,\"original_pc_segv\":%llu,\"original_pc_bus\":%llu,\"recovery_pc_segv\":%llu,\"recovery_pc_bus\":%llu},\"main_error\":%d,\"worker_error\":%d}", main.segv ? "true" : "false", main.bus ? "true" : "false", (unsigned long long)main.address[0], (unsigned long long)main.address[1], (unsigned long long)main.fault_pc[0], (unsigned long long)main.fault_pc[1], (unsigned long long)main.recovery_pc[0], (unsigned long long)main.recovery_pc[1], worker.segv ? "true" : "false", worker.bus ? "true" : "false", (unsigned long long)worker.address[0], (unsigned long long)worker.address[1], (unsigned long long)worker.fault_pc[0], (unsigned long long)worker.fault_pc[1], (unsigned long long)worker.recovery_pc[0], (unsigned long long)worker.recovery_pc[1], main.result, worker.result);
    return ok ? 0 : (main.result != 0 ? main.result : worker.result);
#endif
}
static int dlopen_bundle(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__APPLE__)
    set_text(summary, ss, "Bundle dylib probe requires Darwin"); set_text(details, ds, "{\"status\":\"unverified\"}"); return PROBE_INTERNAL_SKIP;
#else
    if (!g_dylib_path[0]) { set_text(summary, ss, "Bundle dylib path is not configured"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Configure GraftProbeTest.dylib path from app bundle\"}"); return PROBE_INTERNAL_SKIP; }
    void *handle = dlopen(g_dylib_path, RTLD_NOW | RTLD_LOCAL); if (!handle) { const char *error = dlerror(); set_text(summary, ss, "dlopen failed"); set_text(details, ds, "{\"path\":\"%s\",\"error\":\"%s\"}", g_dylib_path, error ? error : "unknown"); return EFAULT; }
    int (*value)(void) = (int (*)(void))dlsym(handle, "graft_probe_test_value"); const char *error = dlerror(); int result = value ? value() : -1; dlclose(handle); if (error || result != 64) { set_text(summary, ss, "dlsym validation failed"); set_text(details, ds, "{\"path\":\"%s\",\"return_value\":%d}", g_dylib_path, result); return EFAULT; }
    set_text(summary, ss, "Loaded and called bundled dylib"); set_text(details, ds, "{\"path\":\"%s\",\"return_value\":64}", g_dylib_path); return 0;
#endif
}
static int unix_socket_probe(char *summary, size_t ss, char *details, size_t ds) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return errno;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t deadline_ms = (uint64_t)now.tv_sec * 1000u +
                           (uint64_t)now.tv_nsec / 1000000u + 2000u;
    const char request[] = "graft64-request";
    const char response[] = "graft64-response";
    char request_out[sizeof(request)] = {0};
    char response_out[sizeof(response)] = {0};
    int request_ok = write_full_deadline(fds[0], request, sizeof(request), deadline_ms) == 0 &&
                     read_full_deadline(fds[1], request_out, sizeof(request_out), deadline_ms) == 0 &&
                     memcmp(request, request_out, sizeof(request)) == 0;
    int response_ok = request_ok &&
                      write_full_deadline(fds[1], response, sizeof(response), deadline_ms) == 0 &&
                      read_full_deadline(fds[0], response_out, sizeof(response_out), deadline_ms) == 0 &&
                      memcmp(response, response_out, sizeof(response)) == 0;
    int close_ok = 0;
    if (response_ok && shutdown(fds[0], SHUT_WR) == 0) {
        struct pollfd pfd = {.fd = fds[1], .events = POLLIN};
        int poll_result = poll(&pfd, 1, 2000);
        unsigned char byte = 0;
        close_ok = poll_result == 1 && (pfd.revents & (POLLIN | POLLHUP)) &&
                   read(fds[1], &byte, 1) == 0;
    }
    close(fds[0]); close(fds[1]);
    int ok = request_ok && response_ok && close_ok;
    set_text(summary, ss, ok ? "Bidirectional Unix socket and close handling completed" : "Unix socket round trip failed");
    set_text(details, ds, "{\"bidirectional\":%s,\"request_bytes\":%zu,\"response_bytes\":%zu,\"deadline_ms\":2000,\"peer_close_observed\":%s}", request_ok && response_ok ? "true" : "false", sizeof(request), sizeof(response), close_ok ? "true" : "false");
    return ok ? 0 : EIO;
}

typedef enum helper_probe_mode {
    HELPER_PROBE_SPAWN,
    HELPER_PROBE_SHARED,
    HELPER_PROBE_IPC,
} helper_probe_mode;

static int helper_roundtrip(helper_probe_mode mode, char *summary, size_t ss,
                            char *details, size_t ds) {
    if (!g_helper_path[0]) { set_text(summary, ss, "Bundled helper path is not configured"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Configure helper path from app bundle\"}"); return PROBE_INTERNAL_SKIP; }
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) { int e = errno; set_text(summary, ss, "Helper socketpair failed"); set_text(details, ds, "{\"stage\":\"socketpair\",\"os_error\":%d}", e); return e; }
    char shared_path[PATH_MAX];
    const char *shared_dir = (g_cache_root[0] && access(g_cache_root, W_OK) == 0) ? g_cache_root : "/tmp";
    if (snprintf(shared_path, sizeof(shared_path), "%s/.graft64-helper-shm-XXXXXX", shared_dir) >= (int)sizeof(shared_path)) {
        close(fds[0]); close(fds[1]); errno = ENAMETOOLONG;
        set_text(summary, ss, "Helper shared-memory path is too long");
        set_text(details, ds, "{\"stage\":\"shared_path\",\"os_error\":%d}", errno);
        return errno;
    }
    int shared_fd = mkstemp(shared_path);
    if (shared_fd < 0) { int e = errno; close(fds[0]); close(fds[1]); set_text(summary, ss, "Helper shared-memory file creation failed"); set_text(details, ds, "{\"stage\":\"mkstemp\",\"os_error\":%d}", e); return e; }
    unlink(shared_path);
    const size_t shared_size = (size_t)getpagesize();
    if (ftruncate(shared_fd, (off_t)shared_size) != 0) { int e = errno; close(shared_fd); close(fds[0]); close(fds[1]); set_text(summary, ss, "Helper shared-memory resize failed"); set_text(details, ds, "{\"stage\":\"ftruncate\",\"os_error\":%d}", e); return e; }
    graft_helper_shared_state *shared = mmap(NULL, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, shared_fd, 0);
    if (shared == MAP_FAILED) { int e = errno; close(shared_fd); close(fds[0]); close(fds[1]); set_text(summary, ss, "Helper shared-memory map failed"); set_text(details, ds, "{\"stage\":\"mmap\",\"os_error\":%d}", e); return e; }
    memset(shared, 0, sizeof(*shared));
    shared->parent_magic = 0x4752414654504152ull;
    __sync_synchronize();
    const graft_helper_launch_config launch = {
        .path = g_helper_path,
        .parent_socket_fd = fds[0],
        .socket_fd = fds[1],
        .shared_fd = shared_fd,
        .shared_size = shared_size,
    };
    graft_helper_process process = {0};
    int spawn_result = graft_spawn_helper(&launch, &process);
    int spawn_error = errno;
    close(fds[1]);
    if (spawn_result != 0) { munmap(shared, shared_size); close(shared_fd); close(fds[0]); errno = spawn_error; set_text(summary, ss, "Helper process spawn failed"); set_text(details, ds, "{\"stage\":\"host_adapter\",\"os_error\":%d}", spawn_error); return spawn_error; }
    pid_t pid = (pid_t)process.pid;
    graft_msg_header requests[5] = {{0}};
    size_t request_count = 0;
    requests[request_count++] = (graft_msg_header){GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_HELLO, 0, 1};
    if (mode == HELPER_PROBE_IPC)
        requests[request_count++] = (graft_msg_header){GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_PING, 0, 2};
    if (mode != HELPER_PROBE_SPAWN)
        requests[request_count++] = (graft_msg_header){GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_SHARED_MEMORY, 0, 3};
    if (mode == HELPER_PROBE_IPC)
        requests[request_count++] = (graft_msg_header){GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_PING, 0, 4};
    requests[request_count++] = (graft_msg_header){GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_SHUTDOWN, 0, 5};
    int ok = 1;
    graft_helper_hello_payload hello = {0};
    struct timespec deadline_clock;
    clock_gettime(CLOCK_MONOTONIC, &deadline_clock);
    const uint64_t deadline_ms = (uint64_t)deadline_clock.tv_sec * 1000u + (uint64_t)deadline_clock.tv_nsec / 1000000u + 10000u;
    for (size_t i = 0; i < request_count; ++i) {
        if (write_full_deadline(fds[0], &requests[i], sizeof(requests[i]), deadline_ms) != 0) { ok = 0; break; }
        graft_msg_header response;
        if (read_full_deadline(fds[0], &response, sizeof(response), deadline_ms) != 0 || graft_ipc_validate_header(&response) != 0 || response.request_id != requests[i].request_id) { ok = 0; break; }
        if (response.payload_size) {
            if (response.payload_size != sizeof(hello) || read_full_deadline(fds[0], &hello, sizeof(hello), deadline_ms) != 0) { ok = 0; break; }
        }
    }
    __sync_synchronize();
    int shared_ok = shared->parent_magic == 0x4752414654504152ull && shared->helper_magic == 0x4752414654484c50ull && shared->helper_pid == (uint64_t)pid && shared->heartbeat >= request_count;
    uint64_t heartbeat = shared->heartbeat;
    uint64_t helper_magic = shared->helper_magic;
    uint64_t helper_pid = shared->helper_pid;
    munmap(shared, shared_size); close(shared_fd); close(fds[0]);
    int status = 0; int wait_ok = 0;
    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) { wait_ok = 1; break; }
        if (waited < 0 && errno != EINTR) break;
        struct timespec pause_for = {.tv_sec = 0, .tv_nsec = 10000000}; nanosleep(&pause_for, NULL);
        clock_gettime(CLOCK_MONOTONIC, &deadline_clock);
        uint64_t now_ms = (uint64_t)deadline_clock.tv_sec * 1000u + (uint64_t)deadline_clock.tv_nsec / 1000000u;
        if (now_ms >= deadline_ms) { errno = ETIMEDOUT; break; }
    }
    if (!wait_ok) { kill(pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
    int exited_ok = wait_ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int require_shared = mode != HELPER_PROBE_SPAWN;
    int result_ok = ok && exited_ok && hello.pid > 0 && hello.page_size > 0 &&
                    hello.nonce != 0 && (!require_shared || shared_ok);
    const char *mode_name = mode == HELPER_PROBE_SPAWN ? "spawn" :
                            mode == HELPER_PROBE_SHARED ? "shared_mapping" : "ipc";
    const char *success_summary = mode == HELPER_PROBE_SPAWN ? "Bundled helper spawned and exited cleanly" :
                                  mode == HELPER_PROBE_SHARED ? "Cross-process shared mapping completed" :
                                  "Persistent bidirectional helper IPC completed";
    set_text(summary, ss, result_ok ? success_summary : "Helper process validation failed");
    set_text(details, ds, "{\"mode\":\"%s\",\"spawned\":true,\"request_count\":%zu,\"persistent_requests\":%zu,\"exit_code\":%d,\"wait_completed\":%s,\"pid\":%lld,\"page_size\":%llu,\"nonce_present\":%s,\"shared_round_trip\":%s,\"mmap_flags\":\"MAP_SHARED\",\"shared_path_root\":\"%s\",\"helper_magic\":%llu,\"shared_helper_pid\":%llu,\"heartbeat\":%llu}", mode_name, request_count, request_count - 1u, wait_ok && WIFEXITED(status) ? WEXITSTATUS(status) : 128, wait_ok ? "true" : "false", (long long)hello.pid, (unsigned long long)hello.page_size, hello.nonce ? "true" : "false", shared_ok ? "true" : "false", shared_dir, (unsigned long long)helper_magic, (unsigned long long)helper_pid, (unsigned long long)heartbeat);
    return result_ok ? 0 : EIO;
}
static int shared_mapping(char *summary, size_t ss, char *details, size_t ds) { return helper_roundtrip(HELPER_PROBE_SHARED, summary, ss, details, ds); }
static int helper_spawn_probe(char *summary, size_t ss, char *details, size_t ds) { return helper_roundtrip(HELPER_PROBE_SPAWN, summary, ss, details, ds); }
static int helper_ipc_probe(char *summary, size_t ss, char *details, size_t ds) { return helper_roundtrip(HELPER_PROBE_IPC, summary, ss, details, ds); }
static int lifecycle_jit(char *summary, size_t ss, char *details, size_t ds) {
    if (!atomic_load_explicit(&g_lifecycle_background_seen, memory_order_acquire) ||
        !atomic_load_explicit(&g_lifecycle_foreground_seen, memory_order_acquire)) {
        set_text(summary, ss, "Waiting for background/foreground transition");
        set_text(details, ds, "{\"status\":\"manual\",\"instructions\":\"Send the app to background and return to foreground; lifecycle_jit will run automatically\"}");
        return PROBE_INTERNAL_SKIP;
    }
    int prepare_error = atomic_load_explicit(&g_lifecycle_prepare_error, memory_order_acquire);
    if (prepare_error != 0) {
        set_text(summary, ss, "JIT code cache preparation failed before suspend");
        set_text(details, ds, "{\"status\":\"fail\",\"stage\":\"prepare_before_suspend\",\"cache_reused\":false,\"os_error\":%d}", prepare_error);
        errno = prepare_error;
        return (prepare_error == EACCES || prepare_error == EPERM)
                   ? PROBE_INTERNAL_BLOCKED : prepare_error;
    }
    if (!atomic_load_explicit(&g_lifecycle_code_cache_ready, memory_order_acquire)) {
        set_text(summary, ss, "JIT code cache was not prepared before suspend");
        set_text(details, ds, "{\"status\":\"fail\",\"cache_reused\":false}");
        return EINVAL;
    }
#if defined(__aarch64__) || defined(__arm64__)
    int value = jit_call_42(g_lifecycle_code_cache.base);
#else
    int value = -1;
#endif
    int rc = value == 42 ? 0 : EIO;
    if (rc == 0) {
        set_text(summary, ss, "JIT resumed after background/foreground transition");
        set_text(details, ds, "{\"status\":\"pass\",\"background_seen\":true,\"foreground_seen\":true,\"jit_return_value\":42,\"cache_reused\":true}");
    } else {
        set_text(summary, ss, "Original JIT code cache failed after resume");
        set_text(details, ds, "{\"status\":\"fail\",\"cache_reused\":true,\"jit_return_value\":%d}", value);
    }
    return rc;
}

static const probe_entry probes[] = {
    {"runtime_paths", runtime_paths}, {"page_model", page_model}, {"jit_basic", jit_basic}, {"jit_write_protect", jit_write_protect}, {"jit_multithread", jit_multithread}, {"signal_resume", signal_resume}, {"dlopen_bundle", dlopen_bundle}, {"unix_socket", unix_socket_probe}, {"shared_mapping", shared_mapping}, {"helper_spawn", helper_spawn_probe}, {"helper_ipc", helper_ipc_probe}, {"lifecycle_jit", lifecycle_jit},
};

const char *graft_probe_status_name(graft_probe_status status) { switch (status) { case GRAFT_PROBE_PASS: return "PASS"; case GRAFT_PROBE_FAIL: return "FAIL"; case GRAFT_PROBE_SKIP: return "SKIP"; case GRAFT_PROBE_BLOCKED: return "BLOCKED"; default: return "UNKNOWN"; } }

static graft_probe_reason_code reason_for_result(const char *name, graft_probe_status status, int os_error) {
    if (status == GRAFT_PROBE_PASS) return GRAFT_REASON_NONE;
    if (status == GRAFT_PROBE_SKIP) return strcmp(name, "lifecycle_jit") == 0 ? GRAFT_REASON_LIFECYCLE_WAITING : GRAFT_REASON_NOT_SUPPORTED;
    if (status == GRAFT_PROBE_BLOCKED) return GRAFT_REASON_JIT_NOT_ENABLED;
    if (strncmp(name, "helper", 6) == 0 && (os_error == ENOENT || os_error == ESRCH)) return GRAFT_REASON_CHILD_PROCESS;
    if (os_error == EACCES || os_error == EPERM) return GRAFT_REASON_PERMISSION;
    if (os_error == EINVAL) return GRAFT_REASON_INVALID_ARGUMENT;
    if (os_error == EIO || os_error == EPIPE || os_error == ETIMEDOUT) return GRAFT_REASON_IO;
    if (strncmp(name, "helper", 6) == 0) return GRAFT_REASON_CHILD_PROCESS;
    return GRAFT_REASON_INTERNAL;
}

static graft_error_code graft_error_for_result(graft_probe_status status, int os_error) {
    if (status == GRAFT_PROBE_PASS) return GRAFT_ERROR_NONE;
    if (status == GRAFT_PROBE_SKIP) return GRAFT_ERROR_UNSUPPORTED;
    if (status == GRAFT_PROBE_BLOCKED && os_error != 0) return GRAFT_ERROR_PERMISSION;
    if (os_error == EACCES || os_error == EPERM) return GRAFT_ERROR_PERMISSION;
    if (os_error == EINVAL) return GRAFT_ERROR_ARGUMENT;
    if (os_error != 0) return GRAFT_ERROR_IO;
    return GRAFT_ERROR_INTERNAL;
}

static const probe_entry *find_probe(const char *name) { for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) if (strcmp(name, probes[i].name) == 0) return &probes[i]; return NULL; }
int graft_run_probe(const char *name, graft_probe_callback callback, void *context) {
    if (!name || !callback) { errno = EINVAL; return -1; } const probe_entry *entry = find_probe(name); if (!entry) { errno = ENOENT; return -1; }
    char summary[256] = {0}, details[8192] = {0}; struct timespec start = {0}, end = {0}; clock_gettime(CLOCK_MONOTONIC, &start); errno = 0; int rc = entry->fn(summary, sizeof(summary), details, sizeof(details)); int saved_errno = errno; clock_gettime(CLOCK_MONOTONIC, &end);
    graft_probe_status status = rc == 0 ? GRAFT_PROBE_PASS : rc == PROBE_INTERNAL_SKIP ? GRAFT_PROBE_SKIP : rc == PROBE_INTERNAL_BLOCKED ? GRAFT_PROBE_BLOCKED : GRAFT_PROBE_FAIL;
    int os_error = rc > 0 ? rc : saved_errno;
    graft_probe_result result = {.name = entry->name, .status = status, .reason_code = reason_for_result(entry->name, status, os_error), .graft_error = graft_error_for_result(status, os_error), .os_error = os_error, .duration_ns = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ull + (uint64_t)(end.tv_nsec - start.tv_nsec), .summary = summary, .details_json = details}; callback(&result, context); return 0;
}
int graft_run_all_probes(graft_probe_callback callback, void *context) { if (!callback) { errno = EINVAL; return -1; } for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) if (graft_run_probe(probes[i].name, callback, context) != 0) return -1; return 0; }
