/* The watch driver (requirements v2 4.1-4.3, M3).
 *
 * Scans the configured watch paths and the manual queue file, picks at most
 * one ready archive per tick (FR-8), extracts it, records the outcome, and
 * optionally deletes the source after verifying the extraction (FR-6).
 *
 * The driver owns no thread of its own. ua_driver_tick() does one pass and
 * returns, so M4 can call it from whatever loop it likes and keep extraction
 * off the HTTP serving path (v2 section 7).
 */
#ifndef UA_DRIVER_H
#define UA_DRIVER_H

#include "../ua_common.h"
#include "ua_config.h"
#include "ua_extract.h"

/* How many archives can be tracked as "seen but not yet stable" at once. A
 * watch path holding more than this simply gets them on later polls. */
#define UA_MAX_CANDIDATES 256

typedef struct {
    char     archive[UA_MAX_PATH];
    char     dest[UA_MAX_PATH];
    uint64_t size;
    int64_t  mtime;
    int      from_queue_file;
} ua_job;

typedef struct ua_driver ua_driver;

/* `state_dir` is where queue.txt, queue.status and debug.log live
 * (/data/fgg-unpack on the console). It is created if missing. */
ua_result ua_driver_create(const ua_config *cfg, const char *state_dir,
                           ua_driver **out);
void      ua_driver_destroy(ua_driver *d);

/* One pass. Returns UA_OK when a job ran (successful or not — check
 * report->result), or UA_ERR_NOT_FOUND when nothing was ready, which is the
 * normal idle case. `report` and `job` may be NULL. */
ua_result ua_driver_tick(ua_driver *d, ua_job *job, ua_job_report *report);

/* Blocking loop: tick, then sleep poll_interval_seconds, until *stop != 0.
 * When a job runs, the next tick happens immediately rather than after the
 * interval, so a batch of archives drains at full speed. */
void ua_driver_run(ua_driver *d, const volatile int *stop);

/* Counters, for the M4 dashboard and for the tests. */
typedef struct {
    uint64_t jobs_ok;
    uint64_t jobs_failed;
    uint64_t archives_deleted;
    uint64_t polls;
} ua_driver_stats;

void ua_driver_get_stats(const ua_driver *d, ua_driver_stats *out);

#endif /* UA_DRIVER_H */
