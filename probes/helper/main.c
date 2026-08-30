#include "graft/graft_ipc_protocol.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_full(int fd, void *buffer, size_t size) { unsigned char *p = buffer; while (size) { ssize_t n = read(fd, p, size); if (n <= 0) return -1; p += n; size -= (size_t)n; } return 0; }
static int write_full(int fd, const void *buffer, size_t size) { const unsigned char *p = buffer; while (size) { ssize_t n = write(fd, p, size); if (n <= 0) return -1; p += n; size -= (size_t)n; } return 0; }

int main(int argc, char **argv) {
    if (argc < 2) return 64;
    int fd = atoi(argv[1]);
    for (;;) {
        graft_msg_header header;
        if (read_full(fd, &header, sizeof(header)) != 0 || graft_ipc_validate_header(&header) != 0) return 65;
        unsigned char payload[GRAFT_IPC_MAX_PAYLOAD];
        if (header.payload_size && read_full(fd, payload, header.payload_size) != 0) return 66;
        graft_msg_header response = {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_PONG, 0, header.request_id};
        if (header.type == GRAFT_IPC_HELLO) response.type = GRAFT_IPC_HELLO;
        if (header.type == GRAFT_IPC_SHUTDOWN) response.type = GRAFT_IPC_SHUTDOWN;
        if (write_full(fd, &response, sizeof(response)) != 0) return 67;
        if (header.type == GRAFT_IPC_SHUTDOWN) break;
    }
    close(fd); return 0;
}
