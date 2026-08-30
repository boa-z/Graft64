#include "graft/graft_process.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int graft_spawn_helper(const char *path, int socket_fd, graft_helper_result *out) {
    if (!path || !out || socket_fd < 0) { errno = EINVAL; return -1; }
    const int child_fd = 10;
    char fd_string[] = "10";
    char *const argv[] = { (char *)path, fd_string, NULL };
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) { errno = EINVAL; return -1; }
    int action_result = posix_spawn_file_actions_adddup2(&actions, socket_fd, child_fd);
    if (socket_fd != child_fd) action_result |= posix_spawn_file_actions_addclose(&actions, socket_fd);
    if (action_result != 0) { posix_spawn_file_actions_destroy(&actions); errno = EINVAL; return -1; }
    pid_t pid = 0;
    int rc = posix_spawn(&pid, path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) { errno = rc; return -1; }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { return -1; }
    out->pid = pid;
    out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    out->page_size = (size_t)getpagesize();
    out->nonce = 0;
    return 0;
}
