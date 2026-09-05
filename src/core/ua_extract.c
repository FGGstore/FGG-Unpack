#include "ua_extract.h"

#include "ua_log.h"
#include "ua_path.h"
#include "../backend/ua_backend.h"
#include "../backend/ua_detect.h"
#include "../platform/ua_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1024ull * 1024ull)

void ua_extract_opts_defaults(ua_extract_opts *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);

    /* Requirements v2 4.4 defaults. */
    o->overwrite              = 0;
    o->min_free_margin_mb     = 1024;
    o->chunk_size_kb          = 256;
    o->stability_wait_seconds = 15;

    /* Additions; see docs/spec-gaps.md. */
    o->allow_symlinks         = 0;
    o->max_expansion_ratio    = 200;
    o->expansion_floor_bytes  = 64 * MB;
    o->max_entries            = UA_MAX_ENTRIES;
    o->progress_percent_step  = 5;
    o->decode_threads         = 0;      /* auto */
    o->decode_memory_budget_mb = 1024;
}

/* ---- reporting ------------------------------------------------------ */

static void rep_fail(ua_job_report *rep, ua_result r,
                     const char *entry, const char *fmt, ...)
{
    va_list ap;

    rep->result = r;

    if (entry) {
        size_t n = strlen(entry);
        if (n >= sizeof rep->failed_entry) n = sizeof rep->failed_entry - 1;
        memcpy(rep->failed_entry, entry, n);
        rep->failed_entry[n] = '\0';
    }

    va_start(ap, fmt);
    vsnprintf(rep->detail, sizeof rep->detail, fmt, ap);
    va_end(ap);

    if (rep->failed_entry[0])
        UA_LOG_E("job failed at \"%s\": %s (%s)",
                 rep->failed_entry, rep->detail, ua_strerror(r));
    else
        UA_LOG_E("job failed: %s (%s)", rep->detail, ua_strerror(r));
}

/* ---- preflight helpers ---------------------------------------------- */

/* statvfs needs a path that exists, but the destination is created after the
 * space check (FR-2 steps 4 then 5). Walking up to the nearest existing
 * ancestor queries the right filesystem without creating anything. */
static ua_result free_space_for(const char *dest, uint64_t *out)
{
    char probe[UA_MAX_PATH];
    char parent[UA_MAX_PATH];
    size_t n;

    n = strlen(dest);
    if (n == 0 || n >= sizeof probe) return UA_ERR_LIMIT;
    memcpy(probe, dest, n + 1);

    for (;;) {
        ua_stat st;

        if (ua_plat_lstat(probe, &st) == UA_OK && st.is_dir && !st.is_symlink)
            return ua_plat_free_space(probe, out);

        ua_path_dirname(probe, parent, sizeof parent);
        if (parent[0] == '\0') return UA_ERR_NOT_FOUND;

        n = strlen(parent);
        memcpy(probe, parent, n + 1);
    }
}

static ua_result check_source(const char *archive, const ua_extract_opts *opts,
                              ua_job_report *rep)
{
    ua_stat st;
    ua_result r;
    int64_t age;

    r = ua_plat_lstat(archive, &st);
    if (r != UA_OK) {
        rep_fail(rep, r, archive, "source not readable");
        return r;
    }
    if (st.is_dir) {
        rep_fail(rep, UA_ERR_BADARG, archive, "source is a directory");
        return UA_ERR_BADARG;
    }
    if (st.is_symlink) {
        /* Following a link out of the queue file would let the queue name a
         * file the user never placed there. */
        rep_fail(rep, UA_ERR_UNSAFE_PATH, archive, "source is a symlink");
        return UA_ERR_UNSAFE_PATH;
    }
    if (st.size == 0) {
        rep_fail(rep, UA_ERR_UNSUPPORTED_FORMAT, archive, "source is empty");
        return UA_ERR_UNSUPPORTED_FORMAT;
    }

    /* FR-2 step 2: an archive still arriving over FTP would otherwise be read
     * half-written and reported as corrupt. */
    age = ua_plat_now() - st.mtime;
    if (age < (int64_t)opts->stability_wait_seconds) {
        rep_fail(rep, UA_ERR_NOT_STABLE, archive,
                 "modified %lld s ago, need %u s of quiet",
                 (long long)age, (unsigned)opts->stability_wait_seconds);
        return UA_ERR_NOT_STABLE;
    }

    rep->archive_size = st.size;

    return UA_OK;
}

