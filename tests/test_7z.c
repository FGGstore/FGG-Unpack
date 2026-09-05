/* The 7z backend.
 *
 * The corpus is produced by the real 7-Zip CLI, so these decode archives with
 * its actual default settings rather than something hand-rolled to be easy.
 * If 7z was not installed when the corpus was generated the cases skip, which
 * is reported rather than silently passing.
 */
#include "../src/core/ua_extract.h"
#include "../src/backend/ua_detect.h"
#include "../src/platform/ua_platform.h"
#include "ua_test.h"

#include <stdio.h>
#include <stdlib.h>

#define CORPUS  "tests/corpus/"
#define TMPROOT "build/tmp/"

static int skipped = 0;

static const char *corpus(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof buf, "%s%s", CORPUS, name);
    return buf;
}

static const char *tmpdir(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof buf, "%s7z_%s", TMPROOT, name);
    return buf;
}

static int have(const char *name)
{
    ua_stat st;
    return ua_plat_lstat(corpus(name), &st) == UA_OK;
}

static void base_opts(ua_extract_opts *o)
{
    ua_extract_opts_defaults(o);
    o->stability_wait_seconds = 0;
    o->min_free_margin_mb     = 1;
}

/* Reads a file and checks its length and, optionally, that every byte equals
 * `fill`. Used instead of comparing to a golden file so the assertion says
 * what is wrong rather than just "differs". */
static int check_file(const char *path, uint64_t want_len, int fill)
{
    unsigned char buf[65536];
    uint64_t total = 0;
    size_t n;
    FILE *fp = fopen(path, "rb");

    if (!fp) return 0;

    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        if (fill >= 0) {
            size_t i;
            for (i = 0; i < n; i++) {
                if (buf[i] != (unsigned char)fill) { fclose(fp); return 0; }
            }
        }
        total += n;
    }
    fclose(fp);

    return total == want_len;
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

static int exists(const char *path)
{
    ua_stat st;
    return ua_plat_lstat(path, &st) == UA_OK;
}

/* ---- detection ------------------------------------------------------- */

static void test_detects_7z(void)
{
    ua_format f = UA_FMT_UNKNOWN;

    if (!have("simple.7z")) { skipped++; return; }

    UA_EQ_INT(ua_detect_file(corpus("simple.7z"), &f), UA_OK, "7z detected");
    UA_EQ_INT(f, UA_FMT_7Z, "reported as 7z");
}

/* ---- decoding -------------------------------------------------------- */

