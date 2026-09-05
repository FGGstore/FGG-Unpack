/* config.ini parsing (v2 4.4) and the queue.txt line grammar (v2 4.2). */
#include "../src/core/ua_config.h"
#include "../src/core/ua_queue.h"
#include "ua_test.h"

#include <stdio.h>
#include <stdlib.h>

#define TMPROOT "build/tmp/"

static void write_file(const char *path, const char *body)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { printf("  FAIL cannot write %s\n", path); ua_tests_failed++; return; }
    fputs(body, fp);
    fclose(fp);
}

static void test_defaults(void)
{
    ua_config c;

    ua_config_defaults(&c);

    /* Straight from the v2 4.4 table. */
    UA_EQ_INT(c.debug, 1, "debug");
    UA_EQ_INT(c.scan_depth, 1, "scan_depth");
    UA_EQ_INT(c.poll_interval_seconds, 10, "poll_interval_seconds");
    UA_EQ_INT(c.stability_wait_seconds, 15, "stability_wait_seconds");
    UA_EQ_INT(c.delete_after_extract, 0, "delete_after_extract defaults off");
    UA_EQ_INT(c.notify, 1, "notify");
    UA_EQ_INT(c.overwrite, 0, "overwrite");
    UA_EQ_INT(c.min_free_margin_mb, 1024, "min_free_margin_mb");
    UA_EQ_INT(c.chunk_size_kb, 256, "chunk_size_kb");
    UA_EQ_INT(c.http_port, 9022, "http_port");

    UA_EQ_INT(c.n_watchpath, 1, "one default watch path");
    UA_EQ_STR(c.watchpath[0], "/data/homebrew");
}

static void test_set_values(void)
{
    ua_config c;

    ua_config_defaults(&c);

    UA_EQ_INT(ua_config_set(&c, "overwrite", "1"), UA_OK, "numeric bool");
    UA_EQ_INT(c.overwrite, 1, "overwrite set");

    /* People write these, so they are accepted. */
    UA_EQ_INT(ua_config_set(&c, "notify", "false"), UA_OK, "word bool");
    UA_EQ_INT(c.notify, 0, "notify cleared");
    UA_EQ_INT(ua_config_set(&c, "debug", "yes"), UA_OK, "yes");
    UA_EQ_INT(c.debug, 1, "debug set");

    UA_EQ_INT(ua_config_set(&c, "poll_interval_seconds", "30"), UA_OK, "uint");
    UA_EQ_INT(c.poll_interval_seconds, 30, "poll interval set");

    /* Documented ranges are enforced, and a rejected value leaves the previous
     * one in place rather than zeroing it. */
    UA_EQ_INT(ua_config_set(&c, "poll_interval_seconds", "0"), UA_ERR_BADARG, "below range");
    UA_EQ_INT(ua_config_set(&c, "poll_interval_seconds", "3601"), UA_ERR_BADARG, "above range");
    UA_EQ_INT(c.poll_interval_seconds, 30, "kept previous value");

    UA_EQ_INT(ua_config_set(&c, "scan_depth", "3"), UA_ERR_BADARG, "scan_depth max 2");
    UA_EQ_INT(ua_config_set(&c, "scan_depth", "2"), UA_OK, "scan_depth 2");

    UA_EQ_INT(ua_config_set(&c, "poll_interval_seconds", "12x"), UA_ERR_BADARG, "trailing junk");
    UA_EQ_INT(ua_config_set(&c, "poll_interval_seconds", ""), UA_ERR_BADARG, "empty");
    UA_EQ_INT(ua_config_set(&c, "nonsense", "1"), UA_ERR_NOT_FOUND, "unknown key");
}

static void test_watchpath_replaces_default(void)
{
    ua_config c;

    ua_config_defaults(&c);

    /* The first configured path displaces the built-in default: a user who
     * names their own paths should not silently keep scanning ours too. */
    UA_EQ_INT(ua_config_set(&c, "watchpath", "/mnt/usb0/incoming"), UA_OK, "first");
    UA_EQ_INT(c.n_watchpath, 1, "replaced, not appended");
    UA_EQ_STR(c.watchpath[0], "/mnt/usb0/incoming");

    UA_EQ_INT(ua_config_set(&c, "watchpath", "/data/homebrew"), UA_OK, "second");
    UA_EQ_INT(c.n_watchpath, 2, "appended");
    UA_EQ_STR(c.watchpath[1], "/data/homebrew");
}

