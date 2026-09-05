/* The M3 watch driver: scanning, cross-poll stability, job tracking and the
 * FR-6 verified delete. */
#include "../src/core/ua_driver.h"
#include "../src/core/ua_status.h"
#include "../src/platform/ua_platform.h"
#include "ua_test.h"

#include <stdio.h>
#include <stdlib.h>

#define CORPUS  "tests/corpus/"
#define TMPROOT "build/tmp/"

/* Each case gets its own tree; run_tests.ps1 wipes build/tmp beforehand. */
typedef struct {
    char watch[UA_MAX_PATH];
    char state[UA_MAX_PATH];
} test_env;

static void env_make(test_env *e, const char *name)
{
    snprintf(e->watch, sizeof e->watch, "%sdrv_%s/watch", TMPROOT, name);
    snprintf(e->state, sizeof e->state, "%sdrv_%s/state", TMPROOT, name);
    ua_plat_mkdirs(e->watch);
    ua_plat_mkdirs(e->state);
}

static int copy_into(const char *corpus_name, const char *dir, const char *as)
{
    char src[512], dst[UA_MAX_PATH];
    char buf[65536];
    size_t n;
    FILE *in, *out;

    snprintf(src, sizeof src, "%s%s", CORPUS, corpus_name);
    snprintf(dst, sizeof dst, "%s/%s", dir, as);

    in = fopen(src, "rb");
    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }

    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);

    fclose(in);
    fclose(out);
    return 1;
}

static int exists(const char *path)
{
    ua_stat st;
    return ua_plat_lstat(path, &st) == UA_OK;
}

static void base_config(ua_config *c, const test_env *e)
{
    ua_config_defaults(c);
    ua_config_set(c, "watchpath", e->watch);
    c->stability_wait_seconds = 0;   /* stability has its own test below */
    c->notify = 0;                   /* keeps test output readable */
    c->min_free_margin_mb = 1;
    c->progress_percent_step = 0;
}

/* ---- basic watch behaviour ------------------------------------------ */

static void test_extracts_in_place(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_job job;
    ua_job_report rep;
    char path[UA_MAX_PATH];

    env_make(&e, "inplace");
    base_config(&c, &e);
    UA_CHECK(copy_into("simple.zip", e.watch, "simple.zip"), "corpus copied");

    UA_EQ_INT(ua_driver_create(&c, e.state, &d), UA_OK, "driver starts");

    UA_EQ_INT(ua_driver_tick(d, &job, &rep), UA_OK, "job ran");
    UA_EQ_INT(rep.result, UA_OK, "extraction succeeded: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 6, "all files written");

    /* v2 4.1: the destination is the archive's own directory, so the contents
     * land beside it rather than in a subfolder. */
    snprintf(path, sizeof path, "%s/readme.txt", e.watch);
    UA_CHECK(exists(path), "extracted in place next to the archive");
    snprintf(path, sizeof path, "%s/dir/sub/two.txt", e.watch);
    UA_CHECK(exists(path), "nested structure preserved");

    /* Source is kept: delete_after_extract defaults off. */
    snprintf(path, sizeof path, "%s/simple.zip", e.watch);
    UA_CHECK(exists(path), "archive kept by default");

    /* v2 4.3: recorded, so it is not redone every poll. */
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "not reprocessed on the next poll");

    ua_driver_destroy(d);
}

static void test_refuses_second_instance(void)
{
    test_env e;
    ua_config c;
    ua_driver *first = NULL, *second = NULL;

    env_make(&e, "lock");
    base_config(&c, &e);

    UA_EQ_INT(ua_driver_create(&c, e.state, &first), UA_OK, "first instance starts");

    /* Two payloads racing on the same archives is how a correctly sized but
     * wrong-content image gets produced, so the second one must not run at
     * all. This is exactly what a double launch from a payload loader does. */
    UA_EQ_INT(ua_driver_create(&c, e.state, &second), UA_ERR_EXISTS,
              "second instance refuses to start");
    UA_CHECK(second == NULL, "no driver handed back to the second caller");

    /* Once the holder shuts down cleanly the lock is released, so the next
     * start does not have to wait out the staleness window. */
    ua_driver_destroy(first);

    UA_EQ_INT(ua_driver_create(&c, e.state, &second), UA_OK,
              "starts again after the holder exits");
    ua_driver_destroy(second);
}

