#include "ua_driver.h"

#include "ua_log.h"
#include "ua_path.h"
#include "ua_queue.h"
#include "ua_status.h"
#include "../backend/ua_detect.h"
#include "../platform/ua_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A file seen during a scan but not yet eligible to run.
 *
 * v2 4.1 requires that size *and* mtime have been stable for
 * stability_wait_seconds. Age alone is not enough: some FTP servers stamp the
 * final mtime when a transfer starts, so a file still arriving can already
 * look old. Comparing across polls is the only way to see it settle, which is
 * why this table exists at all. */
typedef struct {
    char     path[UA_MAX_PATH];
    uint64_t size;
    int64_t  mtime;
    int64_t  stable_since;   /* when this exact (size, mtime) was first seen */
    int      is_archive;     /* cached content sniff for this (size, mtime) */
    int      touched;        /* survived the current scan */
} ua_candidate;

struct ua_driver {
    ua_config    cfg;
    char         state_dir[UA_MAX_PATH];
    char         queue_file[UA_MAX_PATH];
    char         lock_path[UA_MAX_PATH];
    ua_status   *status;

    ua_candidate cand[UA_MAX_CANDIDATES];
    size_t       n_cand;

    /* Basename of the job in flight, so progress notifications can name it. */
    char         current_name[64];

    ua_driver_stats stats;
};

/* ---- single-instance lock -------------------------------------------
 *
 * FR-8 is about one job at a time, and the driver honours that within a
 * process. Nothing stopped two *processes*, though, and a payload loader makes
 * double-launching easy: two drivers then race on the same archives with
 * separate state. Observed in practice, where one instance created the output
 * file and the other refused it as already existing. With overwrite=1 they
 * would have interleaved writes into one file and produced a corrupt image
 * with no error at all.
 *
 * The lock is a file whose mtime is refreshed while the driver is alive. A
 * fresh one means somebody else is running; a stale one is left by a crash or
 * a reboot and gets taken over. There is no flock() to rely on here, and a
 * heartbeat needs nothing from the filesystem beyond mtime.
 */
#define UA_LOCK_STALE_SECONDS 600

static void lock_touch(const ua_driver *d)
{
    ua_file *f;

    if (ua_plat_open_write(d->lock_path, 1, &f) != UA_OK) return;
    ua_plat_write(f, "fgg-unpack\n", 11);
    ua_plat_close(f);
}

/* Returns 1 when another instance is alive and holding the lock. */
static int lock_held_by_other(const char *path)
{
    ua_stat st;
    int64_t age;

    if (ua_plat_lstat(path, &st) != UA_OK) return 0;   /* no lock at all */

    age = ua_plat_now() - st.mtime;

    return age >= 0 && age < UA_LOCK_STALE_SECONDS;
}

/* ---- notifications --------------------------------------------------- */

static void notify(const ua_driver *d, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    if (!d->cfg.notify) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    ua_plat_notify(msg);
}

/* Human sizes, because "163940663296 bytes" in a toast tells nobody anything. */
static void fmt_size(uint64_t bytes, char *out, size_t n)
{
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(out, n, "%.1f GB", (double)bytes / 1073741824.0);
    else if (bytes >= 1024 * 1024)
        snprintf(out, n, "%.0f MB", (double)bytes / 1048576.0);
    else
        snprintf(out, n, "%llu B", (unsigned long long)bytes);
}

static void fmt_dur(unsigned secs, char *out, size_t n)
{
    if (secs >= 3600) snprintf(out, n, "%uh %um", secs / 3600, (secs % 3600) / 60);
    else if (secs >= 60) snprintf(out, n, "%um", secs / 60);
    else snprintf(out, n, "%us", secs);
}

/* The archive this tool exists for holds a single 150 GB entry, so an
 * entries-done counter reads "0/1" for an hour and a half. Bytes, rate and a
 * time remaining are what actually tell you it is alive and progressing. */
static void on_progress(void *ud, const ua_job_report *rep)
{
    const ua_driver *d = (const ua_driver *)ud;
    char done[32], total[32], eta[32];

    lock_touch(d);

    fmt_size(rep->bytes_done, done, sizeof done);
    fmt_size(rep->bytes_total, total, sizeof total);
    fmt_dur(rep->eta_seconds, eta, sizeof eta);

    if (rep->bytes_per_sec > 0 && rep->eta_seconds > 0)
        notify(d, "%s  %u%%  %s / %s  %llu MB/s  %s left",
               d->current_name, rep->percent, done, total,
               (unsigned long long)(rep->bytes_per_sec / (1024 * 1024)), eta);
    else
        notify(d, "%s  %u%%  %s / %s",
               d->current_name, rep->percent, done, total);
}