/* Walks every entry once: counts them, sums the declared uncompressed size,
 * and validates each path. Doing the path checks here rather than during
 * extraction is what makes "no partial extraction on a failed preflight"
 * (FR-2) true for a malicious archive whose bad entry is not the first. */
static ua_result scan_archive(ua_backend *be, const char *dest,
                              const ua_extract_opts *opts, ua_job_report *rep)
{
    ua_entry e;
    ua_result r;

    r = be->v->reset(be);
    if (r != UA_OK) {
        rep_fail(rep, r, NULL, "cannot rewind archive");
        return r;
    }

    for (;;) {
        char safe[UA_MAX_PATH];
        char abs_path[UA_MAX_PATH];

        r = be->v->next(be, &e);
        if (r == UA_ERR_NOT_FOUND) break;
        if (r != UA_OK) {
            rep_fail(rep, r, e.path[0] ? e.path : NULL, "%s",
                     be->detail[0] ? be->detail : "cannot read entry");
            return r;
        }

        if (++rep->entries_total > opts->max_entries) {
            rep_fail(rep, UA_ERR_LIMIT, e.path,
                     "more than %llu entries",
                     (unsigned long long)opts->max_entries);
            return UA_ERR_LIMIT;
        }

        r = ua_path_sanitize_entry(e.path, strlen(e.path), safe, sizeof safe);
        if (r != UA_OK) {
            rep_fail(rep, r, e.path, "rejected entry path");
            return r;
        }

        /* An entry name that is legal on its own can still be too long once it
         * sits under the destination. Checking only the relative form here
         * would let the job start and then fail part-way through, which is
         * exactly what FR-2 forbids. */
        r = ua_path_join_checked(dest, safe, abs_path, sizeof abs_path);
        if (r != UA_OK) {
            rep_fail(rep, r, e.path,
                     "entry does not fit under destination %s", dest);
            return r;
        }

        if (e.kind == UA_ENTRY_SYMLINK) {
            /* Checked even when symlinks are disabled: an archive carrying an
             * escaping link is hostile, and saying so is more useful than
             * silently skipping it. */
            r = ua_path_check_symlink(safe, e.link_target);
            if (r != UA_OK) {
                rep_fail(rep, r, e.path, "symlink escapes destination: -> %s",
                         e.link_target);
                return r;
            }
            continue;   /* link bodies are not extraction payload */
        }

        if (e.kind == UA_ENTRY_FILE) {
            /* Overflow guard: a header can claim a size near UINT64_MAX. */
            if (e.size > UINT64_MAX - rep->bytes_total) {
                rep_fail(rep, UA_ERR_LIMIT, e.path, "declared size overflows");
                return UA_ERR_LIMIT;
            }
            rep->bytes_total += e.size;
            rep->files_expected++;
        }
    }

    if (rep->entries_total == 0) {
        rep_fail(rep, UA_ERR_CORRUPT, NULL, "archive contains no entries");
        return UA_ERR_CORRUPT;
    }

    /* Decompression-bomb guard. Not in the requirements draft: FR-2 checks
     * free space against the declared size, which a 42-byte zip declaring
     * 4 GB happily passes. */
    if (opts->max_expansion_ratio > 0 &&
        rep->bytes_total > opts->expansion_floor_bytes &&
        rep->archive_size > 0 &&
        rep->bytes_total / rep->archive_size > opts->max_expansion_ratio) {
        rep_fail(rep, UA_ERR_LIMIT, NULL,
                 "expands %llux (%llu -> %llu bytes), limit is %ux",
                 (unsigned long long)(rep->bytes_total / rep->archive_size),
                 (unsigned long long)rep->archive_size,
                 (unsigned long long)rep->bytes_total,
                 (unsigned)opts->max_expansion_ratio);
        return UA_ERR_LIMIT;
    }

    return UA_OK;
}

