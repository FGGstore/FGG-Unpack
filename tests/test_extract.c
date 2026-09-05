/* End-to-end extraction against the generated corpus (Section 8).
 *
 * Run from the repository root: paths are relative so the same test binary
 * works on the console, where the corpus would be copied to /data.
 */
#include "../src/core/ua_extract.h"
#include "../src/platform/ua_platform.h"
#include "ua_test.h"

#include <stdio.h>
#include <stdlib.h>

#define CORPUS  "tests/corpus/"
#define TMPROOT "build/tmp/"

/* The corpus is generated immediately before the tests run, so every archive
 * is newer than any sane stability window. Tests that are not about stability
 * disable the wait; test_stability_gate re-enables it deliberately. */
static void base_opts(ua_extract_opts *o)
{
    ua_extract_opts_defaults(o);
    o->stability_wait_seconds = 0;
    o->min_free_margin_mb     = 1;   /* the host disk is not the thing under test */
}

static const char *corpus(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof buf, "%s%s", CORPUS, name);
    return buf;
}

static const char *tmpdir(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof buf, "%s%s", TMPROOT, name);
    return buf;
}

static int exists(const char *path)
{
    ua_stat st;
    return ua_plat_lstat(path, &st) == UA_OK;
}

static int file_contains(const char *path, const char *want)
{
    char buf[512];
    size_t n;
    FILE *fp = fopen(path, "rb");

    if (!fp) return 0;
    n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    return strstr(buf, want) != NULL;
}

/* ---- happy paths ---------------------------------------------------- */

