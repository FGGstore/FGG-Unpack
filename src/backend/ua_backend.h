/* Archive backend interface.
 *
 * The requirements name libarchive as the intended backend and miniz as the
 * fallback (Section 3, open question 3). That decision is still open, so the
 * extractor talks only to this vtable: swapping miniz for libarchive later is
 * a new ua_backend_vtable and one line in ua_backend_open(), with no change to
 * the preflight, path-safety or job-tracking code.
 *
 * The interface is a sequential iterator rather than indexed random access,
 * because a tar.gz stream cannot be seeked cheaply. Preflight (FR-2) walks it
 * once to count entries and total size, calls reset(), then walks it again to
 * extract.
 */
#ifndef UA_BACKEND_H
#define UA_BACKEND_H

#include "../ua_common.h"

/* Receives one chunk of the current entry. Returning anything but UA_OK aborts
 * the entry; the error is propagated to the caller of extract_current. */
typedef ua_result (*ua_sink_fn)(void *ud, const void *buf, size_t len);

/* Progress hook for the parallel path, which has no sink to count bytes
 * through. Called from the coordinating thread, not from workers. */
typedef void (*ua_backend_bytes_fn)(void *ud, uint64_t bytes_done);

typedef struct {
    unsigned threads;            /* already resolved; never 0 */
    unsigned memory_budget_mb;   /* ceiling on threads x dictionary */
    ua_backend_bytes_fn on_bytes;
    void    *ud;

    /* One descriptor, already open, shared by every worker.
     *
     * Workers used to open their own handle each. That works on Windows and
     * fails on the console, and it was the wrong shape regardless: positioned
     * writes need no private file offset, so one descriptor is enough and
     * there is nothing to coordinate. */
    ua_file *out;
} ua_parallel_opts;

typedef struct ua_backend ua_backend;

typedef struct {
    const char *name;

    ua_result (*open)(ua_backend *be, const char *path);
    ua_result (*reset)(ua_backend *be);

    /* UA_ERR_NOT_FOUND signals a clean end of archive, not a failure. */
    ua_result (*next)(ua_backend *be, ua_entry *out);

    /* Streams the entry last returned by next(). Never buffers the whole
     * entry: chunk size is the backend's business, bounded by FR-4. */
    ua_result (*extract_current)(ua_backend *be, ua_sink_fn sink, void *ud);

    void (*close)(ua_backend *be);

    /* Optional fast path: write the current entry straight to `path` using
     * `threads` workers, bypassing the sequential sink.
     *
     * A sink is inherently single-threaded — it is a stream — so a backend
     * that can decode an entry out of order cannot express that through it.
     * This exists for exactly that case.
     *
     * Return UA_ERR_NOT_FOUND to decline (wrong shape of archive, one thread
     * requested, no independent restart points); the caller then falls back to
     * extract_current and nothing is lost. NULL if the backend has no such
     * path at all. The file at `path` must already exist.
     */
    ua_result (*extract_current_parallel)(ua_backend *be, const char *path,
                                          const ua_parallel_opts *po,
                                          uint64_t *written);
} ua_backend_vtable;

struct ua_backend {
    const ua_backend_vtable *v;
    void *impl;
    char  detail[160];   /* backend-specific reason, copied into the log */

    /* Preferred I/O buffer size, from config chunk_size_kb. Set before open()
     * and treated as advice: a backend that manages its own buffering (miniz
     * does) is free to ignore it. */
    size_t chunk_bytes;
};

/* Selects and opens a backend for an already-detected format. Returns
 * UA_ERR_UNSUPPORTED_FORMAT for formats this build cannot handle, so the
 * caller can log a specific reason instead of failing mid-extraction (FR-1). */
ua_result ua_backend_open(ua_format fmt, const char *path,
                          size_t chunk_bytes, ua_backend *be);
void      ua_backend_close(ua_backend *be);

extern const ua_backend_vtable ua_backend_zip;
extern const ua_backend_vtable ua_backend_7z;

#endif /* UA_BACKEND_H */
