#include "graft/graft_probe.h"
#include "graft/graft_jit.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int count;
static int helper_passed;
static int missing_helper_checked;
static int page_model_checked;

static void check_page_model_details(const char *details) {
    assert(strstr(details, "\"logical_page_size\":4096") != NULL);
    assert(strstr(details, "\"region_size\":65536") != NULL);
    assert(strstr(details, "\"offsets_tested\":16") != NULL);
    assert(strstr(details, "\"same_host_page_test\":{") != NULL);
    assert(strstr(details, "\"same_host_page_logical_permissions_separable\":") != NULL);
    assert(strstr(details, "\"distinct_4k_permissions_supported\":") != NULL);
    int offset_entries = 0;
    const char *cursor = details;
    while ((cursor = strstr(cursor, "\"offset\":")) != NULL) {
        ++offset_entries;
        cursor += strlen("\"offset\":");
    }
    assert(offset_entries == 16);
    for (int offset = 0; offset < 65536; offset += 4096) {
        char expected[32];
        snprintf(expected, sizeof(expected), "\"offset\":%d", offset);
        assert(strstr(details, expected) != NULL);
    }
    page_model_checked = 1;
}

static void collect(const graft_probe_result *result, void *context) {
    (void)context;
    assert(result && result->name && result->summary && result->details_json);
    ++count;
    printf("%s %s: %s (%s)\n", graft_probe_status_name(result->status), result->name, result->summary, result->details_json);
    fflush(stdout);
    if (result->name[0] == 'h' && result->status == GRAFT_PROBE_PASS) ++helper_passed;
    if (strcmp(result->name, "page_model") == 0) {
        assert(result->status == GRAFT_PROBE_PASS);
        check_page_model_details(result->details_json);
    }
}

static void collect_missing_helper(const graft_probe_result *result, void *context) {
    (void)context;
    assert(result);
    assert(strcmp(result->name, "helper_spawn") == 0);
    assert(result->status == GRAFT_PROBE_FAIL);
    assert(result->reason_code == GRAFT_REASON_CHILD_PROCESS);
    assert(result->os_error == ENOENT);
    missing_helper_checked = 1;
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
    assert(graft_configure_helper("/definitely/missing/GraftProbeHelper") == 0);
    assert(graft_run_probe("helper_spawn", collect_missing_helper, NULL) == 0);
    assert(missing_helper_checked == 1);
    assert(graft_configure_helper(argv[1]) == 0);
    assert(graft_configure_dylib(argv[2]) == 0);
    assert(graft_run_all_probes(collect, NULL) == 0);
    assert(count == 12);
    assert(helper_passed >= 2);
    assert(page_model_checked == 1);
    printf("probe registry tests passed (%d probes, %d helper passes)\n", count, helper_passed);
    return 0;
}
