#include "ua_queue.h"

#include <string.h>

/* Copies src[0..len) into dst, trimming surrounding whitespace. */
static ua_result copy_trimmed(const char *src, size_t len,
                              char *dst, size_t dst_sz)
{
    size_t start = 0;

    while (start < len && (src[start] == ' ' || src[start] == '\t'))
        start++;
    while (len > start && (src[len - 1] == ' ' || src[len - 1] == '\t'))
        len--;

    if (len == start) return UA_ERR_BADARG;
    if (len - start >= dst_sz) return UA_ERR_LIMIT;

    memcpy(dst, src + start, len - start);
    dst[len - start] = '\0';

    return UA_OK;
}

ua_result ua_queue_parse_line(const char *line,
                              char *archive, size_t archive_sz,
                              char *dest, size_t dest_sz)
{
    const char *arrow;
    size_t len;

    if (!line || !archive || !dest || !archive_sz || !dest_sz)
        return UA_ERR_BADARG;

    archive[0] = '\0';
    dest[0] = '\0';

    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;

    /* Leading whitespace before a comment marker still makes it a comment. */
    {
        size_t i = 0;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i == len || line[i] == '#') return UA_ERR_NOT_FOUND;
    }

    /* Only the first arrow separates; a later "->" belongs to the destination,
     * which is legal in a filename. */
    arrow = strstr(line, "->");
    if (arrow && (size_t)(arrow - line) < len) {
        ua_result r;

        r = copy_trimmed(line, (size_t)(arrow - line), archive, archive_sz);
        if (r != UA_OK) return r;

        return copy_trimmed(arrow + 2, len - (size_t)(arrow - line) - 2,
                            dest, dest_sz);
    }

    return copy_trimmed(line, len, archive, archive_sz);
}