static void test_load_file(void)
{
    const char *path = TMPROOT "cfg_load.ini";
    ua_config c;
    int created = -1;

    write_file(path,
        "# a comment\n"
        "; another comment\n"
        "[general]\n"
        "\n"
        "  overwrite = 1  \n"
        "delete_after_extract=1\n"
        "watchpath=/mnt/usb0/in\n"
        "watchpath=/data/hb\n"
        "poll_interval_seconds=42\n"
        "garbage line with no equals\n"
        "unknown_key=7\n"
        "scan_depth=99\n");

    UA_EQ_INT(ua_config_load(path, &c, &created), UA_OK, "load succeeds");
    UA_EQ_INT(created, 0, "existing file not recreated");

    UA_EQ_INT(c.overwrite, 1, "whitespace around key and value tolerated");
    UA_EQ_INT(c.delete_after_extract, 1, "delete_after_extract");
    UA_EQ_INT(c.poll_interval_seconds, 42, "poll interval");
    UA_EQ_INT(c.n_watchpath, 2, "both watch paths");
    UA_EQ_STR(c.watchpath[0], "/mnt/usb0/in");

    /* A bad line must not prevent startup (v2 section 7): the rest of the file
     * still applies and the out-of-range value keeps its default. */
    UA_EQ_INT(c.scan_depth, 1, "out-of-range value keeps default");
    UA_EQ_INT(c.notify, 1, "untouched key keeps default");
}

static void test_missing_file_writes_template(void)
{
    const char *path = TMPROOT "cfg_new.ini";
    ua_config c, reloaded;
    int created = -1;

    remove(path);

    UA_EQ_INT(ua_config_load(path, &c, &created), UA_OK, "missing file is not an error");
    UA_EQ_INT(created, 1, "template written on first run");

    /* The template must reload to exactly the defaults, or the file would
     * quietly disagree with the code it documents. */
    created = -1;
    UA_EQ_INT(ua_config_load(path, &reloaded, &created), UA_OK, "template reloads");
    UA_EQ_INT(created, 0, "second load does not rewrite");

    UA_EQ_INT(reloaded.poll_interval_seconds, c.poll_interval_seconds, "poll interval round-trips");
    UA_EQ_INT(reloaded.stability_wait_seconds, c.stability_wait_seconds, "stability round-trips");
    UA_EQ_INT(reloaded.delete_after_extract, c.delete_after_extract, "delete flag round-trips");
    UA_EQ_INT(reloaded.min_free_margin_mb, c.min_free_margin_mb, "margin round-trips");
    UA_EQ_INT(reloaded.http_port, c.http_port, "http_port round-trips");
    UA_EQ_INT(reloaded.n_watchpath, 1, "one watch path");
    UA_EQ_STR(reloaded.watchpath[0], c.watchpath[0]);
}

/* ---- queue.txt grammar ---------------------------------------------- */

static void test_queue_lines(void)
{
    char a[UA_MAX_PATH], d[UA_MAX_PATH];

    UA_EQ_INT(ua_queue_parse_line("/data/x.zip", a, sizeof a, d, sizeof d),
              UA_OK, "bare path");
    UA_EQ_STR(a, "/data/x.zip");
    UA_EQ_STR(d, "");

    UA_EQ_INT(ua_queue_parse_line("/mnt/usb0/p.zip -> /data/hb/\n",
                                  a, sizeof a, d, sizeof d),
              UA_OK, "with destination");
    UA_EQ_STR(a, "/mnt/usb0/p.zip");
    UA_EQ_STR(d, "/data/hb/");

    /* The example straight out of the requirements. */
    UA_EQ_INT(ua_queue_parse_line("/data/backups/configs.tar.gz\r\n",
                                  a, sizeof a, d, sizeof d),
              UA_OK, "crlf tolerated");
    UA_EQ_STR(a, "/data/backups/configs.tar.gz");

    UA_EQ_INT(ua_queue_parse_line("", a, sizeof a, d, sizeof d),
              UA_ERR_NOT_FOUND, "empty line");
    UA_EQ_INT(ua_queue_parse_line("   \t \n", a, sizeof a, d, sizeof d),
              UA_ERR_NOT_FOUND, "whitespace only");
    UA_EQ_INT(ua_queue_parse_line("# /mnt/usb0/disabled.zip\n", a, sizeof a, d, sizeof d),
              UA_ERR_NOT_FOUND, "comment");
    UA_EQ_INT(ua_queue_parse_line("   # indented comment\n", a, sizeof a, d, sizeof d),
              UA_ERR_NOT_FOUND, "indented comment");

    UA_EQ_INT(ua_queue_parse_line("/data/x.zip ->\n", a, sizeof a, d, sizeof d),
              UA_ERR_BADARG, "arrow with no destination");
    UA_EQ_INT(ua_queue_parse_line("-> /data/dest\n", a, sizeof a, d, sizeof d),
              UA_ERR_BADARG, "arrow with no archive");

    /* Only the first arrow separates, so a destination containing "->" works. */
    UA_EQ_INT(ua_queue_parse_line("/a.zip -> /data/we->ird", a, sizeof a, d, sizeof d),
              UA_OK, "arrow inside destination");
    UA_EQ_STR(a, "/a.zip");
    UA_EQ_STR(d, "/data/we->ird");
}

void test_config_all(void)
{
    printf("test_config\n");
    test_defaults();
    test_set_values();
    test_watchpath_replaces_default();
    test_load_file();
    test_missing_file_writes_template();
    test_queue_lines();
}
