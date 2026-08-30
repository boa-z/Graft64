#include "graft/graft_probe.h"
#include "graft/graft_jit.h"
#include "graft/graft_ipc_protocol.h"
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
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

extern char **environ;
static char g_helper_path[PATH_MAX];
static char g_dylib_path[PATH_MAX];
static char g_guest_bundle_root[PATH_MAX];
static char g_runtime_root[PATH_MAX];
static char g_data_root[PATH_MAX];
static char g_cache_root[PATH_MAX];
static atomic_bool g_lifecycle_background_seen;
static atomic_bool g_lifecycle_foreground_seen;
static graft_jit_region g_lifecycle_code_cache;
static atomic_bool g_lifecycle_code_cache_ready;
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
int graft_lifecycle_note_background(void) {
    /* Prepare while the app is still running. The foreground phase must only
     * execute this existing cache, proving suspend/resume preservation. */
    if (!atomic_load_explicit(&g_lifecycle_code_cache_ready, memory_order_acquire)) {
        uint32_t code[] = { 0x52800540u, 0xD65F03C0u }; /* mov w0,#42; ret */
        if (graft_jit_alloc(4096, &g_lifecycle_code_cache) != 0) return -1;
        if (graft_jit_begin_write(&g_lifecycle_code_cache) != 0) {
            graft_jit_free(&g_lifecycle_code_cache);
            return -1;
        }
        memcpy(g_lifecycle_code_cache.base, code, sizeof(code));
        if (graft_jit_invalidate(&g_lifecycle_code_cache, 0, sizeof(code)) != 0 ||
            graft_jit_commit(&g_lifecycle_code_cache) != 0) {
            graft_jit_free(&g_lifecycle_code_cache);
            return -1;
        }
        atomic_store_explicit(&g_lifecycle_code_cache_ready, true, memory_order_release);
    }
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

static int runtime_paths(char *summary, size_t ss, char *details, size_t ds) {
    if (!g_guest_bundle_root[0] || !g_runtime_root[0] || !g_data_root[0] || !g_cache_root[0]) {
        set_text(summary, ss, "Explicit runtime path context is missing");
        set_text(details, ds, "{\"reason\":\"path_context_missing\",\"required\":[\"guest_bundle_root\",\"runtime_root\",\"data_root\",\"cache_root\"]}");
        return 2;
    }
    set_text(summary, ss, "Resolved explicit runtime path context");
    set_text(details, ds, "{\"source\":\"host_context\",\"guest_bundle_root\":\"%s\",\"runtime_root\":\"%s\",\"data_root\":\"%s\",\"cache_root\":\"%s\"}", g_guest_bundle_root, g_runtime_root, g_data_root, g_cache_root);
    return 0;
}

static int page_model(char *summary, size_t ss, char *details, size_t ds) {
    long page = getpagesize(); long sys_page = sysconf(_SC_PAGESIZE);
    size_t len = (size_t)page * 2; void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) { set_text(summary, ss, "mmap failed"); set_text(details, ds, "{\"errno\":%d}", errno); return errno; }
    int first = mprotect((char *)map + page, (size_t)page, PROT_READ);
    int second = mprotect((char *)map + page / 2, (size_t)page, PROT_READ | PROT_EXEC);
    int offsets_tested = 0, offset_successes = 0;
    for (size_t offset = 0; offset < 65536; offset += 4096) {
        ++offsets_tested;
        if (mprotect((char *)map + (offset % len), 4096, PROT_READ) == 0) ++offset_successes;
    }
    munmap(map, len);
    set_text(summary, ss, "Host page model measured");
    set_text(details, ds, "{\"getpagesize\":%ld,\"sysconf_pagesize\":%ld,\"allocation_granularity\":%ld,\"mprotect_aligned\":%d,\"mprotect_unaligned\":%d,\"offsets_tested\":%d,\"offset_successes\":%d}", page, sys_page, page, first, second, offsets_tested, offset_successes);
    return 0;
}

 #if defined(__aarch64__) || defined(__arm64__)
static int jit_call_42(void *base) {
    return ((int (*)(void))base)();
}
#endif