static ua_result check_space(const char *dest, const ua_extract_opts *opts,
                             ua_job_report *rep)
{
    uint64_t margin = opts->min_free_margin_mb * MB;
    ua_result r;

    if (rep->bytes_total > UINT64_MAX - margin) {
        rep_fail(rep, UA_ERR_LIMIT, NULL, "required size overflows");
        return UA_ERR_LIMIT;
    }
    rep->required = rep->bytes_total + margin;

    r = free_space_for(dest, &rep->free_before);
    if (r != UA_OK) {
        rep_fail(rep, r, NULL, "cannot determine free space at %s", dest);
        return r;
    }

    if (rep->free_before < rep->required) {
        rep_fail(rep, UA_ERR_NO_SPACE, NULL,
                 "need %llu MB (%llu MB payload + %llu MB margin), have %llu MB",
                 (unsigned long long)(rep->required / MB),
                 (unsigned long long)(rep->bytes_total / MB),
                 (unsigned long long)opts->min_free_margin_mb,
                 (unsigned long long)(rep->free_before / MB));
        return UA_ERR_NO_SPACE;
    }

    return UA_OK;
}

/* ---- extraction ----------------------------------------------------- */

static void maybe_progress(ua_job_report *rep, const ua_extract_opts *opts,
                           ua_progress_fn progress, void *ud, unsigned *last);

/* The sink carries the progress machinery because reporting once per *entry*
 * is useless for the archives that take a long time: a disk-image 7z holds a
 * single entry, so the job would report nothing at all until it finished. */
typedef struct {
    ua_file  *file;
    uint64_t  written;
    uint64_t  declared;
    ua_job_report *rep;

    const ua_extract_opts *opts;
    ua_progress_fn         progress;
    void                  *progress_ud;
    unsigned              *last_pct;
} file_sink;

static ua_result file_sink_write(void *ud, const void *buf, size_t len)
{
    file_sink *s = (file_sink *)ud;
    ua_result r;

    /* The central directory said how big this entry is; a stream that keeps
     * going past that is either corrupt or an attempt to fill the disk behind
     * the preflight check. Either way it stops here. */
    if (s->written + len > s->declared) return UA_ERR_CORRUPT;

    r = ua_plat_write(s->file, buf, len);
    if (r != UA_OK) return r;

    s->written        += len;
    s->rep->bytes_done += len;

    /* Throttled inside maybe_progress by percentage, so this costs a couple of
     * arithmetic ops per chunk and only does real work at a step boundary. */
    maybe_progress(s->rep, s->opts, s->progress, s->progress_ud, s->last_pct);

    return UA_OK;
}

static void maybe_progress(ua_job_report *rep, const ua_extract_opts *opts,
                           ua_progress_fn progress, void *ud, unsigned *last)
{
    unsigned pct;

    /* This runs on every written chunk, so the cheap percentage test comes
     * first and almost every call returns at it. Reading the clock, formatting
     * and logging only happen at an actual step boundary. */
    if (rep->bytes_total > 0)
        pct = (unsigned)((rep->bytes_done * 100ull) / rep->bytes_total);
    else if (rep->entries_total > 0)
        pct = (unsigned)((rep->entries_done * 100ull) / rep->entries_total);
    else
        pct = 0;

    if (pct > 100) pct = 100;
    rep->percent = pct;

    /* Rate limited by percentage step, never per entry: an archive with tens
     * of thousands of files would otherwise flood the notification UI (v2 4.6). */
    if (opts->progress_percent_step == 0) return;
    if (pct < *last + opts->progress_percent_step) return;

    *last = pct - (pct % opts->progress_percent_step);

    if (rep->started_at > 0) {
        int64_t el = ua_plat_now() - rep->started_at;

        rep->elapsed_seconds = (unsigned)(el > 0 ? el : 0);
        if (el > 0) {
            rep->bytes_per_sec = rep->bytes_done / (uint64_t)el;
            if (rep->bytes_per_sec > 0 && rep->bytes_total > rep->bytes_done)
                rep->eta_seconds = (unsigned)((rep->bytes_total - rep->bytes_done)
                                              / rep->bytes_per_sec);
            else
                rep->eta_seconds = 0;
        }
    }

    /* Logged as well as notified: the notification is transient and rate
     * limited, the log is the record that survives to be read afterwards. */
    UA_LOG_I("progress %u%%  %llu/%llu bytes  %llu MB/s  elapsed %us  eta %us",
             pct,
             (unsigned long long)rep->bytes_done,
             (unsigned long long)rep->bytes_total,
             (unsigned long long)(rep->bytes_per_sec / (1024 * 1024)),
             rep->elapsed_seconds, rep->eta_seconds);

    if (progress) progress(ud, rep);
}