/* ---- candidate table ------------------------------------------------- */

static ua_candidate *cand_find(ua_driver *d, const char *path)
{
    size_t i;

    for (i = 0; i < d->n_cand; i++)
        if (!strcmp(d->cand[i].path, path))
            return &d->cand[i];

    return NULL;
}

static ua_candidate *cand_add(ua_driver *d, const char *path)
{
    ua_candidate *c;
    size_t len = strlen(path);

    if (len >= UA_MAX_PATH) return NULL;
    if (d->n_cand >= UA_MAX_CANDIDATES) return NULL;

    c = &d->cand[d->n_cand++];
    memset(c, 0, sizeof *c);
    memcpy(c->path, path, len + 1);

    return c;
}

/* Drops entries that were not seen in the scan just finished, so a file
 * removed from a watch path stops occupying a slot. */
static void cand_sweep(ua_driver *d)
{
    size_t i = 0;

    while (i < d->n_cand) {
        if (!d->cand[i].touched) {
            memmove(&d->cand[i], &d->cand[i + 1],
                    (d->n_cand - i - 1) * sizeof d->cand[0]);
            d->n_cand--;
            continue;
        }
        d->cand[i].touched = 0;
        i++;
    }
}

/* Observes one file. Returns 1 when it is an archive that has been stable long
 * enough and has not already been processed. */
static int observe(ua_driver *d, const char *path, const ua_stat *st)
{
    ua_candidate *c;
    int64_t now = ua_plat_now();

    c = cand_find(d, path);
    if (!c) {
        c = cand_add(d, path);
        if (!c) return 0;               /* table full; picked up on a later poll */
        c->size = st->size;
        c->mtime = st->mtime;
        c->stable_since = now;
        c->is_archive = -1;             /* not sniffed yet */
    } else if (c->size != st->size || c->mtime != st->mtime) {
        /* Still changing: restart the clock and re-sniff, since the content
         * may not have been an archive when we last looked. */
        c->size = st->size;
        c->mtime = st->mtime;
        c->stable_since = now;
        c->is_archive = -1;
    }

    c->touched = 1;

    if (now - c->stable_since < (int64_t)d->cfg.stability_wait_seconds)
        return 0;

    if (c->is_archive < 0) {
        ua_format fmt = UA_FMT_UNKNOWN;

        /* FR-1: by content. Only done once the file has settled, so a
         * half-written upload is not sniffed repeatedly, and cached against
         * (size, mtime) so a quiet watch path costs one stat per file. */
        c->is_archive = (ua_detect_file(path, &fmt) == UA_OK);
        if (!c->is_archive)
            UA_LOG_D("ignoring non-archive %s", path);
    }
    if (!c->is_archive) return 0;

    if (ua_status_seen(d->status, path, st->size, st->mtime)) return 0;

    return 1;
}

/* ---- scanning -------------------------------------------------------- */

/* Depth 1 looks only at files directly in `dir`; depth 2 descends one level
 * (v2 4.1 scan_depth). Returns 1 as soon as a ready job is found: FR-8 allows
 * only one at a time, so there is no reason to keep walking. */
static int scan_dir(ua_driver *d, const char *dir, unsigned depth, ua_job *job)
{
    ua_dirent ent;
    ua_dir *dh;
    int found = 0;

    if (ua_plat_opendir(dir, &dh) != UA_OK) {
        UA_LOG_D("watch path not readable: %s", dir);
        return 0;
    }

    while (!found && ua_plat_readdir(dh, &ent) == UA_OK) {
        char full[UA_MAX_PATH];
        ua_stat st;

        if (snprintf(full, sizeof full, "%s/%s", dir, ent.name) >= (int)sizeof full)
            continue;

        if (ua_plat_lstat(full, &st) != UA_OK) continue;

        /* Never follow links out of a watch path: the destination is derived
         * from the archive path, and a link could point anywhere. */
        if (st.is_symlink) continue;

        if (st.is_dir) {
            if (depth > 1)
                found = scan_dir(d, full, depth - 1, job);
            continue;
        }

        if (st.size == 0) continue;

        if (observe(d, full, &st)) {
            snprintf(job->archive, sizeof job->archive, "%s", full);
            /* v2 4.1: the destination is the archive's own directory. */
            ua_path_dirname(full, job->dest, sizeof job->dest);
            if (job->dest[0] == '\0')
                snprintf(job->dest, sizeof job->dest, "%s", dir);
            job->size = st.size;
            job->mtime = st.mtime;
            job->from_queue_file = 0;
            found = 1;
        }
    }

    ua_plat_closedir(dh);
    return found;
}

