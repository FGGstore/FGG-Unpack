/* Structured logging to /data/fgg-unpack/debug.log (requirements v2 4.5).
 *
 * Bounded by construction: the log rotates at a byte cap so a runaway job
 * cannot fill /data, which on this console would be a far worse failure than
 * losing log history.
 */
#ifndef UA_LOG_H
#define UA_LOG_H

#include "../ua_common.h"

typedef enum {
    UA_LOG_ERROR = 0,
    UA_LOG_WARN,
    UA_LOG_INFO,
    UA_LOG_DEBUG
} ua_log_level;

/* `max_bytes` is the size at which the current file is rotated to
 * "<path>.1" (replacing any previous .1), so disk use is bounded at roughly
 * twice the cap. */
ua_result ua_log_open(const char *path, ua_log_level level, uint64_t max_bytes);
void      ua_log_close(void);

/* Mirrors output to stderr as well as the file. The host harness turns this on
 * so test failures are readable without opening the log. */
void ua_log_set_echo(int enabled);

void ua_log_write(ua_log_level level, const char *fmt, ...);

#define UA_LOG_E(...) ua_log_write(UA_LOG_ERROR, __VA_ARGS__)
#define UA_LOG_W(...) ua_log_write(UA_LOG_WARN,  __VA_ARGS__)
#define UA_LOG_I(...) ua_log_write(UA_LOG_INFO,  __VA_ARGS__)
#define UA_LOG_D(...) ua_log_write(UA_LOG_DEBUG, __VA_ARGS__)

#endif /* UA_LOG_H */
