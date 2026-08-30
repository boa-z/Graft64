#include "graft/graft_probe.h"
#include "graft/graft_jit.h"
#include "graft/graft_ipc_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <dlfcn.h>
#endif

typedef int (*probe_fn)(char *summary, size_t summary_size, char *details, size_t details_size);
typedef struct probe_entry { const char *name; probe_fn fn; } probe_entry;

static void set_text(char *dst, size_t size, const char *fmt, ...) {
    va_list args; va_start(args, fmt); vsnprintf(dst, size, fmt, args); va_end(args);
}

static int runtime_paths(char *summary, size_t ss, char *details, size_t ds) {
    char cwd[PATH_MAX] = {0}; if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), "<error:%d>", errno);
    char executable[PATH_MAX] = {0}; uint32_t length = (uint32_t)sizeof(executable);
#if defined(__APPLE__)
    if (_NSGetExecutablePath(executable, &length) != 0) snprintf(executable, sizeof(executable), "<too-long>");
#else
    ssize_t read_len = readlink("/proc/self/exe", executable, sizeof(executable) - 1); if (read_len > 0) executable[read_len] = 0;
#endif
    const char *home = getenv("HOME"); if (!home) home = "<unset>";
    set_text(summary, ss, "Resolved executable and sandbox paths");
    set_text(details, ds, "{\"executable\":\"%s\",\"argv0\":\"%s\",\"cwd\":\"%s\",\"home\":\"%s\",\"documents\":\"%s/Documents\",\"library\":\"%s/Library\",\"tmp\":\"%s/tmp\",\"livecontainer_detected\":%s}", executable, executable, cwd, home, home, home, home, strstr(executable, "LiveContainer") ? "true" : "false");
    return 0;
}

static int page_model(char *summary, size_t ss, char *details, size_t ds) {
    long page = getpagesize(); long sys_page = sysconf(_SC_PAGESIZE);
    size_t len = (size_t)page * 2; void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) { set_text(summary, ss, "mmap failed"); set_text(details, ds, "{\"errno\":%d}", errno); return errno; }
    int first = mprotect((char *)map + page, (size_t)page, PROT_READ);
    int second = mprotect((char *)map + page / 2, (size_t)page, PROT_READ | PROT_EXEC);
    munmap(map, len);
    set_text(summary, ss, "Host page model measured");
    set_text(details, ds, "{\"getpagesize\":%ld,\"sysconf_pagesize\":%ld,\"mprotect_aligned\":%d,\"mprotect_unaligned\":%d}", page, sys_page, first, second);
    return 0;
}

static int jit_call_42(void *base) {
#if defined(__aarch64__) || defined(__arm64__)
    return ((int (*)(void))base)();
#else
    (void)base; return -1;
#endif
}

static int jit_basic(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return 2;
#else
    graft_jit_region region = {0}; if (graft_jit_alloc(4096, &region) != 0) { set_text(summary, ss, "MAP_JIT allocation failed"); set_text(details, ds, "{\"errno\":%d}", errno); return errno; }
    uint32_t code[] = { 0x52800540u, 0xD65F03C0u }; memcpy(region.base, code, sizeof(code));
    int result = graft_jit_invalidate_icache(&region, 0, sizeof(code)); int protect = graft_jit_end_write(&region); int value = protect == 0 ? jit_call_42(region.base) : -1;
    graft_jit_free(&region); if (result || protect || value != 42) { set_text(summary, ss, "JIT function did not return 42"); set_text(details, ds, "{\"invalidate\":%d,\"protect\":%d,\"value\":%d,\"errno\":%d}", result, protect, value, errno); return EACCES; }
    set_text(summary, ss, "Executed ARM64 JIT function"); set_text(details, ds, "{\"return_value\":42,\"backend\":\"MAP_JIT\"}"); return 0;
#endif
}

static int jit_write_protect(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return 2;
#else
    graft_jit_region region = {0}; if (graft_jit_alloc(4096, &region) != 0) return errno;
    uint32_t code[] = { 0x52800020u, 0xD65F03C0u }; memcpy(region.base, code, sizeof(code)); graft_jit_invalidate_icache(&region, 0, sizeof(code));
    int rx1 = graft_jit_end_write(&region); int first = rx1 == 0 ? jit_call_42(region.base) : -1;
    int rw = graft_jit_begin_write(&region); code[0] = 0x52800140u; memcpy(region.base, code, sizeof(code)); graft_jit_invalidate_icache(&region, 0, sizeof(code)); int rx2 = graft_jit_end_write(&region); int second = rx2 == 0 ? jit_call_42(region.base) : -1;
    graft_jit_free(&region); set_text(summary, ss, "JIT write/execute transitions completed"); set_text(details, ds, "{\"first\":%d,\"second\":%d,\"begin_write\":%d}", first, second, rw); return (first == 1 && second == 10 && rw == 0) ? 0 : EACCES;
#endif
}

