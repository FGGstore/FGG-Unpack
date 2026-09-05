/* /data/fgg-unpack/queue.status (requirements v2 4.3).
 *
 * One record per processed archive, so a job is not redone on every poll or
 * after a restart. Keyed by path + size + mtime: replacing a file with a new
 * version of the same name is a different key, so it correctly runs again.
 *
 * Bounded by construction (v2 section 7): the record set is a fixed-size ring,
 * and the file is rewritten from it, so neither can grow without limit.
 */
#ifndef UA_STATUS_H
#define UA_STATUS_H

#include "../ua_common.h"

/* Roughly 600 KB of file at worst. Old records ageing out only means an
 * archive that is still sitting in a watch path could be reprocessed after
 * hundreds of newer ones have gone through, which is the right failure. */
#define UA_STATUS_MAX 512

typedef struct {
    char      path[UA_MAX_PATH];
    uint64_t  size;
    int64_t   mtime;
    int64_t   processed_at;
    ua_result result;
} ua_status_rec;

typedef struct ua_status ua_status;

/* Reads `path` if it exists. A missing file is a normal first run; a corrupt
 * line is skipped rather than failing the load, because refusing to start over
 * a bad status file would strand the payload. */
ua_result ua_status_open(const char *path, ua_status **out);
void      ua_status_close(ua_status *s);

/* True when this exact (path, size, mtime) has already been processed. */
int ua_status_seen(const ua_status *s, const char *path,
                   uint64_t size, int64_t mtime);

/* Looks up the recorded outcome. Returns 0 when there is no record. */
int ua_status_result_of(const ua_status *s, const char *path,
                        uint64_t size, int64_t mtime, ua_result *out);

/* Adds or replaces a record and rewrites the file atomically. */
ua_result ua_status_record(ua_status *s, const char *path, uint64_t size,
                           int64_t mtime, ua_result result);

/* Drops the record for a path, used after the source archive is deleted so a
 * re-uploaded file is not matched against a stale entry. */
void ua_status_forget(ua_status *s, const char *path);

size_t ua_status_count(const ua_status *s);

#endif /* UA_STATUS_H */
