#include "graft/graft_ipc_protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

int graft_ipc_validate_header(const graft_msg_header *header) {
    if (!header || header->magic != GRAFT_IPC_MAGIC || header->version != GRAFT_IPC_VERSION ||
        header->payload_size > GRAFT_IPC_MAX_PAYLOAD) { errno = EPROTO; return -1; }
    return 0;
}

int graft_ipc_encode_header(const graft_msg_header *header, uint8_t out[sizeof(graft_msg_header)]) {
    if (graft_ipc_validate_header(header) != 0 || !out) { if (!out) errno = EINVAL; return -1; }
    memcpy(out, header, sizeof(*header));
    return 0;
}

int graft_ipc_decode_header(const uint8_t bytes[sizeof(graft_msg_header)], graft_msg_header *out_header) {
    if (!bytes || !out_header) { errno = EINVAL; return -1; }
    memcpy(out_header, bytes, sizeof(*out_header));
    return graft_ipc_validate_header(out_header);
}