/* Lets the parallel decoder report through the ordinary progress path. It
 * counts total bytes for the entry, so the running total is the bytes finished
 * before this entry plus whatever the workers have produced. */
typedef struct {
    ua_job_report         *rep;
    const ua_extract_opts *opts;
    ua_progress_fn         progress;
    void                  *ud;
    unsigned              *last_pct;
    uint64_t               base;
} parallel_ctx;

static void on_parallel_bytes(void *ud, uint64_t bytes_done)
{
    parallel_ctx *c = (parallel_ctx *)ud;

    c->rep->bytes_done = c->base + bytes_done;
    maybe_progress(c->rep, c->opts, c->progress, c->ud, c->last_pct);
}

static ua_result extract_one_file(ua_backend *be, const ua_entry *e,
                                  const char *abs_path,
                                  const ua_extract_opts *opts,
                                  ua_job_report *rep,
                                  ua_progress_fn progress, void *progress_ud,
                                  unsigned *last_pct)
{
    file_sink sink;
    ua_result r, cr;
    char parent[UA_MAX_PATH];

    ua_path_dirname(abs_path, parent, sizeof parent);
    if (parent[0]) {
        r = ua_plat_mkdirs(parent);
        if (r != UA_OK) {
            rep_fail(rep, r, e->path, "cannot create directory %s", parent);
            return r;
        }
    }

    r = ua_plat_open_write(abs_path, opts->overwrite, &sink.file);
    if (r != UA_OK) {
        rep_fail(rep, r, e->path,
                 r == UA_ERR_EXISTS ? "destination file exists (overwrite=0)"
                                    : "cannot open %s for writing", abs_path);
        return r;
    }

    sink.written     = 0;
    sink.declared    = e->size;
    sink.rep         = rep;
    sink.opts        = opts;
    sink.progress    = progress;
    sink.progress_ud = progress_ud;
    sink.last_pct    = last_pct;

    /* Fast path: a backend that can decode this entry out of order writes it
     * straight to the file with several workers. It declines with
     * UA_ERR_NOT_FOUND whenever that is not possible, and the sequential path
     * below runs instead, so this is purely additive. */
    if (be->v->extract_current_parallel && opts->decode_threads != 1) {
        ua_parallel_opts po;
        parallel_ctx     ctx;
        uint64_t         got = 0;
        unsigned         threads = opts->decode_threads;

        if (threads == 0) {
            int cpus = ua_plat_cpu_count();
            threads = (unsigned)(cpus > 0 ? cpus : 1);
            if (threads > 8) threads = 8;
        }

        ctx.rep      = rep;
        ctx.opts     = opts;
        ctx.progress = progress;
        ctx.ud       = progress_ud;
        ctx.last_pct = last_pct;
        ctx.base     = rep->bytes_done;

        po.threads          = threads;
        po.memory_budget_mb = opts->decode_memory_budget_mb;
        po.on_bytes         = on_parallel_bytes;
        po.ud               = &ctx;
        po.out              = sink.file;   /* shared; workers use positioned writes */

        r = be->v->extract_current_parallel(be, abs_path, &po, &got);

        if (r == UA_OK) {
            rep->bytes_done = ctx.base + got;
            UA_LOG_I("parallel decode: %u workers, %llu bytes",
                     threads, (unsigned long long)got);

            if (got != e->size) {
                ua_plat_close(sink.file);
                rep_fail(rep, UA_ERR_CORRUPT, e->path,
                         "parallel decode wrote %llu bytes, header declared %llu",
                         (unsigned long long)got, (unsigned long long)e->size);
                return UA_ERR_CORRUPT;
            }

            r = ua_plat_close(sink.file);
            if (r != UA_OK) {
                rep_fail(rep, r, e->path, "cannot close %s", abs_path);
                return r;
            }

            if (e->mtime > 0) ua_plat_set_mtime(abs_path, e->mtime);
            rep->files_written++;
            return UA_OK;
        }

        /* Anything other than a decline is a failure of the fast path, not of
         * the job: the sequential decoder can still do the work. Falling back
         * rather than failing means a platform quirk in the parallel path
         * costs speed, never the extraction. */
        if (r != UA_ERR_NOT_FOUND)
            UA_LOG_W("parallel decode unavailable (%s: %s); using one thread",
                     ua_strerror(r), be->detail[0] ? be->detail : "no detail");

        /* Whatever the workers may have written is discarded: reopened
         * truncating, so the sequential pass starts from an empty file. */
        ua_plat_close(sink.file);
        sink.file = NULL;

        r = ua_plat_open_write(abs_path, 1, &sink.file);
        if (r != UA_OK) {
            rep_fail(rep, r, e->path, "cannot reopen %s", abs_path);
            return r;
        }

        rep->bytes_done = ctx.base;   /* discard the partial byte count too */
    }

    r  = be->v->extract_current(be, file_sink_write, &sink);
    cr = ua_plat_close(sink.file);
    if (r == UA_OK) r = cr;

    if (r != UA_OK) {
        rep_fail(rep, r, e->path, "%s",
                 be->detail[0] ? be->detail : "write failed mid-entry");
        return r;
    }

    if (sink.written != e->size) {
        rep_fail(rep, UA_ERR_CORRUPT, e->path,
                 "wrote %llu bytes, header declared %llu",
                 (unsigned long long)sink.written,
                 (unsigned long long)e->size);
        return UA_ERR_CORRUPT;
    }

    /* FR-5: timestamps where the format carries them; permissions deliberately
     * not taken from the archive. Failure to set the time is not a job
     * failure. */
    if (e->mtime > 0) ua_plat_set_mtime(abs_path, e->mtime);

    rep->files_written++;

    return UA_OK;
}