static int jit_basic(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return 2;
#else
    graft_jit_region region = {0}; if (graft_jit_alloc(4096, &region) != 0) { int e = errno; set_text(summary, ss, "JIT allocation failed"); set_text(details, ds, "{\"errno\":%d,\"jit_enabled\":false}", e); return (e == EACCES || e == EPERM) ? 3 : e; }
    uint32_t code[] = { 0x52800540u, 0xD65F03C0u }; memcpy(region.base, code, sizeof(code));
    int result = graft_jit_invalidate(&region, 0, sizeof(code)); int protect = graft_jit_commit(&region); int value = protect == 0 ? jit_call_42(region.base) : -1;
    int backend_kind = region.backend;
    const char *backend = backend_kind == 2 ? "debugged_anonymous" : "MAP_JIT";
    graft_jit_free(&region); if (result || protect || value != 42) { set_text(summary, ss, "JIT function did not return 42"); set_text(details, ds, "{\"invalidate\":%d,\"protect\":%d,\"value\":%d,\"errno\":%d,\"backend\":\"%s\"}", result, protect, value, errno, backend); return (protect == -1 && (errno == EACCES || errno == EPERM)) ? 3 : EACCES; }
    set_text(summary, ss, "Executed ARM64 JIT function"); set_text(details, ds, "{\"return_value\":42,\"backend\":\"%s\"}", backend); return 0;
#endif
}

static int jit_write_protect(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return 2;
#else
    graft_jit_region region = {0};
    if (graft_jit_alloc(4096, &region) != 0) {
        int e = errno;
        set_text(summary, ss, "JIT write-protect allocation failed");
        set_text(details, ds, "{\"stage\":\"alloc\",\"os_error\":%d}", e);
        return (e == EACCES || e == EPERM) ? 3 : e;
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
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return 2;
#else
    int basic_result = jit_basic(summary, ss, details, ds);
    if (basic_result != 0) return basic_result;
    graft_jit_region region = {0};
    if (graft_jit_alloc(4096, &region) != 0) {
        int e = errno;
        set_text(summary, ss, "Multithread JIT allocation failed");
        set_text(details, ds, "{\"stage\":\"alloc\",\"os_error\":%d}", e);
        return (e == EACCES || e == EPERM) ? 3 : e;
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
static void signal_handler(int signal_number, siginfo_t *info, void *context) {
    (void)signal_number; (void)info;
    g_fault_seen = 1;
#if defined(__aarch64__) || defined(__arm64__)
    ucontext_t *uc = (ucontext_t *)context;
#if defined(__APPLE__)
    uc->uc_mcontext->__ss.__pc = (uint64_t)g_recovery_pc;
#else
    uc->uc_mcontext.pc = g_recovery_pc;
#endif
#else
    (void)context;
#endif
}

static int signal_resume_once(void) {
    size_t stack_size = (size_t)SIGSTKSZ;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED) return errno;
    stack_t alt = {.ss_sp = stack, .ss_size = stack_size, .ss_flags = 0};
    stack_t previous = {0};
    if (sigaltstack(&alt, &previous) != 0) { int e = errno; munmap(stack, stack_size); return e; }
    g_fault_seen = 0;
    g_recovery_pc = (uintptr_t)&&recovered;
    int result = EFAULT;
    volatile int *bad = (volatile int *)(uintptr_t)0x1;
    *bad = 7;
recovered:
    result = g_fault_seen ? 0 : EFAULT;
    (void)sigaltstack(&previous, NULL);
    munmap(stack, stack_size);
    return result;
}

typedef struct signal_worker_result { int result; } signal_worker_result;
static void *signal_worker(void *opaque) {
    signal_worker_result *result = (signal_worker_result *)opaque;
    result->result = signal_resume_once();
    return NULL;
}
#endif
static int signal_resume(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Run on arm64 device and capture ucontext\"}"); return 2;
#else
    struct sigaction action = {0}, old_action = {0};
    action.sa_sigaction = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &action, &old_action) != 0) return errno;
    int main_result = signal_resume_once();
    signal_worker_result worker = {.result = EFAULT};
    pthread_t worker_thread;
    int worker_create = pthread_create(&worker_thread, NULL, signal_worker, &worker);
    if (worker_create == 0) pthread_join(worker_thread, NULL);
    else worker.result = worker_create;
    int restore_result = sigaction(SIGSEGV, &old_action, NULL);
    int ok = main_result == 0 && worker.result == 0 && restore_result == 0;
    set_text(summary, ss, ok ? "SIGSEGV recovered through TLS ucontext (main and worker)" : "Fault handler did not recover on all threads");
    set_text(details, ds, "{\"fault_signal\":\"SIGSEGV\",\"ucontext_pc_rewritten\":%s,\"worker_thread_fault_recovered\":%s,\"main_error\":%d,\"worker_error\":%d}", main_result == 0 ? "true" : "false", worker.result == 0 ? "true" : "false", main_result, worker.result);
    return ok ? 0 : (main_result != 0 ? main_result : worker.result);
