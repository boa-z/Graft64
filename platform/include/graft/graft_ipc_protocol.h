#ifndef GRAFT_IPC_PROTOCOL_H
#define GRAFT_IPC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRAFT_IPC_MAGIC 0x47524654u
#define GRAFT_IPC_VERSION 1u
#define GRAFT_IPC_MAX_PAYLOAD (64u * 1024u)

typedef enum graft_ipc_type {
    GRAFT_IPC_HELLO = 1,
    GRAFT_IPC_PING = 2,
    GRAFT_IPC_PONG = 3,
    GRAFT_IPC_SHUTDOWN = 4,
    GRAFT_IPC_SHARED_MEMORY = 5,
    GRAFT_IPC_ERROR = 255,
} graft_ipc_type;

typedef struct graft_msg_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;
    uint64_t request_id;
} graft_msg_header;

typedef struct graft_helper_hello_payload {
    int64_t pid;
    uint64_t page_size;
    uint64_t nonce;
} graft_helper_hello_payload;

/* Shared-memory heartbeat exchanged after the parent inherits a MAP_SHARED
 * file descriptor. The helper updates helper_magic/heartbeat without sending
 * the payload over the socket, proving both processes see the same mapping. */
typedef struct graft_helper_shared_state {
    uint64_t parent_magic;
    uint64_t helper_magic;
    uint64_t helper_pid;
    uint64_t heartbeat;
} graft_helper_shared_state;

int graft_ipc_encode_header(const graft_msg_header *header,
                            uint8_t out[sizeof(graft_msg_header)]);
int graft_ipc_decode_header(const uint8_t bytes[sizeof(graft_msg_header)],
                            graft_msg_header *out_header);
int graft_ipc_validate_header(const graft_msg_header *header);

#ifdef __cplusplus
}
#endif

#endif