/* ---- entry points --------------------------------------------------- */

static ua_result open_and_scan(const char *archive, const char *dest,
                               const ua_extract_opts *opts,
                               ua_job_report *rep, ua_backend *be)
{
    ua_result r;

    r = check_source(archive, opts, rep);
    if (r != UA_OK) return r;

    r = ua_detect_file(archive, &rep->format);
    if (r != UA_OK) {
        rep_fail(rep, r, archive, "content does not match any supported format");
        return r;
    }
    UA_LOG_I("detected format: %s", ua_format_name(rep->format));

    r = ua_backend_open(rep->format, archive,
                        (size_t)opts->chunk_size_kb * 1024, be);
    if (r != UA_OK) {
        rep_fail(rep, r, archive, "%s",
                 be->detail[0] ? be->detail : "cannot open archive");
        return r;
    }

    r = scan_archive(be, dest, opts, rep);
    if (r != UA_OK) return r;

    UA_LOG_I("preflight: %llu entries, %llu bytes uncompressed",
             (unsigned long long)rep->entries_total,
             (unsigned long long)rep->bytes_total);

    return check_space(dest, opts, rep);
}

ua_result ua_extract_preflight(const char *archive_path, const char *dest_dir,
                               const ua_extract_opts *opts,
                               ua_job_report *report)
{
    ua_backend be;
    ua_result r;

    if (!archive_path || !dest_dir || !opts || !report) return UA_ERR_BADARG;

    memset(report, 0, sizeof *report);
    memset(&be, 0, sizeof be);

    r = open_and_scan(archive_path, dest_dir, opts, report, &be);

    ua_backend_close(&be);
    report->result = r;

    return r;
}

