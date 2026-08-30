#include "graft/graft_probe.h"
#include "graft/graft_jit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int count;
static int helper_passed;
static void collect(const graft_probe_result *result, void *context) {
    (void)context;
    assert(result && result->name && result->summary && result->details_json);
    ++count;
    printf("%s %s: %s (%s)\n", graft_probe_status_name(result->status), result->name, result->summary, result->details_json);
    fflush(stdout);
    if (result->name[0] == 'h' && result->status == GRAFT_PROBE_PASS) ++helper_passed;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    assert(strcmp(graft_jit_status_name(GRAFT_JIT_STATUS_UNKNOWN), "unknown") == 0);
    assert(strcmp(graft_jit_status_name(GRAFT_JIT_STATUS_ENABLED), "enabled") == 0);
    assert(strcmp(graft_jit_status_name(GRAFT_JIT_STATUS_DISABLED), "disabled") == 0);
    assert(strcmp(graft_jit_status_name(GRAFT_JIT_STATUS_UNAVAILABLE), "unavailable") == 0);
    const graft_path_context paths = {
        .guest_bundle_root = "/tmp/graft64-bundle",
        .runtime_root = "/tmp/graft64-runtime",
        .data_root = "/tmp/graft64-data",
        .cache_root = "/tmp/graft64-cache",
    };
    assert(graft_configure_path_context(&paths) == 0);
    assert((graft_jit_capabilities() & (GRAFT_JIT_CAP_ALLOCATE |
                                        GRAFT_JIT_CAP_WRITE |
                                        GRAFT_JIT_CAP_EXECUTE |
                                        GRAFT_JIT_CAP_ICACHE_INVALIDATE)) ==
           (GRAFT_JIT_CAP_ALLOCATE | GRAFT_JIT_CAP_WRITE |
            GRAFT_JIT_CAP_EXECUTE | GRAFT_JIT_CAP_ICACHE_INVALIDATE));
    assert(graft_configure_helper(argv[1]) == 0);
    assert(graft_configure_dylib(argv[2]) == 0);
    assert(graft_run_all_probes(collect, NULL) == 0);
    assert(count == 12);
    assert(helper_passed >= 2);
    printf("probe registry tests passed (%d probes, %d helper passes)\n", count, helper_passed);
    return 0;
}
