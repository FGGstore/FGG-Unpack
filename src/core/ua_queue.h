/* /data/fgg-unpack/queue.txt (requirements v2 4.2) — the manual queue, for
 * archives outside the watched paths.
 *
 * Parsing is a pure function so the line grammar can be tested without a
 * filesystem. The driver owns the file itself.
 */
#ifndef UA_QUEUE_H
#define UA_QUEUE_H

#include "../ua_common.h"

/* Parses one line of the form:
 *
 *     <archive_path> [-> <destination_path>]
 *
 * Returns UA_OK with `archive` filled and `dest` either the given destination
 * or an empty string. Blank lines and lines starting with '#' return
 * UA_ERR_NOT_FOUND, which means "nothing here", not an error. A line that has
 * an arrow with nothing after it, or a path too long to hold, returns
 * UA_ERR_BADARG so the driver can log which line was wrong.
 */
ua_result ua_queue_parse_line(const char *line,
                              char *archive, size_t archive_sz,
                              char *dest, size_t dest_sz);

#endif /* UA_QUEUE_H */