static void test_simple_archive(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("simple");
    char path[512];

    base_opts(&opts);

    UA_EQ_INT(ua_extract_run(corpus("simple.zip"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "simple.zip extracts: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 6, "file count");
    UA_EQ_INT(rep.format, UA_FMT_ZIP, "format recorded");
    UA_EQ_INT(rep.percent, 100, "finished at 100 percent");

    /* FR-5: structure preserved, including the empty file and the deep path. */
    snprintf(path, sizeof path, "%s/readme.txt", dest);
    UA_CHECK(file_contains(path, "hello from the corpus"), "readme content");

    snprintf(path, sizeof path, "%s/dir/sub/deeper/three.bin", dest);
    UA_CHECK(exists(path), "deep path created");

    snprintf(path, sizeof path, "%s/empty.dat", dest);
    UA_CHECK(exists(path), "zero-byte file created");

    snprintf(path, sizeof path, "%s/dir/sub/two.txt", dest);
    UA_CHECK(file_contains(path, "two"), "nested file content");
}

static void test_unicode_names(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("unicode");
    char path[512];

    base_opts(&opts);

    /* Names are byte strings. The console stores them verbatim, so the harness
     * has to as well, or it is not testing the same thing (v2 section 9 asks
     * for this to be verified end to end).
     *
     * Names are written as explicit byte escapes rather than literals so the
     * assertion does not depend on the encoding this source file is saved in.
     *
     * Counting files is not enough: an earlier version of this test checked
     * only files_written and passed while every non-ASCII name was being
     * stored double-encoded. */
    UA_EQ_INT(ua_extract_run(corpus("unicode.zip"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "unicode.zip extracts: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 4, "all four names written");

    snprintf(path, sizeof path, "%s/hello.txt", dest);
    UA_CHECK(exists(path), "ascii name");

    /* café/naïve.txt */
    snprintf(path, sizeof path, "%s/caf\xc3\xa9/na\xc3\xafve.txt", dest);
    UA_CHECK(exists(path), "latin-1 range name survived as UTF-8");

    /* 日本語/テスト.txt */
    snprintf(path, sizeof path,
             "%s/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e/\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88.txt",
             dest);
    UA_CHECK(exists(path), "three-byte sequences survived");

    /* emoji-U+1F600.txt, four-byte sequence: a surrogate pair once the host
     * converts it, and the case most likely to be truncated. */
    snprintf(path, sizeof path, "%s/emoji-\xf0\x9f\x98\x80.txt", dest);
    UA_CHECK(exists(path), "four-byte sequence survived");

    /* The double-encoded form UTF-8 bytes decay into when a byte string is
     * read through a single-byte codepage. Its absence is the regression. */
    snprintf(path, sizeof path, "%s/caf\xc3\x83\xc2\xa9", dest);
    UA_CHECK(!exists(path), "name was stored double-encoded");
}

static void test_large_entry_streams(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    base_opts(&opts);

    /* FR-4: a 48 MB entry with a 256 KB buffer. If this ever buffers the whole
     * entry, it still passes here but the console would run out of memory, so
     * the real assertion is that chunk_size_kb stays the only bound. */
    UA_EQ_INT(ua_extract_run(corpus("big.zip"), tmpdir("big"), &opts,
                             NULL, NULL, &rep),
              UA_OK, "big.zip extracts: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 1, "one entry");
    UA_EQ_INT(rep.bytes_done, 48ull * 1024 * 1024, "all bytes written");
}

struct progress_count { unsigned calls; unsigned last_pct; int monotonic; };

static void count_progress(void *ud, const ua_job_report *rep)
{
    struct progress_count *c = (struct progress_count *)ud;

    if (rep->percent < c->last_pct) c->monotonic = 0;
    c->last_pct = rep->percent;
    c->calls++;
}

static void test_many_entries_and_progress(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    struct progress_count c = { 0, 0, 1 };

    base_opts(&opts);
    opts.progress_percent_step = 10;

    UA_EQ_INT(ua_extract_run(corpus("many.zip"), tmpdir("many"), &opts,
                             count_progress, &c, &rep),
              UA_OK, "many.zip extracts: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 20000, "20000 entries written");

    /* v2 4.6: rate-limited by percent, not per entry. Twenty thousand files must
     * not produce twenty thousand notifications. */
    UA_CHECK(c.calls <= 12, "progress fired %u times, expected at most 12", c.calls);
    UA_CHECK(c.calls >= 5, "progress fired only %u times", c.calls);
    UA_CHECK(c.monotonic, "progress percentage went backwards");
}

/* ---- FR-3: hostile archives ----------------------------------------- */

static void expect_refused(const char *archive, const char *dest_name,
                           ua_result want, const char *what)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir(dest_name);

    base_opts(&opts);

    UA_EQ_INT(ua_extract_run(corpus(archive), dest, &opts, NULL, NULL, &rep),
              want, "%s", what);

    /* FR-2: no partial extraction on a failed preflight. The destination is
     * created only after every check passes, so its absence is the assertion. */
    UA_CHECK(!exists(dest), "%s: destination was created anyway", what);
    UA_CHECK(rep.detail[0] != '\0', "%s: no reason recorded", what);
}

static void test_rejects_hostile_archives(void)
{
    expect_refused("traversal.zip", "traversal", UA_ERR_UNSAFE_PATH,
                   "../ traversal");
    expect_refused("traversal_deep.zip", "traversal_deep", UA_ERR_UNSAFE_PATH,
                   "traversal behind 20 innocent entries");
    expect_refused("absolute.zip", "absolute", UA_ERR_UNSAFE_PATH,
                   "absolute entry path");
    expect_refused("backslash.zip", "backslash", UA_ERR_UNSAFE_PATH,
                   "backslash traversal");
    expect_refused("symlink_escape.zip", "symlink_escape", UA_ERR_UNSAFE_PATH,
                   "symlink escaping the destination");
    expect_refused("longname.zip", "longname", UA_ERR_LIMIT,
                   "entry name over the path limit");
    expect_refused("deep.zip", "deep", UA_ERR_LIMIT,
                   "entry nested past the component limit");

    /* Legal on its own, too long once joined under the destination. Preflight
     * has to reject it, not the extraction loop: the archive also contains an
     * innocent entry that would otherwise be written first. */
    expect_refused("nearlimit.zip", "nearlimit", UA_ERR_LIMIT,
                   "entry that only overflows once joined to the destination");

    /* The escaped file must not exist anywhere near the destination root. */
    UA_CHECK(!exists(TMPROOT "escaped.txt"), "traversal wrote outside dest");
    UA_CHECK(!exists("build/escaped.txt"), "traversal wrote two levels up");
}

static void test_safe_symlink_is_skipped_by_default(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    base_opts(&opts);

    /* A symlink that stays inside the destination is still skipped unless
     * explicitly allowed: it is a way to redirect entries extracted after it,
     * and nothing on the console needs one. */
    UA_EQ_INT(ua_extract_run(corpus("symlink_safe.zip"), tmpdir("symlink_safe"),
                             &opts, NULL, NULL, &rep),
              UA_OK, "safe symlink archive extracts: %s", rep.detail);
    UA_EQ_INT(rep.symlinks_skipped, 1, "symlink skipped");
    UA_EQ_INT(rep.files_written, 1, "regular file still written");
}

/* ---- FR-2 / FR-7: refusals and failures ----------------------------- */

static void test_format_refusals(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    base_opts(&opts);

    UA_EQ_INT(ua_extract_run(corpus("notarchive.bin"), tmpdir("notarchive"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_UNSUPPORTED_FORMAT, "plain text refused");

    UA_EQ_INT(ua_extract_run(corpus("empty.bin"), tmpdir("emptysrc"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_UNSUPPORTED_FORMAT, "zero-byte source refused");

    UA_EQ_INT(ua_extract_run(corpus("plain.tar"), tmpdir("plaintar"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_UNSUPPORTED_FORMAT, "tar detected but not implemented");
    UA_EQ_INT(rep.format, UA_FMT_TAR, "format still reported");

    UA_EQ_INT(ua_extract_run(corpus("missing.zip"), tmpdir("missing"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_NOT_FOUND, "missing source");
}

static void test_broken_archives(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    base_opts(&opts);

    UA_EQ_INT(ua_extract_run(corpus("truncated.zip"), tmpdir("truncated"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_CORRUPT, "truncated zip");
    UA_CHECK(rep.detail[0] != '\0', "truncated zip recorded no reason");

    /* Scrambled compressed data: the central directory still parses, so this
     * fails during extraction and must be reported with the entry that broke
     * (FR-7) rather than as a generic error. */
    UA_CHECK(ua_extract_run(corpus("corrupt.zip"), tmpdir("corrupt"),
                            &opts, NULL, NULL, &rep) != UA_OK,
             "corrupt body should fail");
    UA_CHECK(rep.detail[0] != '\0', "corrupt zip recorded no reason");
}

static void test_bomb_guard(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    base_opts(&opts);

    /* 256 KB declaring 256 MB. The free-space check in FR-2 passes this
     * happily on a console with a spare gigabyte, which is exactly why the
     * ratio guard exists. */
    UA_EQ_INT(ua_extract_run(corpus("bomb.zip"), tmpdir("bomb"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_LIMIT, "bomb refused: %s", rep.detail);
    UA_CHECK(!exists(tmpdir("bomb")), "bomb created its destination");

    /* Disabling the guard makes it a plain, very compressible archive again,
     * so the guard is what refused it and not something incidental. */
    opts.max_expansion_ratio = 0;
    UA_EQ_INT(ua_extract_preflight(corpus("bomb.zip"), tmpdir("bomb2"),
                                   &opts, &rep),
              UA_OK, "bomb passes preflight with the guard off");
    UA_EQ_INT(rep.bytes_total, 256ull * 1024 * 1024, "declared size read");
}

static void test_free_space_check(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    base_opts(&opts);
    opts.min_free_margin_mb = 1024ull * 1024ull * 8ull;   /* 8 TB of margin */

    UA_EQ_INT(ua_extract_run(corpus("simple.zip"), tmpdir("nospace"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_NO_SPACE, "margin larger than the disk");
    UA_CHECK(!exists(tmpdir("nospace")), "destination created despite refusal");
    UA_CHECK(rep.required > rep.free_before, "required/free not recorded");
}

static void test_stability_gate(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    base_opts(&opts);
    opts.stability_wait_seconds = 3600;

    /* The corpus was generated moments ago, so an hour-long quiet window
     * cannot have elapsed. This is the FR-2 step 2 guard against reading a
     * file still arriving over FTP. */
    UA_EQ_INT(ua_extract_run(corpus("simple.zip"), tmpdir("unstable"),
                             &opts, NULL, NULL, &rep),
              UA_ERR_NOT_STABLE, "recently written archive deferred");
    UA_CHECK(!exists(tmpdir("unstable")), "destination created despite deferral");
}

static void test_overwrite_policy(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("overwrite");

    base_opts(&opts);

    UA_EQ_INT(ua_extract_run(corpus("simple.zip"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "first extraction");

    UA_EQ_INT(ua_extract_run(corpus("simple.zip"), dest, &opts, NULL, NULL, &rep),
              UA_ERR_EXISTS, "second extraction refused with overwrite=0");

    opts.overwrite = 1;
    UA_EQ_INT(ua_extract_run(corpus("simple.zip"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "second extraction allowed with overwrite=1");
    UA_EQ_INT(rep.files_written, 6, "all files rewritten");
}

static void test_preflight_writes_nothing(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("preflight_only");

    base_opts(&opts);

    UA_EQ_INT(ua_extract_preflight(corpus("simple.zip"), dest, &opts, &rep),
              UA_OK, "preflight succeeds: %s", rep.detail);
    UA_EQ_INT(rep.entries_total, 6, "entries counted");
    UA_CHECK(rep.bytes_total > 0, "uncompressed size summed");
    UA_CHECK(!exists(dest), "preflight created the destination");
}

void test_extract_all(void)
{
    printf("test_extract\n");
    test_simple_archive();
    test_unicode_names();
    test_large_entry_streams();
    test_many_entries_and_progress();
    test_rejects_hostile_archives();
    test_safe_symlink_is_skipped_by_default();
    test_format_refusals();
    test_broken_archives();
    test_bomb_guard();
    test_free_space_check();
    test_stability_gate();
    test_overwrite_policy();
    test_preflight_writes_nothing();
}