/* The manual queue takes precedence: it is an explicit instruction, whereas a
 * watch path is a standing one. */
static int scan_queue_file(ua_driver *d, ua_job *job)
{
    char line[UA_MAX_PATH * 2];
    unsigned lineno = 0;
    int found = 0;
    FILE *fp;

    fp = fopen(d->queue_file, "rb");
    if (!fp) return 0;

    while (!found && fgets(line, sizeof line, fp)) {
        char archive[UA_MAX_PATH];
        char dest[UA_MAX_PATH];
        ua_stat st;
        ua_result r;

        lineno++;

        r = ua_queue_parse_line(line, archive, sizeof archive, dest, sizeof dest);
        if (r == UA_ERR_NOT_FOUND) continue;
        if (r != UA_OK) {
            UA_LOG_W("%s:%u: cannot parse line", d->queue_file, lineno);
            continue;
        }

        if (ua_plat_lstat(archive, &st) != UA_OK) {
            UA_LOG_D("%s:%u: %s not present", d->queue_file, lineno, archive);
            continue;
        }
        if (st.is_dir || st.is_symlink || st.size == 0) continue;

        if (!observe(d, archive, &st)) continue;

        snprintf(job->archive, sizeof job->archive, "%s", archive);
        if (dest[0])
            snprintf(job->dest, sizeof job->dest, "%s", dest);
        else
            ua_path_dirname(archive, job->dest, sizeof job->dest);
        job->size = st.size;
        job->mtime = st.mtime;
        job->from_queue_file = 1;
        found = 1;
    }

    fclose(fp);
    return found;
}

/* ---- FR-6 verified delete -------------------------------------------- */

/* Every condition FR-6 lists, checked explicitly. A deleted archive after a
 * bad extraction is unrecoverable, so this fails closed: anything unexpected
 * keeps the file. */
static int extraction_is_verified(const ua_job_report *rep, const char **why)
{
    if (rep->result != UA_OK) {
        *why = "job did not succeed";
        return 0;
    }
    if (rep->entries_done != rep->entries_total) {
        *why = "entry count does not match the archive";
        return 0;
    }
    if (rep->files_written != rep->files_expected) {
        *why = "written file count does not match the archive";
        return 0;
    }
    /* Per-file sizes are enforced during extraction: extract_one_file fails
     * the job if an entry finishes short of or beyond its declared size, so
     * reaching here means every file matched. */
    if (rep->bytes_done != rep->bytes_total) {
        *why = "byte total does not match the archive";
        return 0;
    }

    *why = NULL;
    return 1;
}

static void maybe_delete(ua_driver *d, const ua_job *job,
                         const ua_job_report *rep)
{
    const char *why = NULL;

    if (!d->cfg.delete_after_extract) return;

    if (!extraction_is_verified(rep, &why)) {
        UA_LOG_W("keeping %s: %s", job->archive, why ? why : "unverified");
        return;
    }

    if (ua_plat_unlink(job->archive) != UA_OK) {
        UA_LOG_W("extraction verified but could not delete %s", job->archive);
        return;
    }

    d->stats.archives_deleted++;
    UA_LOG_I("deleted %s after verified extraction", job->archive);
}

/* ---- lifecycle ------------------------------------------------------- */

ua_result ua_driver_create(const ua_config *cfg, const char *state_dir,
                           ua_driver **out)
{
    char status_path[UA_MAX_PATH];
    ua_driver *d;
    ua_result r;

    if (!cfg || !state_dir || !out) return UA_ERR_BADARG;
    *out = NULL;

    d = (ua_driver *)calloc(1, sizeof *d);
    if (!d) return UA_ERR_NOMEM;

    d->cfg = *cfg;

    if (snprintf(d->state_dir, sizeof d->state_dir, "%s", state_dir)
            >= (int)sizeof d->state_dir ||
        snprintf(d->queue_file, sizeof d->queue_file, "%s/queue.txt", state_dir)
            >= (int)sizeof d->queue_file ||
        snprintf(status_path, sizeof status_path, "%s/queue.status", state_dir)
            >= (int)sizeof status_path ||
        snprintf(d->lock_path, sizeof d->lock_path, "%s/fgg-unpack.lock", state_dir)
            >= (int)sizeof d->lock_path) {
        free(d);
        return UA_ERR_LIMIT;
    }

    r = ua_plat_mkdirs(d->state_dir);
    if (r != UA_OK) { free(d); return r; }

    /* Checked after the state directory exists but before any state is
     * loaded, so a second instance stops before it can act on anything. */
    if (lock_held_by_other(d->lock_path)) {
        UA_LOG_E("another FGG Unpack instance is already running (%s); refusing to start",
                 d->lock_path);
        free(d);
        return UA_ERR_EXISTS;
    }
    lock_touch(d);

    r = ua_status_open(status_path, &d->status);
    if (r != UA_OK) { free(d); return r; }

    *out = d;
    return UA_OK;
}

