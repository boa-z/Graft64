#include "graft/graft_process.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int graft_spawn_helper(const char *path, int socket_fd, graft_helper_result *out) {
    if (!path || !out || socket_fd < 0) { errno = EINVAL; return -1; }
    char fd_string[32];
    int n = snprintf(fd_string, sizeof(fd_string), "%d", socket_fd);
    if (n < 0 || (size_t)n >= sizeof(fd_string)) { errno = EOVERFLOW; return -1; }
    char *const argv[] = { (char *)path, NULL };
    char *const envp[] = { fd_string, NULL };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, path, NULL, NULL, argv, envp);
    if (rc != 0) { errno = rc; return -1; }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { return -1; }
    out->pid = pid;
    out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    out->page_size = (size_t)getpagesize();
    out->nonce = 0;
    return 0;
}
