#ifndef GRAFT_PROCESS_H
#define GRAFT_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct graft_helper_result {
    int exit_code;
    int64_t pid;
    size_t page_size;
    uint64_t nonce;
} graft_helper_result;

int graft_spawn_helper(const char *path, int socket_fd, graft_helper_result *out);

#ifdef __cplusplus
}
#endif

#endif
