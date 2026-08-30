#ifndef GRAFT_LOG_H
#define GRAFT_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*graft_log_callback)(int level, const char *message, void *context);
void graft_set_log_callback(graft_log_callback callback, void *context);
void graft_log(int level, const char *message);

#ifdef __cplusplus
}
#endif

#endif
