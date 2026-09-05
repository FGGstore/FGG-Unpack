/* FR-3 conformance. Every hostile input the requirements name has a case here,
 * plus the near-misses that a naive "does it contain ..?" check would let
 * through. */
#include "../src/core/ua_path.h"
#include "ua_test.h"

/* Convenience: sanitise a NUL-terminated literal. */
static ua_result san(const char *raw, char *out, size_t n)
{
    return ua_path_sanitize_entry(raw, strlen(raw), out, n);
}

static void test_accepts_ordinary_paths(void)
{
    char out[UA_MAX_PATH];

    UA_EQ_INT(san("file.txt", out, sizeof out), UA_OK, "plain file");
    UA_EQ_STR(out, "file.txt");

    UA_EQ_INT(san("dir/sub/file.txt", out, sizeof out), UA_OK, "nested");
    UA_EQ_STR(out, "dir/sub/file.txt");

    /* Trailing slash marks a directory entry; the name is what matters. */
    UA_EQ_INT(san("dir/sub/", out, sizeof out), UA_OK, "dir entry");
    UA_EQ_STR(out, "dir/sub");

    /* Windows-written zips use backslashes. Treating them as separators is the
     * safe reading: the alternative is a literal filename containing "..\". */
    UA_EQ_INT(san("dir\\sub\\file.txt", out, sizeof out), UA_OK, "backslash");
    UA_EQ_STR(out, "dir/sub/file.txt");

    UA_EQ_INT(san("a//b///c", out, sizeof out), UA_OK, "repeated separators");
    UA_EQ_STR(out, "a/b/c");

    UA_EQ_INT(san("./a/./b", out, sizeof out), UA_OK, "dot components");
    UA_EQ_STR(out, "a/b");

    /* Names that merely start with dots are legal and must not be confused
     * with traversal. */
    UA_EQ_INT(san("...", out, sizeof out), UA_OK, "triple dot is a name");
    UA_EQ_STR(out, "...");
    UA_EQ_INT(san("..foo/.bar", out, sizeof out), UA_OK, "dot-prefixed names");
    UA_EQ_STR(out, "..foo/.bar");
    UA_EQ_INT(san("a/..b/c", out, sizeof out), UA_OK, "..b is not ..");
    UA_EQ_STR(out, "a/..b/c");

    UA_EQ_INT(san("h\xc3\xa9llo/\xe6\x97\xa5.txt", out, sizeof out), UA_OK,
              "utf-8 names pass through byte-transparently");
    UA_EQ_STR(out, "h\xc3\xa9llo/\xe6\x97\xa5.txt");
}

static void test_rejects_traversal(void)
{
    char out[UA_MAX_PATH];

    UA_EQ_INT(san("../etc/passwd", out, sizeof out), UA_ERR_UNSAFE_PATH, "leading ..");
    UA_EQ_INT(san("a/../../b", out, sizeof out), UA_ERR_UNSAFE_PATH, "mid ..");
    UA_EQ_INT(san("a/b/..", out, sizeof out), UA_ERR_UNSAFE_PATH, "trailing ..");
    UA_EQ_INT(san("..", out, sizeof out), UA_ERR_UNSAFE_PATH, "bare ..");
    UA_EQ_INT(san("..\\..\\win", out, sizeof out), UA_ERR_UNSAFE_PATH, "backslash ..");
    UA_EQ_INT(san(".//..//x", out, sizeof out), UA_ERR_UNSAFE_PATH, "obscured ..");

    /* Cancelling out still counts: we refuse rather than reason about whether
     * the net effect stays inside. */
    UA_EQ_INT(san("a/../b", out, sizeof out), UA_ERR_UNSAFE_PATH, "net-zero ..");
}

static void test_rejects_absolute_and_drive(void)
{
    char out[UA_MAX_PATH];

    UA_EQ_INT(san("/etc/passwd", out, sizeof out), UA_ERR_UNSAFE_PATH, "absolute");
    UA_EQ_INT(san("\\windows\\x", out, sizeof out), UA_ERR_UNSAFE_PATH, "backslash absolute");
    UA_EQ_INT(san("C:/Windows/x", out, sizeof out), UA_ERR_UNSAFE_PATH, "drive");
    UA_EQ_INT(san("c:x", out, sizeof out), UA_ERR_UNSAFE_PATH, "drive relative");
    UA_EQ_INT(san("//server/share/x", out, sizeof out), UA_ERR_UNSAFE_PATH, "UNC");
    UA_EQ_INT(san("/data/unarchiver/queue.txt", out, sizeof out), UA_ERR_UNSAFE_PATH,
              "absolute path at our own config location");
}