void ua_driver_destroy(ua_driver *d)
{
    if (!d) return;

    /* Released explicitly so a clean shutdown does not make the next start
     * wait out the staleness window. */
    ua_plat_unlink(d->lock_path);

    ua_status_close(d->status);
    free(d);
}

ua_result ua_driver_tick(ua_driver *d, ua_job *job_out, ua_job_report *rep_out)
{
    ua_extract_opts opts;
    ua_job_report   rep;
    ua_job          job;
    size_t          i;
    int             found;

    if (!d) return UA_ERR_BADARG;

    d->stats.polls++;
    lock_touch(d);

    memset(&job, 0, sizeof job);

    found = scan_queue_file(d, &job);
    for (i = 0; !found && i < d->cfg.n_watchpath; i++)
        found = scan_dir(d, d->cfg.watchpath[i], d->cfg.scan_depth, &job);

    cand_sweep(d);

    if (!found) return UA_ERR_NOT_FOUND;

    /* Notifications carry the basename: a full console path does not fit in a
     * toast and the leading directories are the same every time anyway. */
    {
        const char *base = job.archive, *p;

        for (p = job.archive; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;

        snprintf(d->current_name, sizeof d->current_name, "%s", base);
    }

    UA_LOG_I("job accepted: %s -> %s (%llu bytes%s)",
             job.archive, job.dest, (unsigned long long)job.size,
             job.from_queue_file ? ", from queue.txt" : "");
    notify(d, "FGG Unpack: starting %s", d->current_name);

    ua_config_to_extract_opts(&d->cfg, &opts);

    /* The driver has already established stability across polls, which is the
     * stronger check; the extractor's own age test would only re-reject a file
     * that legitimately settled fast. */
    opts.stability_wait_seconds = 0;

    ua_extract_run(job.archive, job.dest, &opts,
                   d->cfg.progress_percent_step ? on_progress : NULL, d, &rep);

    if (rep.result == UA_OK) {
        char size[32], took[32];

        d->stats.jobs_ok++;

        fmt_size(rep.bytes_done, size, sizeof size);
        fmt_dur(rep.elapsed_seconds, took, sizeof took);

        UA_LOG_I("job summary: %s, %llu files, %s in %s, average %llu MB/s",
                 d->current_name, (unsigned long long)rep.files_written,
                 size, took,
                 (unsigned long long)(rep.elapsed_seconds
                     ? rep.bytes_done / rep.elapsed_seconds / (1024 * 1024) : 0));

        notify(d, "FGG Unpack: %s done  %s in %s", d->current_name, size, took);
        maybe_delete(d, &job, &rep);
    } else {
        d->stats.jobs_failed++;
        notify(d, "FGG Unpack: %s FAILED - %s", d->current_name, rep.detail);
    }

    /* Recorded whatever the outcome, so a failing archive is not retried on
     * every poll for as long as it sits there (v2 4.3, FR-7). */
    if (ua_status_record(d->status, job.archive, job.size, job.mtime,
                         rep.result) != UA_OK)
        UA_LOG_W("could not update queue.status; %s may be reprocessed",
                 job.archive);

    if (job_out) *job_out = job;
    if (rep_out) *rep_out = rep;

    return UA_OK;
}

void ua_driver_run(ua_driver *d, const volatile int *stop)
{
    if (!d) return;

    UA_LOG_I("watch driver started: %u path(s), poll %us, depth %u",
             (unsigned)d->cfg.n_watchpath, d->cfg.poll_interval_seconds,
             d->cfg.scan_depth);

    while (!stop || !*stop) {
        if (ua_driver_tick(d, NULL, NULL) == UA_OK)
            continue;   /* drain a backlog without waiting a full interval */

        /* Sleeping in short slices keeps shutdown responsive even when the
         * poll interval is set to the maximum a config allows. */
        {
            unsigned remaining = d->cfg.poll_interval_seconds;

            while (remaining > 0 && (!stop || !*stop)) {
                ua_plat_sleep_ms(1000);
                remaining--;
            }
        }
    }

    UA_LOG_I("watch driver stopped");
}

void ua_driver_get_stats(const ua_driver *d, ua_driver_stats *out)
{
    if (!d || !out) return;
    *out = d->stats;
}