static void test_survives_restart(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_driver_stats s;

    env_make(&e, "restart");
    base_config(&c, &e);
    copy_into("simple.zip", e.watch, "simple.zip");

    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_OK, "first run extracts");
    ua_driver_destroy(d);

    /* v2 section 7: starting again must not reprocess completed jobs. The
     * jailbreak does not survive a reboot, so this is the normal case, not an
     * edge case. */
    d = NULL;
    UA_EQ_INT(ua_driver_create(&c, e.state, &d), UA_OK, "driver restarts");
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "completed job not redone after restart");

    ua_driver_get_stats(d, &s);
    UA_EQ_INT(s.jobs_ok, 0, "no job ran in the second process");

    ua_driver_destroy(d);
}

static void test_ignores_non_archives(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;

    env_make(&e, "nonarchive");
    base_config(&c, &e);

    /* FR-1 is by content, so a non-archive is ignored no matter what it is
     * called, and an archive is picked up no matter what it is called. */
    copy_into("notarchive.bin", e.watch, "looks-like.zip");
    copy_into("empty.bin", e.watch, "empty.zip");

    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "plain data named .zip is ignored");

    ua_driver_destroy(d);
}

static void test_detects_by_content_not_name(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_job job;
    ua_job_report rep;

    env_make(&e, "byname");
    base_config(&c, &e);
    copy_into("simple.zip", e.watch, "no-extension-at-all");

    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, &job, &rep), UA_OK, "extensionless archive found");
    UA_EQ_INT(rep.result, UA_OK, "and extracted: %s", rep.detail);

    ua_driver_destroy(d);
}

static void test_one_job_per_tick(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_driver_stats s;

    env_make(&e, "onejob");
    base_config(&c, &e);
    copy_into("simple.zip", e.watch, "a.zip");
    copy_into("unicode.zip", e.watch, "b.zip");
    copy_into("symlink_safe.zip", e.watch, "c.zip");

    ua_driver_create(&c, e.state, &d);

    /* FR-8: one archive at a time. Three ticks, three jobs, then idle. */
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_OK, "tick 1");
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_OK, "tick 2");
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_OK, "tick 3");
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND, "then idle");

    ua_driver_get_stats(d, &s);
    UA_EQ_INT(s.jobs_ok, 3, "three jobs succeeded");
    UA_EQ_INT(s.jobs_failed, 0, "none failed");

    ua_driver_destroy(d);
}

static void test_scan_depth(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    char nested[UA_MAX_PATH];

    env_make(&e, "depth");
    snprintf(nested, sizeof nested, "%s/sub", e.watch);
    ua_plat_mkdirs(nested);
    copy_into("simple.zip", nested, "deep.zip");

    /* Depth 1 is first-level only. */
    base_config(&c, &e);
    c.scan_depth = 1;
    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "depth 1 does not descend");
    ua_driver_destroy(d);

    /* Depth 2 adds exactly one nested level. */
    d = NULL;
    c.scan_depth = 2;
    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_OK, "depth 2 descends one level");
    ua_driver_destroy(d);
}

/* ---- stability (v2 4.1) --------------------------------------------- */

static void test_stability_needs_two_polls(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;

    env_make(&e, "stability");
    base_config(&c, &e);
    c.stability_wait_seconds = 1;

    copy_into("simple.zip", e.watch, "arriving.zip");

    ua_driver_create(&c, e.state, &d);

    /* The first sighting can never be enough: size and mtime have to be
     * observed *unchanged* over time, and one sample shows no change at all.
     * This is what an mtime-age check alone would get wrong on an FTP server
     * that stamps the final mtime when the transfer starts. */
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "first sighting is never ready");

    ua_plat_sleep_ms(1200);

    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_OK,
              "ready once it has held still");

    ua_driver_destroy(d);
}

static void test_growing_file_resets_the_clock(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    char path[UA_MAX_PATH];
    FILE *fp;

    env_make(&e, "growing");
    base_config(&c, &e);
    c.stability_wait_seconds = 1;

    copy_into("simple.zip", e.watch, "growing.zip");
    snprintf(path, sizeof path, "%s/growing.zip", e.watch);

    ua_driver_create(&c, e.state, &d);

    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND, "first sighting");

    ua_plat_sleep_ms(1200);

    /* Simulating the rest of an upload arriving. */
    fp = fopen(path, "ab");
    if (fp) { fwrite("more data", 1, 9, fp); fclose(fp); }

    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "size changed, so the clock restarts");

    ua_plat_sleep_ms(1200);

    /* Now stable again. The file is a corrupted zip at this point, which is
     * fine: what matters is that the driver offered it to the extractor only
     * after it stopped changing. */
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_OK, "ready after it settles");

    ua_driver_destroy(d);
}

/* ---- queue.txt (v2 4.2) --------------------------------------------- */

