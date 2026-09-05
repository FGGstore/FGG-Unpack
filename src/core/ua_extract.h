/* Job execution: preflight (FR-2), streaming extraction (FR-4), structure
 * preservation (FR-5) and failure reporting (FR-7).
 *
 * One job at a time (FR-8) is a property of the caller, not of this module;
 * nothing here holds global state, so the host harness can run cases without
 * the queue driver.
 */
#ifndef UA_EXTRACT_H
#define UA_EXTRACT_H

#include "../ua_common.h"

typedef struct {
    int      overwrite;                 /* config: overwrite */
    uint64_t min_free_margin_mb;        /* config: min_free_margin_mb */
    uint32_t chunk_size_kb;             /* config: chunk_size_kb */
    uint32_t stability_wait_seconds;    /* config: stability_wait_seconds */

    /* Not in the requirements draft; see docs/spec-gaps.md.
     *
     * allow_symlinks defaults off. FR-3 only asks that symlinks pointing
     * outside the destination be rejected, but a symlink pointing *inside* it
     * is still a way to redirect later entries, and the console has no use
     * case that needs them. Turning them on re-enables the FR-3 check rather
     * than bypassing it. */
    int      allow_symlinks;

    /* Refuses an archive whose declared uncompressed size exceeds this
     * multiple of its own file size. Zero disables the check. */
    uint32_t max_expansion_ratio;

    /* Below this size no ratio check is applied; small archives legitimately
     * compress enormously and are harmless. */
    uint64_t expansion_floor_bytes;

    uint64_t max_entries;

    /* Worker threads for backends that can decode one entry out of order.
     * 0 means auto (CPU count, capped), 1 disables it.
     *
     * Only 7z benefits: an LZMA2 stream has periodic dictionary resets that
     * can each be decoded from cold. Each worker needs its own dictionary, so
     * memory is threads x dictionary size -- still flat with respect to
     * archive size, which is what FR-4 actually requires. */
    unsigned decode_threads;

    /* Upper bound on threads x dictionary. Threads are reduced until the
     * decoder fits, so a 256 MB window does not silently become 2 GB. */
    unsigned decode_memory_budget_mb;

    /* Emit a progress callback each time this many percent completes.
     * Requirements v2 4.6: rate-limited by percent, never per file. */
    uint32_t progress_percent_step;
} ua_extract_opts;

void ua_extract_opts_defaults(ua_extract_opts *o);

typedef struct {
    ua_format format;

    uint64_t entries_total;
    uint64_t entries_done;
    uint64_t bytes_total;        /* declared uncompressed total */
    uint64_t bytes_done;

    /* Regular-file entries the preflight scan counted, as opposed to
     * directories and symlinks. FR-6 compares this against files_written
     * before it will delete a source archive. */
    uint64_t files_expected;

    uint64_t files_written;
    uint64_t dirs_created;
    uint64_t symlinks_skipped;

    uint64_t archive_size;
    uint64_t free_before;
    uint64_t required;

    unsigned percent;

    /* Timing, so progress can say how fast and how much longer rather than
     * just a percentage. Filled in from the first progress report onward. */
    int64_t  started_at;        /* unix seconds */
    unsigned elapsed_seconds;
    uint64_t bytes_per_sec;
    unsigned eta_seconds;       /* 0 when not yet estimable */

    ua_result result;
    char      failed_entry[UA_MAX_PATH];
    char      detail[192];       /* specific reason, already log-formatted */
} ua_job_report;

typedef void (*ua_progress_fn)(void *ud, const ua_job_report *report);

/* Runs preflight then extraction. `dest` is created if missing. On any failure
 * the report carries the failing entry and a specific reason; nothing is
 * written at all when preflight fails. */
ua_result ua_extract_run(const char *archive_path,
                         const char *dest_dir,
                         const ua_extract_opts *opts,
                         ua_progress_fn progress,
                         void *progress_ud,
                         ua_job_report *report);

/* Preflight only: validates the archive and every entry path, and fills in the
 * size and free-space fields, without creating anything. Exposed separately so
 * the test corpus can assert that a malicious archive is refused before any
 * file is touched. */
ua_result ua_extract_preflight(const char *archive_path,
                               const char *dest_dir,
                               const ua_extract_opts *opts,
                               ua_job_report *report);

#endif /* UA_EXTRACT_H */