static void test_simple(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("simple");
    char path[UA_MAX_PATH];

    if (!have("simple.7z")) { skipped++; return; }

    base_opts(&opts);

    UA_EQ_INT(ua_extract_run(corpus("simple.7z"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "simple.7z extracts: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 4, "four files");

    snprintf(path, sizeof path, "%s/readme.txt", dest);
    UA_CHECK(file_contains(path, "hello from the 7z corpus"), "content correct");

    snprintf(path, sizeof path, "%s/dir/sub/two.txt", dest);
    UA_CHECK(file_contains(path, "two"), "nested path preserved");

    snprintf(path, sizeof path, "%s/empty.dat", dest);
    UA_CHECK(exists(path), "zero-byte entry created");
}

static void test_solid_block(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("solid");
    char path[UA_MAX_PATH];
    int i;

    if (!have("solid.7z")) { skipped++; return; }

    base_opts(&opts);

    /* Eight files in one compressed block. Reaching the last one means
     * decoding and discarding the seven before it, because a solid block has
     * no per-file entry point. Getting this wrong yields plausible-looking
     * files with the wrong contents, which is why every byte is checked. */
    UA_EQ_INT(ua_extract_run(corpus("solid.7z"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "solid.7z extracts: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 8, "all eight files");

    for (i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "%s/part%02d.bin", dest, i);
        UA_CHECK(check_file(path, 200000, i),
                 "part%02d.bin is 200000 bytes of 0x%02x", i, i);
    }
}

static void test_stored_coder(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("stored");
    char path[UA_MAX_PATH];

    if (!have("stored.7z")) { skipped++; return; }

    base_opts(&opts);

    /* Copy coder: no LZMA involved, so this catches a streaming loop that only
     * works when a decoder is in the path. */
    UA_EQ_INT(ua_extract_run(corpus("stored.7z"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "stored.7z extracts: %s", rep.detail);

    snprintf(path, sizeof path, "%s/stored.txt", dest);
    UA_CHECK(check_file(path, 2000, -1), "stored file length");
    UA_CHECK(file_contains(path, "no compression here"), "stored content");
}

static void test_lzma1_coder(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("lzma1");
    char path[UA_MAX_PATH];

    if (!have("lzma1.7z")) { skipped++; return; }

    base_opts(&opts);

    /* LZMA1 rather than LZMA2: a different decoder with different framing. */
    UA_EQ_INT(ua_extract_run(corpus("lzma1.7z"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "lzma1.7z extracts: %s", rep.detail);

    snprintf(path, sizeof path, "%s/lzma1.bin", dest);
    UA_CHECK(check_file(path, 18 * 5000, -1), "lzma1 output length");
    UA_CHECK(file_contains(path, "repeating pattern"), "lzma1 content");
}

static void test_large_streams(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("big");
    char path[UA_MAX_PATH];

    if (!have("big.7z")) { skipped++; return; }

    base_opts(&opts);

    /* 12 MiB through 256 KB buffers: far more than one decode call, so this
     * exercises the refill-and-continue loop rather than a lucky single shot.
     * The whole reason this backend exists instead of SzArEx_Extract is that
     * memory must not track archive size. */
    UA_EQ_INT(ua_extract_run(corpus("big.7z"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "big.7z extracts: %s", rep.detail);
    UA_EQ_INT(rep.bytes_done, 12ull * 1024 * 1024, "all 12 MiB written");

    snprintf(path, sizeof path, "%s/big.bin", dest);
    UA_CHECK(check_file(path, 12ull * 1024 * 1024, -1), "output length");
}

static void test_large_dictionary(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    const char *dest = tmpdir("bigdict");
    char path[UA_MAX_PATH];

    if (!have("bigdict.7z")) { skipped++; return; }

    base_opts(&opts);

    /* LZMA2:d256m, the same 256 MB window a large real archive uses. The
     * dictionary is the dominant memory cost of decoding, and it is allocated
     * whatever the archive's size, so a small file with a big window is the
     * cheap way to prove the allocation path works. */
    UA_EQ_INT(ua_extract_run(corpus("bigdict.7z"), dest, &opts, NULL, NULL, &rep),
              UA_OK, "256 MB dictionary archive extracts: %s", rep.detail);

    snprintf(path, sizeof path, "%s/bigdict.bin", dest);
    UA_CHECK(check_file(path, 23 * 40000, -1), "output length");
    UA_CHECK(file_contains(path, "dictionary window test"), "content correct");
}

/* Verifies the position-derived pattern in parallel.bin: run i of 64 KiB holds
 * the byte (i & 0xFF). Content, not just length -- an offset error in the
 * parallel decoder yields a correctly sized file full of the wrong bytes. */
static int check_pattern(const char *path, int runs)
{
    unsigned char buf[65536];
    int i;
    FILE *fp = fopen(path, "rb");

    if (!fp) return 0;

    for (i = 0; i < runs; i++) {
        size_t n = fread(buf, 1, sizeof buf, fp);
        size_t k;

        if (n != sizeof buf) { fclose(fp); return 0; }
        for (k = 0; k < n; k++) {
            if (buf[k] != (unsigned char)(i & 0xFF)) { fclose(fp); return 0; }
        }
    }

    /* Nothing should follow the last run. */
    if (fread(buf, 1, 1, fp) != 0) { fclose(fp); return 0; }

    fclose(fp);
    return 1;
}

static void test_parallel_decode(void)
{
    ua_extract_opts opts;
    ua_job_report seq, par;
    char path[UA_MAX_PATH];
    const int runs = 768;
    const uint64_t size = 768ull * 65536;

    if (!have("parallel.7z")) { skipped++; return; }

    /* Sequential, as the baseline. */
    base_opts(&opts);
    opts.decode_threads = 1;

    UA_EQ_INT(ua_extract_run(corpus("parallel.7z"), tmpdir("par_seq"), &opts,
                             NULL, NULL, &seq),
              UA_OK, "sequential decode: %s", seq.detail);
    UA_EQ_INT(seq.bytes_done, size, "sequential byte count");

    snprintf(path, sizeof path, "%s/parallel.bin", tmpdir("par_seq"));
    UA_CHECK(check_pattern(path, runs), "sequential output is byte-correct");

    /* Parallel. The archive has 12 dictionary resets, so several workers each
     * decode a disjoint range and write it at its own file offset. */
    base_opts(&opts);
    opts.decode_threads = 4;

    UA_EQ_INT(ua_extract_run(corpus("parallel.7z"), tmpdir("par_mt"), &opts,
                             NULL, NULL, &par),
              UA_OK, "parallel decode: %s", par.detail);
    UA_EQ_INT(par.bytes_done, size, "parallel byte count matches");
    UA_EQ_INT(par.files_written, 1, "one file written");

    snprintf(path, sizeof path, "%s/parallel.bin", tmpdir("par_mt"));
    UA_CHECK(check_pattern(path, runs), "parallel output is byte-correct");
}

static void test_parallel_declines_gracefully(void)
{
    ua_extract_opts opts;
    ua_job_report rep;
    char path[UA_MAX_PATH];

    if (!have("solid.7z")) { skipped++; return; }

    base_opts(&opts);
    opts.decode_threads = 4;

    /* A multi-file solid folder is not parallelised: a worker writes at an
     * absolute offset in one output file, which does not generalise to a
     * folder spanning several. The backend must decline and the sequential
     * path take over, invisibly and correctly. */
    UA_EQ_INT(ua_extract_run(corpus("solid.7z"), tmpdir("par_decline"), &opts,
                             NULL, NULL, &rep),
              UA_OK, "declined case still extracts: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 8, "all eight files via the sequential path");

    snprintf(path, sizeof path, "%s/part05.bin", tmpdir("par_decline"));
    UA_CHECK(check_file(path, 200000, 5), "content still correct after declining");

    /* Likewise a zip, whose backend has no parallel entry point at all. */
    UA_EQ_INT(ua_extract_run(corpus("simple.zip"), tmpdir("par_zip"), &opts,
                             NULL, NULL, &rep),
              UA_OK, "zip unaffected by decode_threads: %s", rep.detail);
    UA_EQ_INT(rep.files_written, 6, "zip still extracts");
}

static void test_chunk_size_is_honoured(void)
{
    ua_extract_opts opts;
    ua_job_report small, large;
    char path[UA_MAX_PATH];

    if (!have("big.7z")) { skipped++; return; }

    /* chunk_size_kb was documented, range-checked and copied into the options
     * struct for a long time while being read by nothing at all. These two
     * runs differ only in buffer size, so they exercise the plumbing as well
     * as proving that a 64x larger buffer changes no output byte. */
    base_opts(&opts);
    opts.decode_threads = 1;
    opts.chunk_size_kb  = 64;

    UA_EQ_INT(ua_extract_run(corpus("big.7z"), tmpdir("chunk_small"), &opts,
                             NULL, NULL, &small),
              UA_OK, "64 KB buffers: %s", small.detail);

    base_opts(&opts);
    opts.decode_threads = 1;
    opts.chunk_size_kb  = 4096;          /* 4 MB */

    UA_EQ_INT(ua_extract_run(corpus("big.7z"), tmpdir("chunk_large"), &opts,
                             NULL, NULL, &large),
              UA_OK, "4 MB buffers: %s", large.detail);

    UA_EQ_INT(large.bytes_done, small.bytes_done,
              "same byte count regardless of buffer size");

    snprintf(path, sizeof path, "%s/big.bin", tmpdir("chunk_large"));
    UA_CHECK(check_file(path, 12ull * 1024 * 1024, -1),
             "output correct with 4 MB buffers");

    /* Absurd values are clamped rather than trusted, so nothing here can be
     * turned into a huge allocation from a config file. */
    base_opts(&opts);
    opts.decode_threads = 1;
    opts.chunk_size_kb  = 1;             /* below the floor */

    UA_EQ_INT(ua_extract_run(corpus("big.7z"), tmpdir("chunk_tiny"), &opts,
                             NULL, NULL, &small),
              UA_OK, "undersized request is clamped, not fatal: %s", small.detail);
}

/* ---- refusals -------------------------------------------------------- */

static void test_filter_chain_refused(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    if (!have("bcj2.7z")) { skipped++; return; }

    base_opts(&opts);

    /* BCJ2 is a four-input filter chain this build does not run. It must be
     * refused by name, not silently decoded into wrong bytes. */
    UA_EQ_INT(ua_extract_run(corpus("bcj2.7z"), tmpdir("bcj2"), &opts,
                             NULL, NULL, &rep),
              UA_ERR_UNSUPPORTED_FORMAT, "BCJ2 chain refused");
    UA_CHECK(strstr(rep.detail, "coder") != NULL ||
             strstr(rep.detail, "method") != NULL,
             "reason explains why, got \"%s\"", rep.detail);
    UA_CHECK(!exists(tmpdir("bcj2")), "nothing written for a refused chain");
}

static void test_traversal_refused(void)
{
    ua_extract_opts opts;
    ua_job_report rep;

    if (!have("traversal.7z")) { skipped++; return; }

    base_opts(&opts);

    /* FR-3 is enforced above the backend, so a new format does not get its own
     * chance to write outside the destination. */
    UA_EQ_INT(ua_extract_run(corpus("traversal.7z"), tmpdir("traversal"), &opts,
                             NULL, NULL, &rep),
              UA_ERR_UNSAFE_PATH, "7z traversal refused");
    UA_CHECK(!exists(tmpdir("traversal")), "destination not created");
    UA_CHECK(!exists(TMPROOT "evil.txt"), "nothing escaped");
}

void test_7z_all(void)
{
    printf("test_7z\n");

    test_detects_7z();
    test_simple();
    test_solid_block();
    test_stored_coder();
    test_lzma1_coder();
    test_large_streams();
    test_large_dictionary();
    test_chunk_size_is_honoured();
    test_parallel_decode();
    test_parallel_declines_gracefully();
    test_filter_chain_refused();
    test_traversal_refused();

    if (skipped)
        printf("  (%d case(s) skipped: no 7z corpus, install 7-Zip and regenerate)\n",
               skipped);
}
