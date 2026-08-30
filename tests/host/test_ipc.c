#include "graft/graft_ipc_protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void) {
    graft_msg_header input = {GRAFT_IPC_MAGIC, GRAFT_IPC_VERSION, GRAFT_IPC_PING, 7, 42};
    unsigned char bytes[sizeof(input)];
    assert(graft_ipc_encode_header(&input, bytes) == 0);
    graft_msg_header output = {0};
    assert(graft_ipc_decode_header(bytes, &output) == 0);
    assert(output.magic == input.magic && output.version == input.version && output.type == input.type && output.payload_size == 7 && output.request_id == 42);
    output.payload_size = GRAFT_IPC_MAX_PAYLOAD + 1;
    assert(graft_ipc_validate_header(&output) != 0 && errno == EPROTO);
    puts("IPC header tests passed");
    return 0;
}