static void test_queue_file(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_job job;
    ua_job_report rep;
    char qpath[UA_MAX_PATH], apath[UA_MAX_PATH], dest[UA_MAX_PATH], check[UA_MAX_PATH];
    FILE *fp;

    env_make(&e, "queuefile");
    base_config(&c, &e);

    /* Deliberately outside the watch path: that is what queue.txt is for. */
    snprintf(apath, sizeof apath, "%sdrv_queuefile/elsewhere", TMPROOT);
    snprintf(dest, sizeof dest, "%sdrv_queuefile/explicit-dest", TMPROOT);
    ua_plat_mkdirs(apath);
    copy_into("simple.zip", apath, "manual.zip");

    snprintf(qpath, sizeof qpath, "%s/queue.txt", e.state);
    ua_plat_mkdirs(e.state);
    fp = fopen(qpath, "wb");
    UA_CHECK(fp != NULL, "queue.txt writable");
    if (fp) {
        fprintf(fp, "# a comment line\n\n");
        fprintf(fp, "%s/manual.zip -> %s\n", apath, dest);
        fclose(fp);
    }

    ua_driver_create(&c, e.state, &d);

    UA_EQ_INT(ua_driver_tick(d, &job, &rep), UA_OK, "queued job ran");
    UA_EQ_INT(job.from_queue_file, 1, "job came from queue.txt");
    UA_EQ_INT(rep.result, UA_OK, "extracted: %s", rep.detail);

    /* The explicit destination overrides the in-place default. */
    snprintf(check, sizeof check, "%s/readme.txt", dest);
    UA_CHECK(exists(check), "extracted to the destination named in queue.txt");

    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "queued line not rerun");

    ua_driver_destroy(d);
}

/* ---- FR-6 verified delete ------------------------------------------- */

static void test_delete_after_verified_extraction(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_job_report rep;
    ua_driver_stats s;
    char path[UA_MAX_PATH];

    env_make(&e, "delete_ok");
    base_config(&c, &e);
    c.delete_after_extract = 1;

    copy_into("simple.zip", e.watch, "gone.zip");
    snprintf(path, sizeof path, "%s/gone.zip", e.watch);
    UA_CHECK(exists(path), "archive present before");

    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, NULL, &rep), UA_OK, "job ran");
    UA_EQ_INT(rep.result, UA_OK, "extraction succeeded");

    /* FR-6: entry count, file count and sizes all matched, so the source goes. */
    UA_CHECK(!exists(path), "verified archive deleted");
    ua_driver_get_stats(d, &s);
    UA_EQ_INT(s.archives_deleted, 1, "delete counted");

    ua_driver_destroy(d);
}

static void test_failed_extraction_keeps_source(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_job_report rep;
    ua_driver_stats s;
    char path[UA_MAX_PATH];

    env_make(&e, "delete_fail");
    base_config(&c, &e);
    c.delete_after_extract = 1;

    /* v2 section 9 asks for exactly this case: an archive that fails partway,
     * to verify FR-6 does not delete the source. A deleted archive after a bad
     * extraction is unrecoverable. */
    copy_into("corrupt.zip", e.watch, "broken.zip");
    snprintf(path, sizeof path, "%s/broken.zip", e.watch);

    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, NULL, &rep), UA_OK, "job ran");
    UA_CHECK(rep.result != UA_OK, "extraction failed as expected");

    UA_CHECK(exists(path), "failed archive kept");
    ua_driver_get_stats(d, &s);
    UA_EQ_INT(s.archives_deleted, 0, "nothing deleted");
    UA_EQ_INT(s.jobs_failed, 1, "failure counted");

    /* Recorded despite failing, so it is not retried on every single poll. */
    UA_EQ_INT(ua_driver_tick(d, NULL, NULL), UA_ERR_NOT_FOUND,
              "failed job not retried immediately");

    ua_driver_destroy(d);
}

static void test_refused_archive_keeps_source(void)
{
    test_env e;
    ua_config c;
    ua_driver *d = NULL;
    ua_job_report rep;
    char path[UA_MAX_PATH];

    env_make(&e, "delete_hostile");
    base_config(&c, &e);
    c.delete_after_extract = 1;

    /* Refused at preflight rather than failing mid-way. The source must
     * survive that too, or a hostile archive could delete itself and leave
     * only a log line behind. */
    copy_into("traversal.zip", e.watch, "evil.zip");
    snprintf(path, sizeof path, "%s/evil.zip", e.watch);

    ua_driver_create(&c, e.state, &d);
    UA_EQ_INT(ua_driver_tick(d, NULL, &rep), UA_OK, "job ran");
    UA_EQ_INT(rep.result, UA_ERR_UNSAFE_PATH, "traversal refused");
    UA_CHECK(exists(path), "refused archive kept");

    ua_driver_destroy(d);
}

/* ---- queue.status --------------------------------------------------- */

