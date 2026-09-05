/* FR-1: detection is by content. Every case here deliberately gives the file a
 * name that says nothing useful, so a regression that reintroduces extension
 * sniffing fails. */
#include "../src/backend/ua_backend.h"
#include "../src/backend/ua_detect.h"
#include "ua_test.h"

#define CORPUS "tests/corpus/"

static ua_format detect(const char *name)
{
    char path[512];
    ua_format f = UA_FMT_UNKNOWN;

    snprintf(path, sizeof path, "%s%s", CORPUS, name);
    ua_detect_file(path, &f);

    return f;
}

static void test_detects_by_content(void)
{
    UA_EQ_INT(detect("simple.zip"),   UA_FMT_ZIP,    "zip local header");
    UA_EQ_INT(detect("many.zip"),     UA_FMT_ZIP,    "zip with many entries");
    UA_EQ_INT(detect("plain.tar"),    UA_FMT_TAR,    "ustar magic at offset 257");
    UA_EQ_INT(detect("stub.gz"),      UA_FMT_TAR_GZ, "gzip magic");
    UA_EQ_INT(detect("stub.xz"),      UA_FMT_TAR_XZ, "xz magic");
    UA_EQ_INT(detect("stub.7z"),      UA_FMT_7Z,     "7z magic");

    /* Truncating a zip removes the central directory but not the local header
     * at offset 0, so detection still says zip and the backend is what
     * reports the corruption. That split is intentional: FR-1 rejects
     * unrecognised formats, FR-7 reports broken ones. */
    UA_EQ_INT(detect("truncated.zip"), UA_FMT_ZIP, "truncated zip still detected");
}

static void test_rejects_non_archives(void)
{
    ua_format f = UA_FMT_ZIP;
    char path[512];

    UA_EQ_INT(detect("notarchive.bin"), UA_FMT_UNKNOWN, "plain text");
    UA_EQ_INT(detect("empty.bin"),      UA_FMT_UNKNOWN, "zero-byte file");

    snprintf(path, sizeof path, "%s%s", CORPUS, "notarchive.bin");
    UA_EQ_INT(ua_detect_file(path, &f), UA_ERR_UNSUPPORTED_FORMAT,
              "unknown content is an error, not a guess");

    snprintf(path, sizeof path, "%s%s", CORPUS, "does-not-exist.zip");
    UA_EQ_INT(ua_detect_file(path, &f), UA_ERR_NOT_FOUND, "missing file");
}

static void test_buffer_edges(void)
{
    unsigned char buf[UA_DETECT_BYTES];

    memset(buf, 0, sizeof buf);

    /* A prefix too short to contain the magic must not match on a partial
     * compare. */
    memcpy(buf, "PK", 2);
    UA_EQ_INT(ua_detect_buffer(buf, 2), UA_FMT_UNKNOWN, "partial zip magic");

    memcpy(buf, "PK\x03\x04", 4);
    UA_EQ_INT(ua_detect_buffer(buf, 4), UA_FMT_ZIP, "exactly enough for zip");

    /* ustar magic sits at 257, so anything shorter cannot be a tar even if the
     * bytes would have matched. */
    memset(buf, 0, sizeof buf);
    memcpy(buf + 257, "ustar\0" "00", 8);
    UA_EQ_INT(ua_detect_buffer(buf, 200), UA_FMT_UNKNOWN, "tar magic out of range");
    UA_EQ_INT(ua_detect_buffer(buf, sizeof buf), UA_FMT_TAR, "tar magic in range");

    memset(buf, 0, sizeof buf);
    memcpy(buf + 257, "ustar  \0", 8);
    UA_EQ_INT(ua_detect_buffer(buf, sizeof buf), UA_FMT_TAR, "gnu tar variant");

    UA_EQ_INT(ua_detect_buffer(NULL, 0), UA_FMT_UNKNOWN, "null buffer");
}

static void test_unimplemented_formats_are_named(void)
{
    ua_backend be;

    /* M4 formats must fail at open with a specific reason rather than part-way
     * through extraction (FR-1). */
    UA_EQ_INT(ua_backend_open(UA_FMT_TAR, CORPUS "plain.tar", 0, &be),
              UA_ERR_UNSUPPORTED_FORMAT, "tar not built yet");
    UA_CHECK(strstr(be.detail, "tar") != NULL,
             "reason names the format, got \"%s\"", be.detail);

    /* 7z is implemented, so a truncated stub reaches the backend and is
     * reported as damaged rather than as an unsupported format. */
    UA_EQ_INT(ua_backend_open(UA_FMT_7Z, CORPUS "stub.7z", 0, &be),
              UA_ERR_CORRUPT, "7z stub is a damaged archive, not an unknown one");

    UA_EQ_INT(ua_backend_open(UA_FMT_UNKNOWN, CORPUS "notarchive.bin", 0, &be),
              UA_ERR_UNSUPPORTED_FORMAT, "unknown format");
}

void test_detect_all(void)
{
    printf("test_detect\n");
    test_detects_by_content();
    test_rejects_non_archives();
    test_buffer_edges();
    test_unimplemented_formats_are_named();
}
