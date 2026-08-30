#include "graft/graft_ipc_protocol.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int read_full(int fd, void *buffer, size_t size) { unsigned char *p = buffer; while (size) { ssize_t n = read(fd, p, size); if (n <= 0) return -1; p += n; size -= (size_t)n; } return 0; }
static int write_full(int fd, const void *buffer, size_t size) { const unsigned char *p = buffer; while (size) { ssize_t n = write(fd, p, size); if (n <= 0) return -1; p += n; size -= (size_t)n; } return 0; }

int main(int argc, char **argv) {
    if (argc < 2) return 64;
    char *end = NULL;
    long parsed_fd = strtol(argv[1], &end, 10);
    if (!end || *end != '\0' || parsed_fd < 0 || parsed_fd > 1024) return 64;
    int fd = (int)parsed_fd;
    int shared_fd = -1;
    size_t shared_size = 0;
    graft_helper_shared_state *shared = NULL;
    if (argc >= 4) {
        char *shared_end = NULL;
        long parsed_shared_fd = strtol(argv[2], &shared_end, 10);
        if (!shared_end || *shared_end != '\0' || parsed_shared_fd < 0 || parsed_shared_fd > 1024) return 64;
        shared_fd = (int)parsed_shared_fd;
        char *size_end = NULL;
        unsigned long long parsed_size = strtoull(argv[3], &size_end, 10);
        if (!size_end || *size_end != '\0' || parsed_size < sizeof(*shared)) return 64;
        shared_size = (size_t)parsed_size;
        shared = mmap(NULL, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, shared_fd, 0);
        if (shared == MAP_FAILED) return 64;
    }
    const uint64_t helper_magic = 0x4752414654484c50ull;
    uint64_t session_nonce = 0;
    arc4random_buf(&session_nonce, sizeof(session_nonce));
    if (session_nonce == 0) session_nonce = 1;
    for (;;) {
        graft_msg_header header;
        if (read_full(fd, &header, sizeof(header)) != 0 || graft_ipc_validate_header(&header) != 0) return 65;
        unsigned char payload[GRAFT_IPC_MAX_PAYLOAD];
        if (header.payload_size && read_full(fd, payload, header.payload_size) != 0) return 66;
        graft_helper_hello_payload hello = {(int64_t)getpid(), (uint64_t)getpagesize(), session_nonce};
        if (shared) {
            shared->helper_magic = helper_magic;
            shared->helper_pid = (uint64_t)getpid();
            shared->heartbeat++;
            __sync_synchronize();
        }
        uint32_t response_size = header.type == GRAFT_IPC_HELLO ? (uint32_t)sizeof(hello) : 0;
        graft_msg_header response = {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_PONG, response_size, header.request_id};
        if (header.type == GRAFT_IPC_HELLO) response.type = GRAFT_IPC_HELLO;
        if (header.type == GRAFT_IPC_SHUTDOWN) response.type = GRAFT_IPC_SHUTDOWN;
        if (write_full(fd, &response, sizeof(response)) != 0) return 67;
        if (response_size && write_full(fd, &hello, sizeof(hello)) != 0) return 68;
        if (header.type == GRAFT_IPC_SHUTDOWN) break;
    }
    if (shared) munmap(shared, shared_size);
    if (shared_fd >= 0) close(shared_fd);
    close(fd); return 0;
}