static void test_rejects_degenerate(void)
{
    char out[UA_MAX_PATH];
    char deep[UA_MAX_PATH * 2];
    char longname[UA_MAX_PATH + 64];
    size_t i;

    UA_EQ_INT(san("", out, sizeof out), UA_ERR_UNSAFE_PATH, "empty");
    UA_EQ_INT(san(".", out, sizeof out), UA_ERR_UNSAFE_PATH, "bare dot");
    UA_EQ_INT(san("./", out, sizeof out), UA_ERR_UNSAFE_PATH, "dot slash");
    UA_EQ_INT(san("///", out, sizeof out), UA_ERR_UNSAFE_PATH, "separators only");

    /* Embedded NUL: the length says one thing, strlen() would say another. */
    UA_EQ_INT(ua_path_sanitize_entry("a\0b", 3, out, sizeof out),
              UA_ERR_UNSAFE_PATH, "embedded NUL");
    UA_EQ_INT(ua_path_sanitize_entry("evil.txt\0.png", 13, out, sizeof out),
              UA_ERR_UNSAFE_PATH, "NUL-truncated extension trick");

    /* Over-deep: more components than UA_MAX_PATH_COMPONENTS, while staying
     * under the length cap so the depth check is what fires. */
    deep[0] = '\0';
    for (i = 0; i < UA_MAX_PATH_COMPONENTS + 5; i++)
        strcat(deep, "d/");
    strcat(deep, "f");
    UA_EQ_INT(san(deep, out, sizeof out), UA_ERR_LIMIT, "too many components");

    /* Over-long, even though it is a single legal component. */
    for (i = 0; i < UA_MAX_PATH + 32; i++) longname[i] = 'x';
    longname[UA_MAX_PATH + 32] = '\0';
    UA_EQ_INT(san(longname, out, sizeof out), UA_ERR_LIMIT, "over-long name");

    /* Fits UA_MAX_PATH but not the smaller buffer the caller supplied. */
    UA_EQ_INT(san("aaaa/bbbb/cccc", out, 8), UA_ERR_LIMIT, "small out buffer");
}

static void test_join(void)
{
    char out[UA_MAX_PATH];

    UA_EQ_INT(ua_path_join_checked("/data/dest", "a/b.txt", out, sizeof out),
              UA_OK, "plain join");
    UA_EQ_STR(out, "/data/dest/a/b.txt");

    UA_EQ_INT(ua_path_join_checked("/data/dest/", "a", out, sizeof out),
              UA_OK, "trailing slash on dest");
    UA_EQ_STR(out, "/data/dest/a");

    UA_EQ_INT(ua_path_join_checked("/mnt/usb0//", "f", out, sizeof out),
              UA_OK, "repeated trailing slashes");
    UA_EQ_STR(out, "/mnt/usb0/f");

    /* join_checked refuses anything not already sanitised, so a caller that
     * skips the sanitise step fails closed. */
    UA_EQ_INT(ua_path_join_checked("/data/dest", "../x", out, sizeof out),
              UA_ERR_UNSAFE_PATH, "traversal at join");
    UA_EQ_INT(ua_path_join_checked("/data/dest", "/abs", out, sizeof out),
              UA_ERR_UNSAFE_PATH, "absolute at join");
    UA_EQ_INT(ua_path_join_checked("/data/dest", "a//b", out, sizeof out),
              UA_ERR_UNSAFE_PATH, "un-normalised input rejected");
    UA_EQ_INT(ua_path_join_checked("/data/dest", "a\\b", out, sizeof out),
              UA_ERR_UNSAFE_PATH, "backslash input rejected");
}

static void test_symlink(void)
{
    /* Depth of the directory holding the link is what a relative target
     * resolves against: "a/b/link" sits two levels deep. */
    UA_EQ_INT(ua_path_check_symlink("a/b/link", "target"), UA_OK, "sibling");
    UA_EQ_INT(ua_path_check_symlink("a/b/link", "../sib"), UA_OK, "up one, still inside");
    UA_EQ_INT(ua_path_check_symlink("a/b/link", "../../top"), UA_OK, "up to root of dest");
    UA_EQ_INT(ua_path_check_symlink("a/b/link", "../../../escape"),
              UA_ERR_UNSAFE_PATH, "one level too far");
    UA_EQ_INT(ua_path_check_symlink("link", "../escape"),
              UA_ERR_UNSAFE_PATH, "top-level link escaping");
    UA_EQ_INT(ua_path_check_symlink("link", "/etc/passwd"),
              UA_ERR_UNSAFE_PATH, "absolute target");
    UA_EQ_INT(ua_path_check_symlink("link", "C:\\Windows"),
              UA_ERR_UNSAFE_PATH, "drive target");
    UA_EQ_INT(ua_path_check_symlink("link", ""),
              UA_ERR_UNSAFE_PATH, "empty target");

    /* Escapes mid-walk even though the running depth ends non-negative. */
    UA_EQ_INT(ua_path_check_symlink("a/link", "../../x/y"),
              UA_ERR_UNSAFE_PATH, "escape then descend");
    UA_EQ_INT(ua_path_check_symlink("a/b/link", "../x/../../../y"),
              UA_ERR_UNSAFE_PATH, "escape via cancelling components");
}

static void test_dirname_and_stem(void)
{
    char out[UA_MAX_PATH];

    ua_path_dirname("/data/dest/a/b.txt", out, sizeof out);
    UA_EQ_STR(out, "/data/dest/a");
    ua_path_dirname("b.txt", out, sizeof out);
    UA_EQ_STR(out, "");
    ua_path_dirname("/a", out, sizeof out);
    UA_EQ_STR(out, "");

    ua_path_archive_stem("/mnt/usb0/downloads/homebrew-pack.zip", out, sizeof out);
    UA_EQ_STR(out, "homebrew-pack");
    ua_path_archive_stem("/data/backups/configs.tar.gz", out, sizeof out);
    UA_EQ_STR(out, "configs");
    ua_path_archive_stem("/data/backups/configs.TAR.XZ", out, sizeof out);
    UA_EQ_STR(out, "configs");
    ua_path_archive_stem("noext", out, sizeof out);
    UA_EQ_STR(out, "noext");
    ua_path_archive_stem("a.b.c.zip", out, sizeof out);
    UA_EQ_STR(out, "a.b.c");
}

void test_path_all(void)
{
    printf("test_path\n");
    test_accepts_ordinary_paths();
    test_rejects_traversal();
    test_rejects_absolute_and_drive();
    test_rejects_degenerate();
    test_join();
    test_symlink();
    test_dirname_and_stem();
}