#endif
}
static int dlopen_bundle(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__APPLE__)
    set_text(summary, ss, "Bundle dylib probe requires Darwin"); set_text(details, ds, "{\"status\":\"unverified\"}"); return 2;
#else
    if (!g_dylib_path[0]) { set_text(summary, ss, "Bundle dylib path is not configured"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Configure GraftProbeTest.dylib path from app bundle\"}"); return 2; }
    void *handle = dlopen(g_dylib_path, RTLD_NOW | RTLD_LOCAL); if (!handle) { const char *error = dlerror(); set_text(summary, ss, "dlopen failed"); set_text(details, ds, "{\"path\":\"%s\",\"error\":\"%s\"}", g_dylib_path, error ? error : "unknown"); return EFAULT; }
    int (*value)(void) = (int (*)(void))dlsym(handle, "graft_probe_test_value"); const char *error = dlerror(); int result = value ? value() : -1; dlclose(handle); if (error || result != 64) { set_text(summary, ss, "dlsym validation failed"); set_text(details, ds, "{\"path\":\"%s\",\"return_value\":%d}", g_dylib_path, result); return EFAULT; }
    set_text(summary, ss, "Loaded and called bundled dylib"); set_text(details, ds, "{\"path\":\"%s\",\"return_value\":64}", g_dylib_path); return 0;
