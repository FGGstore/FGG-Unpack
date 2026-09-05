#include "ua_path.h"

#include <string.h>

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* True for "C:", "c:\foo" and friends. A zip written on Windows can carry
 * these even though the format nominally forbids them. */
static int has_drive_prefix(const char *s, size_t len)
{
    return len >= 2 && s[1] == ':';
}

ua_result ua_path_sanitize_entry(const char *raw, size_t raw_len,
                                 char *out, size_t out_sz)
{
    size_t i = 0, o = 0, comps = 0;

    if (!raw || !out || out_sz == 0) return UA_ERR_BADARG;
    out[0] = '\0';

    if (raw_len == 0) return UA_ERR_UNSAFE_PATH;
    if (raw_len >= UA_MAX_PATH) return UA_ERR_LIMIT;

    /* An entry name is a byte string in the archive; a NUL inside it means the
     * writer is lying about the length, and consumers that use strlen() would
     * see a different path than consumers that use the length field. */
    if (memchr(raw, '\0', raw_len) != NULL) return UA_ERR_UNSAFE_PATH;

    if (is_sep(raw[0])) return UA_ERR_UNSAFE_PATH;          /* absolute */
    if (has_drive_prefix(raw, raw_len)) return UA_ERR_UNSAFE_PATH;

    while (i < raw_len) {
        size_t start, len;

        while (i < raw_len && is_sep(raw[i])) i++;   /* collapse separators */
        if (i >= raw_len) break;

        start = i;
        while (i < raw_len && !is_sep(raw[i])) i++;
        len = i - start;

        if (len == 1 && raw[start] == '.')
            continue;                                 /* "." contributes nothing */

        if (len == 2 && raw[start] == '.' && raw[start + 1] == '.')
            return UA_ERR_UNSAFE_PATH;                /* Zip Slip */

        if (++comps > UA_MAX_PATH_COMPONENTS) return UA_ERR_LIMIT;

        if (o != 0) {
            if (o + 1 >= out_sz) return UA_ERR_LIMIT;
            out[o++] = '/';
        }
        if (o + len >= out_sz) return UA_ERR_LIMIT;
        memcpy(out + o, raw + start, len);
        o += len;
    }

    out[o] = '\0';

    /* "." , "./" and "///" all normalise away to nothing. There is no file to
     * write, so treat it as a malformed entry rather than silently skipping. */
    if (o == 0) return UA_ERR_UNSAFE_PATH;

    return UA_OK;
}

ua_result ua_path_join_checked(const char *dest, const char *rel,
                               char *out, size_t out_sz)
{
    char verified[UA_MAX_PATH];
    size_t dl, rl, need;
    ua_result r;

    if (!dest || !rel || !out || out_sz == 0) return UA_ERR_BADARG;
    out[0] = '\0';

    /* The caller is expected to have sanitised `rel` already. Re-running the
     * sanitiser and demanding an exact match turns a caller bug into a refusal
     * instead of a write outside the destination. */
    r = ua_path_sanitize_entry(rel, strlen(rel), verified, sizeof verified);
    if (r != UA_OK) return r;
    if (strcmp(verified, rel) != 0) return UA_ERR_UNSAFE_PATH;

    dl = strlen(dest);
    while (dl > 0 && is_sep(dest[dl - 1])) dl--;   /* ignore trailing slashes */
    if (dl == 0) return UA_ERR_BADARG;

    rl = strlen(rel);
    need = dl + 1 + rl;
    if (need >= out_sz || need >= UA_MAX_PATH) return UA_ERR_LIMIT;

    memcpy(out, dest, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, rel, rl);
    out[need] = '\0';

    return UA_OK;
}

ua_result ua_path_check_symlink(const char *link_rel, const char *target)
{
    size_t tl, i = 0;
    long depth = 0;
    const char *p;

    if (!link_rel || !target) return UA_ERR_BADARG;

    tl = strlen(target);
    if (tl == 0) return UA_ERR_UNSAFE_PATH;
    if (tl >= UA_MAX_PATH) return UA_ERR_LIMIT;

    if (is_sep(target[0])) return UA_ERR_UNSAFE_PATH;      /* absolute target */
    if (has_drive_prefix(target, tl)) return UA_ERR_UNSAFE_PATH;

    /* link_rel is sanitised, so it has no leading, trailing or repeated
     * separators: the count of '/' is exactly the depth of the directory the
     * link lives in, which is where a relative target resolves from. */
    for (p = link_rel; *p; p++)
        if (*p == '/') depth++;

    while (i < tl) {
        size_t start, len;

        while (i < tl && is_sep(target[i])) i++;
        if (i >= tl) break;

        start = i;
        while (i < tl && !is_sep(target[i])) i++;
        len = i - start;

        if (len == 1 && target[start] == '.')
            continue;

        if (len == 2 && target[start] == '.' && target[start + 1] == '.') {
            /* Checked at every step, not just at the end, so "../x/../.." is
             * caught even though its running total returns to zero mid-walk. */
            if (--depth < 0) return UA_ERR_UNSAFE_PATH;
            continue;
        }

        if (++depth > (long)UA_MAX_PATH_COMPONENTS) return UA_ERR_LIMIT;
    }

    return UA_OK;
}

void ua_path_dirname(const char *path, char *out, size_t out_sz)
{
    size_t len, i;

    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!path) return;

    len = strlen(path);
    while (len > 0 && is_sep(path[len - 1])) len--;

    i = len;
    while (i > 0 && !is_sep(path[i - 1])) i--;
    while (i > 0 && is_sep(path[i - 1])) i--;   /* drop the separator itself */

    if (i == 0 || i >= out_sz) return;
    memcpy(out, path, i);
    out[i] = '\0';
}

/* Case-insensitive suffix test; archive names arrive with arbitrary casing. */
static int ends_with_ci(const char *s, size_t slen, const char *suffix)
{
    size_t n = strlen(suffix);
    size_t i;

    if (slen < n) return 0;
    for (i = 0; i < n; i++) {
        char a = s[slen - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

void ua_path_archive_stem(const char *archive_path, char *out, size_t out_sz)
{
    static const char *doubles[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst" };
    const char *base;
    size_t len, i;

    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!archive_path) return;

    base = archive_path;
    for (i = 0; archive_path[i]; i++)
        if (is_sep(archive_path[i])) base = archive_path + i + 1;

    len = strlen(base);

    for (i = 0; i < sizeof doubles / sizeof doubles[0]; i++) {
        if (ends_with_ci(base, len, doubles[i])) {
            len -= strlen(doubles[i]);
            goto emit;
        }
    }
    /* Otherwise drop a single trailing extension, if any. */
    for (i = len; i > 0; i--) {
        if (base[i - 1] == '.') { len = i - 1; break; }
    }

emit:
    if (len == 0 || len >= out_sz) {
        /* No usable stem (".zip") or too long: fall back to the whole name. */
        len = strlen(base);
        if (len >= out_sz) len = out_sz - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}