typedef struct thread_arg { graft_jit_region *region; int failures; } thread_arg;
static void *jit_thread(void *opaque) { thread_arg *arg = (thread_arg *)opaque; for (int i = 0; i < 1000; ++i) { if (jit_call_42(arg->region->base) != 42) arg->failures++; } return NULL; }
static int jit_multithread(char *summary, size_t ss, char *details, size_t ds) {
#if !defined(__aarch64__) && !defined(__arm64__)
    set_text(summary, ss, "Requires an arm64 device"); set_text(details, ds, "{\"reason\":\"host architecture is not arm64\"}"); return 2;
#else
    graft_jit_region region = {0}; if (jit_basic(summary, ss, details, ds) != 0 || graft_jit_alloc(4096, &region) != 0) return 1;
    uint32_t code[] = { 0x52800540u, 0xD65F03C0u }; memcpy(region.base, code, sizeof(code)); graft_jit_invalidate_icache(&region, 0, sizeof(code)); if (graft_jit_end_write(&region) != 0) { graft_jit_free(&region); return EACCES; }
    pthread_t threads[4]; thread_arg args[4] = {{0}}; for (int i = 0; i < 4; ++i) { args[i].region = &region; pthread_create(&threads[i], NULL, jit_thread, &args[i]); }
    int failures = 0; for (int i = 0; i < 4; ++i) { pthread_join(threads[i], NULL); failures += args[i].failures; } graft_jit_free(&region);
    set_text(summary, ss, "Four-thread JIT execution completed"); set_text(details, ds, "{\"threads\":4,\"iterations_per_thread\":1000,\"failures\":%d}", failures); return failures ? EIO : 0;
#endif
}

static int signal_resume(char *summary, size_t ss, char *details, size_t ds) { set_text(summary, ss, "Requires controlled device fault validation"); set_text(details, ds, "{\"status\":\"manual\",\"next_step\":\"Run on arm64 device and capture ucontext\"}"); return 2; }
static int dlopen_bundle(char *summary, size_t ss, char *details, size_t ds) { set_text(summary, ss, "Bundle dylib path not configured"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Package GraftProbeTest.dylib\"}"); return 2; }
static int unix_socket_probe(char *summary, size_t ss, char *details, size_t ds) { int fds[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return errno; const char msg[] = "graft64"; char out[sizeof(msg)] = {0}; ssize_t w = write(fds[0], msg, sizeof(msg)); ssize_t r = read(fds[1], out, sizeof(out)); close(fds[0]); close(fds[1]); set_text(summary, ss, "Unix socketpair round trip completed"); set_text(details, ds, "{\"written\":%zd,\"read\":%zd}", w, r); return (w == (ssize_t)sizeof(msg) && r == (ssize_t)sizeof(msg) && memcmp(msg, out, sizeof(msg)) == 0) ? 0 : EIO; }
static int shared_mapping(char *summary, size_t ss, char *details, size_t ds) { char path[] = "/tmp/graft64-shm-XXXXXX"; int fd = mkstemp(path); if (fd < 0) return errno; unlink(path); if (ftruncate(fd, 4096) != 0) { int e = errno; close(fd); return e; } unsigned char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd); if (m == MAP_FAILED) return errno; m[0] = 0xA5; int ok = m[0] == 0xA5; munmap(m, 4096); set_text(summary, ss, "File-backed shared mapping completed"); set_text(details, ds, "{\"bytes\":4096,\"round_trip\":%s}", ok ? "true" : "false"); return ok ? 0 : EIO; }
static int helper_probe(char *summary, size_t ss, char *details, size_t ds) { set_text(summary, ss, "Bundled helper is not available in host build"); set_text(details, ds, "{\"status\":\"unverified\",\"next_step\":\"Build GraftProbeHelper target\"}"); return 2; }
static int lifecycle_jit(char *summary, size_t ss, char *details, size_t ds) { set_text(summary, ss, "Lifecycle requires manual background/foreground action"); set_text(details, ds, "{\"status\":\"manual\",\"instructions\":\"Run jit_basic, background app, return, run again\"}"); return 2; }

static const probe_entry probes[] = {
    {"runtime_paths", runtime_paths}, {"page_model", page_model}, {"jit_basic", jit_basic}, {"jit_write_protect", jit_write_protect}, {"jit_multithread", jit_multithread}, {"signal_resume", signal_resume}, {"dlopen_bundle", dlopen_bundle}, {"unix_socket", unix_socket_probe}, {"shared_mapping", shared_mapping}, {"helper_spawn", helper_probe}, {"helper_ipc", helper_probe}, {"lifecycle_jit", lifecycle_jit},
};

const char *graft_probe_status_name(graft_probe_status status) { switch (status) { case GRAFT_PROBE_PASS: return "PASS"; case GRAFT_PROBE_FAIL: return "FAIL"; case GRAFT_PROBE_SKIP: return "SKIP"; case GRAFT_PROBE_BLOCKED: return "BLOCKED"; default: return "UNKNOWN"; } }

static const probe_entry *find_probe(const char *name) { for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) if (strcmp(name, probes[i].name) == 0) return &probes[i]; return NULL; }
int graft_run_probe(const char *name, graft_probe_callback callback, void *context) {
    if (!name || !callback) { errno = EINVAL; return -1; } const probe_entry *entry = find_probe(name); if (!entry) { errno = ENOENT; return -1; }
    char summary[256] = {0}, details[2048] = {0}; struct timespec start = {0}, end = {0}; clock_gettime(CLOCK_MONOTONIC, &start); errno = 0; int rc = entry->fn(summary, sizeof(summary), details, sizeof(details)); int saved_errno = errno; clock_gettime(CLOCK_MONOTONIC, &end);
    graft_probe_result result = {.name = entry->name, .status = rc == 0 ? GRAFT_PROBE_PASS : rc == 2 ? GRAFT_PROBE_SKIP : GRAFT_PROBE_FAIL, .system_error = saved_errno, .duration_ns = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ull + (uint64_t)(end.tv_nsec - start.tv_nsec), .summary = summary, .details_json = details}; callback(&result, context); return 0;
}
int graft_run_all_probes(graft_probe_callback callback, void *context) { if (!callback) { errno = EINVAL; return -1; } for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) if (graft_run_probe(probes[i].name, callback, context) != 0) return -1; return 0; }