static void test_status_keying(void)
{
    const char *path = TMPROOT "status_key.txt";
    ua_status *s = NULL;
    ua_result recorded;

    remove(path);

    UA_EQ_INT(ua_status_open(path, &s), UA_OK, "missing file is a fresh start");
    UA_EQ_INT(ua_status_count(s), 0, "no records");

    UA_EQ_INT(ua_status_record(s, "/data/a.zip", 100, 5000, UA_OK), UA_OK, "record");
    UA_CHECK(ua_status_seen(s, "/data/a.zip", 100, 5000), "exact key matches");

    /* v2 4.3: keyed by path + size + mtime, so replacing a file with a new
     * version of the same name is a different job. */
    UA_CHECK(!ua_status_seen(s, "/data/a.zip", 101, 5000), "different size is new");
    UA_CHECK(!ua_status_seen(s, "/data/a.zip", 100, 5001), "different mtime is new");
    UA_CHECK(!ua_status_seen(s, "/data/b.zip", 100, 5000), "different path is new");

    UA_EQ_INT(ua_status_record(s, "/data/b.zip", 7, 1, UA_ERR_CORRUPT), UA_OK, "record failure");
    UA_CHECK(ua_status_result_of(s, "/data/b.zip", 7, 1, &recorded), "outcome stored");
    UA_EQ_INT(recorded, UA_ERR_CORRUPT, "failure outcome preserved");

    ua_status_close(s);

    /* Reopen: the file has to survive a restart, which is the whole point. */
    s = NULL;
    UA_EQ_INT(ua_status_open(path, &s), UA_OK, "reopen");
    UA_EQ_INT(ua_status_count(s), 2, "both records reloaded");
    UA_CHECK(ua_status_seen(s, "/data/a.zip", 100, 5000), "record survived");
    UA_CHECK(ua_status_result_of(s, "/data/b.zip", 7, 1, &recorded), "outcome survived");
    UA_EQ_INT(recorded, UA_ERR_CORRUPT, "and is still the failure");

    ua_status_close(s);
}

static void test_status_is_bounded(void)
{
    const char *path = TMPROOT "status_ring.txt";
    ua_status *s = NULL;
    char name[64];
    int i;

    remove(path);
    ua_status_open(path, &s);

    /* v2 section 7: status files must not grow without limit. */
    for (i = 0; i < UA_STATUS_MAX + 50; i++) {
        snprintf(name, sizeof name, "/data/f%04d.zip", i);
        ua_status_record(s, name, (uint64_t)i, i, UA_OK);
    }

    UA_EQ_INT(ua_status_count(s), UA_STATUS_MAX, "capped at the ring size");

    /* Oldest dropped, newest kept. */
    UA_CHECK(!ua_status_seen(s, "/data/f0000.zip", 0, 0), "oldest aged out");
    snprintf(name, sizeof name, "/data/f%04d.zip", UA_STATUS_MAX + 49);
    UA_CHECK(ua_status_seen(s, name, UA_STATUS_MAX + 49, UA_STATUS_MAX + 49),
             "newest retained");

    ua_status_close(s);
}

static void test_status_tolerates_corruption(void)
{
    const char *path = TMPROOT "status_bad.txt";
    ua_status *s = NULL;
    FILE *fp = fopen(path, "wb");

    /* Refusing to start because the status file is damaged would strand the
     * payload, so bad lines are skipped and the good ones still load. */
    if (fp) {
        fprintf(fp, "# ps5-unarchiver queue.status v1\n");
        fprintf(fp, "0\t100\t5000\t1700000000\t/data/good.zip\n");
        fprintf(fp, "this line is nonsense\n");
        fprintf(fp, "0\t100\n");
        fprintf(fp, "\n");
        fprintf(fp, "5\t9\t9\t9\t/data/other.zip\n");
        fclose(fp);
    }

    UA_EQ_INT(ua_status_open(path, &s), UA_OK, "loads despite damage");
    UA_EQ_INT(ua_status_count(s), 2, "kept the two valid records");
    UA_CHECK(ua_status_seen(s, "/data/good.zip", 100, 5000), "valid record read");

    ua_status_close(s);
}

void test_driver_all(void)
{
    printf("test_driver\n");
    test_extracts_in_place();
    test_refuses_second_instance();
    test_survives_restart();
    test_ignores_non_archives();
    test_detects_by_content_not_name();
    test_one_job_per_tick();
    test_scan_depth();
    test_stability_needs_two_polls();
    test_growing_file_resets_the_clock();
    test_queue_file();
    test_delete_after_verified_extraction();
    test_failed_extraction_keeps_source();
    test_refused_archive_keeps_source();
    test_status_keying();
    test_status_is_bounded();
    test_status_tolerates_corruption();
}