#endif
}
static int unix_socket_probe(char *summary, size_t ss, char *details, size_t ds) { int fds[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return errno; const char msg[] = "graft64"; char out[sizeof(msg)] = {0}; ssize_t w = write(fds[0], msg, sizeof(msg)); ssize_t r = read(fds[1], out, sizeof(out)); close(fds[0]); close(fds[1]); set_text(summary, ss, "Unix socketpair round trip completed"); set_text(details, ds, "{\"written\":%zd,\"read\":%zd}", w, r); return (w == (ssize_t)sizeof(msg) && r == (ssize_t)sizeof(msg) && memcmp(msg, out, sizeof(msg)) == 0) ? 0 : EIO; }
static int shared_mapping(char *summary, size_t ss, char *details, size_t ds) {
    char path[PATH_MAX];
    const char *shared_dir = (g_cache_root[0] && access(g_cache_root, W_OK) == 0) ? g_cache_root : "/tmp";
    if (snprintf(path, sizeof(path), "%s/.graft64-shm-XXXXXX", shared_dir) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        set_text(summary, ss, "Shared mapping path is too long");
        set_text(details, ds, "{\"stage\":\"shared_path\",\"os_error\":%d}", errno);
        return errno;
    }
    int fd = mkstemp(path);
    if (fd < 0) { int e = errno; set_text(summary, ss, "Shared mapping file creation failed"); set_text(details, ds, "{\"stage\":\"mkstemp\",\"os_error\":%d}", e); return e; }
    unlink(path);
    if (ftruncate(fd, 4096) != 0) { int e = errno; close(fd); set_text(summary, ss, "Shared mapping resize failed"); set_text(details, ds, "{\"stage\":\"ftruncate\",\"os_error\":%d}", e); return e; }
    unsigned char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int map_error = errno;
    close(fd);
    if (m == MAP_FAILED) { set_text(summary, ss, "Shared mapping failed"); set_text(details, ds, "{\"stage\":\"mmap\",\"os_error\":%d}", map_error); return map_error; }
    m[0] = 0xA5;
    int ok = m[0] == 0xA5;
    munmap(m, 4096);
    set_text(summary, ss, ok ? "File-backed shared mapping completed" : "Shared mapping round trip failed");
    set_text(details, ds, "{\"bytes\":4096,\"round_trip\":%s,\"shared_path_root\":\"%s\"}", ok ? "true" : "false", shared_dir);
    return ok ? 0 : EIO;
}
static int helper_roundtrip(char *summary, size_t ss, char *details, size_t ds) {
    if (!g_helper_path[0]) { set_text(summary, ss, "Bundled helper path is not configured"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Configure helper path from app bundle\"}"); return 2; }
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
    const int socket_child_fd = 10;
    const int shared_child_fd = 11;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int action_result = posix_spawn_file_actions_adddup2(&actions, fds[1], socket_child_fd);
    action_result |= posix_spawn_file_actions_adddup2(&actions, shared_fd, shared_child_fd);
    action_result |= posix_spawn_file_actions_addclose(&actions, fds[0]);
    action_result |= posix_spawn_file_actions_addclose(&actions, fds[1]);
    if (action_result != 0) { posix_spawn_file_actions_destroy(&actions); munmap(shared, shared_size); close(shared_fd); close(fds[0]); close(fds[1]); errno = EINVAL; set_text(summary, ss, "Helper spawn file actions failed"); set_text(details, ds, "{\"stage\":\"file_actions\",\"os_error\":%d}", EINVAL); return EINVAL; }
    char socket_arg[] = "10", shared_arg[] = "11", size_arg[32];
    snprintf(size_arg, sizeof(size_arg), "%zu", shared_size);
    char *const argv[] = {g_helper_path, socket_arg, shared_arg, size_arg, NULL};
    pid_t pid = 0;
    int spawn_rc = posix_spawn(&pid, g_helper_path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawn_rc != 0) { munmap(shared, shared_size); close(shared_fd); close(fds[0]); errno = spawn_rc; set_text(summary, ss, "Helper process spawn failed"); set_text(details, ds, "{\"stage\":\"posix_spawn\",\"os_error\":%d}", spawn_rc); return spawn_rc; }
    graft_msg_header requests[] = {
        {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_HELLO, 0, 1},
        {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_PING, 0, 2},
        {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_SHARED_MEMORY, 0, 3},
        {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_PING, 0, 4},
        {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_SHUTDOWN, 0, 5},
    };
    int ok = 1;
    graft_helper_hello_payload hello = {0};
    struct timespec deadline_clock;
    clock_gettime(CLOCK_MONOTONIC, &deadline_clock);
    const uint64_t deadline_ms = (uint64_t)deadline_clock.tv_sec * 1000u + (uint64_t)deadline_clock.tv_nsec / 1000000u + 10000u;
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        if (write_full_deadline(fds[0], &requests[i], sizeof(requests[i]), deadline_ms) != 0) { ok = 0; break; }
        graft_msg_header response;
        if (read_full_deadline(fds[0], &response, sizeof(response), deadline_ms) != 0 || graft_ipc_validate_header(&response) != 0 || response.request_id != requests[i].request_id) { ok = 0; break; }
        if (response.payload_size) {
            if (response.payload_size != sizeof(hello) || read_full_deadline(fds[0], &hello, sizeof(hello), deadline_ms) != 0) { ok = 0; break; }
        }
    }
    __sync_synchronize();
    int shared_ok = shared->parent_magic == 0x4752414654504152ull && shared->helper_magic == 0x4752414654484c50ull && shared->helper_pid == (uint64_t)pid && shared->heartbeat >= 4;
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
    set_text(summary, ss, ok && shared_ok && exited_ok ? "Persistent helper IPC and shared mapping completed" : "Helper IPC/shared mapping failed");
    set_text(details, ds, "{\"spawned\":true,\"persistent_requests\":4,\"exit_code\":%d,\"wait_completed\":%s,\"pid\":%lld,\"page_size\":%llu,\"nonce_present\":%s,\"shared_round_trip\":%s,\"shared_path_root\":\"%s\",\"helper_magic\":%llu,\"shared_helper_pid\":%llu,\"heartbeat\":%llu}", wait_ok && WIFEXITED(status) ? WEXITSTATUS(status) : 128, wait_ok ? "true" : "false", (long long)hello.pid, (unsigned long long)hello.page_size, hello.nonce ? "true" : "false", shared_ok ? "true" : "false", shared_dir, (unsigned long long)helper_magic, (unsigned long long)helper_pid, (unsigned long long)heartbeat);
    return ok && shared_ok && exited_ok && hello.pid > 0 && hello.page_size > 0 && hello.nonce != 0 ? 0 : EIO;
}
static int helper_probe(char *summary, size_t ss, char *details, size_t ds) { return helper_roundtrip(summary, ss, details, ds); }
static int lifecycle_jit(char *summary, size_t ss, char *details, size_t ds) {
    if (!atomic_load_explicit(&g_lifecycle_background_seen, memory_order_acquire) ||
        !atomic_load_explicit(&g_lifecycle_foreground_seen, memory_order_acquire)) {
        set_text(summary, ss, "Waiting for background/foreground transition");
        set_text(details, ds, "{\"status\":\"manual\",\"instructions\":\"Send the app to background and return to foreground; lifecycle_jit will run automatically\"}");
        return 2;
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
    {"runtime_paths", runtime_paths}, {"page_model", page_model}, {"jit_basic", jit_basic}, {"jit_write_protect", jit_write_protect}, {"jit_multithread", jit_multithread}, {"signal_resume", signal_resume}, {"dlopen_bundle", dlopen_bundle}, {"unix_socket", unix_socket_probe}, {"shared_mapping", shared_mapping}, {"helper_spawn", helper_probe}, {"helper_ipc", helper_probe}, {"lifecycle_jit", lifecycle_jit},
};

const char *graft_probe_status_name(graft_probe_status status) { switch (status) { case GRAFT_PROBE_PASS: return "PASS"; case GRAFT_PROBE_FAIL: return "FAIL"; case GRAFT_PROBE_SKIP: return "SKIP"; case GRAFT_PROBE_BLOCKED: return "BLOCKED"; default: return "UNKNOWN"; } }

static graft_probe_reason_code reason_for_result(const char *name, graft_probe_status status, int os_error) {
    if (status == GRAFT_PROBE_PASS) return GRAFT_REASON_NONE;
    if (status == GRAFT_PROBE_SKIP) return strcmp(name, "lifecycle_jit") == 0 ? GRAFT_REASON_LIFECYCLE_WAITING : GRAFT_REASON_NOT_SUPPORTED;
    if (status == GRAFT_PROBE_BLOCKED || os_error == EACCES || os_error == EPERM)
        return strncmp(name, "jit", 3) == 0 ? GRAFT_REASON_JIT_NOT_ENABLED : GRAFT_REASON_CHILD_PROCESS;
    if (strncmp(name, "helper", 6) == 0 && (os_error == ENOENT || os_error == ESRCH)) return GRAFT_REASON_CHILD_PROCESS;
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
    char summary[256] = {0}, details[2048] = {0}; struct timespec start = {0}, end = {0}; clock_gettime(CLOCK_MONOTONIC, &start); errno = 0; int rc = entry->fn(summary, sizeof(summary), details, sizeof(details)); int saved_errno = errno; clock_gettime(CLOCK_MONOTONIC, &end);
    graft_probe_status status = rc == 0 ? GRAFT_PROBE_PASS : rc == 2 ? GRAFT_PROBE_SKIP : rc == 3 ? GRAFT_PROBE_BLOCKED : GRAFT_PROBE_FAIL;
    graft_probe_result result = {.name = entry->name, .status = status, .reason_code = reason_for_result(entry->name, status, saved_errno), .graft_error = graft_error_for_result(status, saved_errno), .os_error = saved_errno, .duration_ns = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ull + (uint64_t)(end.tv_nsec - start.tv_nsec), .summary = summary, .details_json = details}; callback(&result, context); return 0;
}
int graft_run_all_probes(graft_probe_callback callback, void *context) { if (!callback) { errno = EINVAL; return -1; } for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) if (graft_run_probe(probes[i].name, callback, context) != 0) return -1; return 0; }
