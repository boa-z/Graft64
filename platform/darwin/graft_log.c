#include "graft/graft_log.h"
#include <stdio.h>

static graft_log_callback g_callback;
static void *g_context;

void graft_set_log_callback(graft_log_callback callback, void *context) {
    g_callback = callback;
    g_context = context;
}

void graft_log(int level, const char *message) {
    if (g_callback) {
        g_callback(level, message, g_context);
    } else if (message) {
        fprintf(stderr, "[graft:%d] %s\n", level, message);
    }
}
