#ifndef GRAFT_PROCESS_H
#define GRAFT_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct graft_helper_launch_config {
    const char *path;
    int parent_socket_fd;
    int socket_fd;
    int shared_fd;
    size_t shared_size;
} graft_helper_launch_config;

typedef struct graft_helper_process {
    int64_t pid;
} graft_helper_process;

/* Host adapter for launching the bundled helper. The caller owns IPC and
 * process reaping so it can enforce one deadline across the full exchange. */
int graft_spawn_helper(const graft_helper_launch_config *config,
                       graft_helper_process *out_process);

#ifdef __cplusplus
}
#endif

#endif
