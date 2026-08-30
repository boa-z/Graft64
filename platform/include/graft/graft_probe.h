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

typedef struct graft_probe_result {
    const char *name;
    graft_probe_status status;
    int system_error;
    uint64_t duration_ns;
    const char *summary;
    const char *details_json;
} graft_probe_result;

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
int graft_lifecycle_note_background(void);
int graft_lifecycle_note_foreground(void);

#ifdef __cplusplus
}
#endif

#endif
