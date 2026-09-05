/* /data/fgg-unpack/config.ini (requirements v2 4.4).
 *
 * Every key is optional; a missing or unreadable file is not an error, it just
 * means defaults. Section 7 asks the payload to tolerate being started fresh
 * at any time, and refusing to run because a config file is malformed would be
 * the opposite of that: unparseable lines are logged and skipped.
 */
#ifndef UA_CONFIG_H
#define UA_CONFIG_H

#include "../ua_common.h"
#include "ua_extract.h"

#define UA_MAX_WATCHPATHS 16

typedef struct {
    int      debug;

    char     watchpath[UA_MAX_WATCHPATHS][UA_MAX_PATH];
    size_t   n_watchpath;

    unsigned scan_depth;                /* 1 = first level only, 2 = one nested */
    unsigned poll_interval_seconds;     /* clamped to 1..3600 */
    unsigned stability_wait_seconds;
    int      delete_after_extract;
    int      notify;
    int      overwrite;
    uint64_t min_free_margin_mb;
    unsigned chunk_size_kb;
    unsigned http_port;                 /* deprecated: the web UI was cut */

    /* Not in the requirements table; see docs/spec-gaps.md. Exposed as keys so
     * a decision can be changed on-console without a rebuild. */
    int      allow_symlinks;
    unsigned max_expansion_ratio;
    unsigned progress_percent_step;
    unsigned decode_threads;
    unsigned decode_memory_budget_mb;
} ua_config;

void ua_config_defaults(ua_config *c);

/* Loads `path` over the defaults. Returns UA_OK even when the file is absent:
 * `created` (may be NULL) is set to 1 if a template was written. */
ua_result ua_config_load(const char *path, ua_config *c, int *created);

/* Writes the commented template the requirements ask to ship. */
ua_result ua_config_write_template(const char *path);

/* Applies one "key=value" pair. Exposed for the tests and, later, for the M4
 * settings view. Returns UA_ERR_BADARG for an unknown key or bad value; the
 * caller decides whether that is fatal (it is not, during load). */
ua_result ua_config_set(ua_config *c, const char *key, const char *value);

void ua_config_to_extract_opts(const ua_config *c, ua_extract_opts *o);

#endif /* UA_CONFIG_H */