ua_result ua_extract_run(const char *archive_path, const char *dest_dir,
                         const ua_extract_opts *opts,
                         ua_progress_fn progress, void *progress_ud,
                         ua_job_report *report)
{
    ua_backend be;
    ua_entry   e;
    ua_result  r;
    unsigned   last_pct = 0;

    if (!archive_path || !dest_dir || !opts || !report) return UA_ERR_BADARG;

    memset(report, 0, sizeof *report);
    memset(&be, 0, sizeof be);

    report->started_at = ua_plat_now();
    UA_LOG_I("job start: %s -> %s", archive_path, dest_dir);

    r = open_and_scan(archive_path, dest_dir, opts, report, &be);
    if (r != UA_OK) goto done;

    /* FR-2 step 5, deliberately last: nothing exists on disk until every
     * check has passed. */
    r = ua_plat_mkdirs(dest_dir);
    if (r != UA_OK) {
        rep_fail(report, r, NULL, "cannot create destination %s", dest_dir);
        goto done;
    }

    r = be.v->reset(&be);
    if (r != UA_OK) {
        rep_fail(report, r, NULL, "cannot rewind archive for extraction");
        goto done;
    }

    for (;;) {
        char safe[UA_MAX_PATH];
        char abs_path[UA_MAX_PATH];

        r = be.v->next(&be, &e);
        if (r == UA_ERR_NOT_FOUND) { r = UA_OK; break; }
        if (r != UA_OK) {
            rep_fail(report, r, e.path[0] ? e.path : NULL, "%s",
                     be.detail[0] ? be.detail : "cannot read entry");
            goto done;
        }

        /* Re-validated rather than cached from the scan: the check is cheap
         * and a backend that returns different names on a second pass must not
         * be able to slip one past. */
        r = ua_path_sanitize_entry(e.path, strlen(e.path), safe, sizeof safe);
        if (r != UA_OK) {
            rep_fail(report, r, e.path, "rejected entry path");
            goto done;
        }

        r = ua_path_join_checked(dest_dir, safe, abs_path, sizeof abs_path);
        if (r != UA_OK) {
            rep_fail(report, r, e.path, "cannot place entry under destination");
            goto done;
        }

        switch (e.kind) {
        case UA_ENTRY_DIR:
            r = ua_plat_mkdirs(abs_path);
            if (r != UA_OK) {
                rep_fail(report, r, e.path, "cannot create directory");
                goto done;
            }
            report->dirs_created++;
            break;

        case UA_ENTRY_FILE:
            r = extract_one_file(&be, &e, abs_path, opts, report,
                                 progress, progress_ud, &last_pct);
            if (r != UA_OK) goto done;
            break;

        case UA_ENTRY_SYMLINK:
            if (!opts->allow_symlinks) {
                report->symlinks_skipped++;
                UA_LOG_W("skipped symlink %s -> %s (allow_symlinks=0)",
                         safe, e.link_target);
                break;
            }
            r = ua_path_check_symlink(safe, e.link_target);
            if (r != UA_OK) {
                rep_fail(report, r, e.path, "symlink escapes destination");
                goto done;
            }
            r = ua_plat_symlink(e.link_target, abs_path);
            if (r == UA_ERR_UNSUPPORTED_FORMAT) {
                report->symlinks_skipped++;
                UA_LOG_W("platform cannot create symlink %s, skipped", safe);
                r = UA_OK;
            } else if (r != UA_OK) {
                rep_fail(report, r, e.path, "cannot create symlink");
                goto done;
            }
            break;

        case UA_ENTRY_OTHER:
        default:
            /* Device nodes and fifos have no business on the console
             * filesystem and no format we support needs them. */
            UA_LOG_W("skipped unsupported entry type: %s", safe);
            break;
        }

        report->entries_done++;
        maybe_progress(report, opts, progress, progress_ud, &last_pct);
    }

    report->percent = 100;
    UA_LOG_I("job done: %llu files, %llu dirs, %llu bytes, %llu symlinks skipped",
             (unsigned long long)report->files_written,
             (unsigned long long)report->dirs_created,
             (unsigned long long)report->bytes_done,
             (unsigned long long)report->symlinks_skipped);

done:
    ua_backend_close(&be);
    report->result = r;

    /* FR-7: a half-extracted tree is left in place on purpose. Deleting it
     * would destroy whatever was already at the destination when overwrite=1,
     * and the requirements only demand that the failure be recorded, which
     * rep_fail has already done. */
    return r;
}
