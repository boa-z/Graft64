#ifndef GRAFT_PROBE_H
#define GRAFT_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum graft_probe_status {
    GRAFT_PROBE_PASS = 0,
    GRAFT_PROBE_FAIL = 1,
    GRAFT_PROBE_SKIP = 2,
    GRAFT_PROBE_BLOCKED = 3,
} graft_probe_status;

typedef enum graft_probe_reason_code {
    GRAFT_REASON_NONE = 0,
    GRAFT_REASON_INVALID_ARGUMENT = 1,
    GRAFT_REASON_NOT_SUPPORTED = 2,
    GRAFT_REASON_JIT_NOT_ENABLED = 3,
    GRAFT_REASON_IO = 4,
    GRAFT_REASON_LIFECYCLE_WAITING = 5,
    GRAFT_REASON_PATH_CONTEXT_MISSING = 6,
    GRAFT_REASON_CHILD_PROCESS = 7,
    GRAFT_REASON_INTERNAL = 255,
} graft_probe_reason_code;

typedef enum graft_error_code {
    GRAFT_ERROR_NONE = 0,
    GRAFT_ERROR_ARGUMENT = 1,
    GRAFT_ERROR_UNSUPPORTED = 2,
    GRAFT_ERROR_PERMISSION = 3,
    GRAFT_ERROR_IO = 4,
    GRAFT_ERROR_INTERNAL = 5,
} graft_error_code;

typedef struct graft_probe_result {
    const char *name;
    graft_probe_status status;
    graft_probe_reason_code reason_code;
    graft_error_code graft_error;
    int os_error;
    uint64_t duration_ns;
    const char *summary;
    const char *details_json;
} graft_probe_result;

typedef struct graft_path_context {
    const char *guest_bundle_root;
    const char *runtime_root;
    const char *data_root;
    const char *cache_root;
} graft_path_context;

typedef void (*graft_probe_callback)(const graft_probe_result *result,
                                     void *context);

int graft_run_probe(const char *name,
                    graft_probe_callback callback,
                    void *context);
int graft_run_all_probes(graft_probe_callback callback,
                         void *context);
const char *graft_probe_status_name(graft_probe_status status);
int graft_configure_helper(const char *path);
int graft_configure_dylib(const char *path);
int graft_configure_path_context(const graft_path_context *context);
int graft_lifecycle_note_background(void);
int graft_lifecycle_note_foreground(void);

#ifdef __cplusplus
}
#endif

#endif
