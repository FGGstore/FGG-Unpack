/* FR-1: format detection by content, never by extension. */
#ifndef UA_DETECT_H
#define UA_DETECT_H

#include "../ua_common.h"

/* A tar header carries its magic at offset 257, so anything less than a full
 * 512-byte block cannot identify a tar. */
#define UA_DETECT_BYTES 512

/* Classifies a prefix of the file. `len` may be short for small files; the
 * function only reports a format whose magic actually fits in what it was
 * given. */
ua_format ua_detect_buffer(const unsigned char *buf, size_t len);

/* Reads the first UA_DETECT_BYTES of `path` and classifies it. Returns
 * UA_ERR_UNSUPPORTED_FORMAT (with *out set to UA_FMT_UNKNOWN) rather than
 * guessing from the name. */
ua_result ua_detect_file(const char *path, ua_format *out);

#endif /* UA_DETECT_H */
