#include "graft/graft_process.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int graft_spawn_helper(const graft_helper_launch_config *config,
                       graft_helper_process *out_process) {
    if (!config || !config->path || !config->path[0] || !out_process ||
        config->parent_socket_fd < 0 || config->socket_fd < 0 ||
        config->shared_fd < 0 ||
        config->shared_size == 0) {
        errno = EINVAL;
        return -1;
    }
    int highest_fd = config->parent_socket_fd;
    if (config->socket_fd > highest_fd) highest_fd = config->socket_fd;
    if (config->shared_fd > highest_fd) highest_fd = config->shared_fd;
    if (highest_fd > 1022) { errno = EMFILE; return -1; }
    const int child_socket_fd = highest_fd + 1;
    const int child_shared_fd = highest_fd + 2;
    char socket_string[16];
    char shared_string[16];
    char size_string[32];
    snprintf(socket_string, sizeof(socket_string), "%d", child_socket_fd);
    snprintf(shared_string, sizeof(shared_string), "%d", child_shared_fd);
    snprintf(size_string, sizeof(size_string), "%zu", config->shared_size);
    char *const argv[] = {
        (char *)config->path, socket_string, shared_string, size_string, NULL
    };
    posix_spawn_file_actions_t actions;
    int action_result = posix_spawn_file_actions_init(&actions);
    if (action_result != 0) { errno = action_result; return -1; }
    action_result = posix_spawn_file_actions_adddup2(
        &actions, config->socket_fd, child_socket_fd);
    if (action_result == 0)
        action_result = posix_spawn_file_actions_adddup2(
            &actions, config->shared_fd, child_shared_fd);
    if (action_result == 0)
        action_result = posix_spawn_file_actions_addclose(
            &actions, config->parent_socket_fd);
    if (action_result == 0)
        action_result = posix_spawn_file_actions_addclose(
            &actions, config->socket_fd);
    if (action_result == 0)
        action_result = posix_spawn_file_actions_addclose(
            &actions, config->shared_fd);
    if (action_result != 0) {
        posix_spawn_file_actions_destroy(&actions);
        errno = action_result;
        return -1;
    }
    pid_t pid = 0;
    int rc = posix_spawn(&pid, config->path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) { errno = rc; return -1; }
    out_process->pid = pid;
    return 0;
}
