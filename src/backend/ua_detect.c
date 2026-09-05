#include "ua_detect.h"

#include <stdio.h>
#include <string.h>

/* Offset of the format magic inside a tar header block. */
#define TAR_MAGIC_OFF 257

static int has_prefix(const unsigned char *buf, size_t len,
                      const unsigned char *magic, size_t n)
{
    return len >= n && memcmp(buf, magic, n) == 0;
}

/* A tar has no magic at offset 0. POSIX ustar writes "ustar\0" plus a two-byte
 * version; GNU tar writes "ustar  \0". Older v7 tars have neither, and are not
 * distinguishable from arbitrary data without parsing the header checksum, so
 * they are deliberately not detected. */
static int looks_like_tar(const unsigned char *buf, size_t len)
{
    if (len < TAR_MAGIC_OFF + 8) return 0;
    if (memcmp(buf + TAR_MAGIC_OFF, "ustar\0" "00", 8) == 0) return 1;
    if (memcmp(buf + TAR_MAGIC_OFF, "ustar  \0", 8) == 0) return 1;
    return 0;
}

ua_format ua_detect_buffer(const unsigned char *buf, size_t len)
{
    static const unsigned char zip_local[]   = { 'P', 'K', 0x03, 0x04 };
    static const unsigned char zip_empty[]   = { 'P', 'K', 0x05, 0x06 };
    static const unsigned char zip_spanned[] = { 'P', 'K', 0x07, 0x08 };
    static const unsigned char gzip_magic[]  = { 0x1f, 0x8b };
    static const unsigned char xz_magic[]    = { 0xfd, '7', 'z', 'X', 'Z', 0x00 };
    static const unsigned char sevenz_magic[]= { '7', 'z', 0xbc, 0xaf, 0x27, 0x1c };

    if (!buf) return UA_FMT_UNKNOWN;

    if (has_prefix(buf, len, zip_local,   sizeof zip_local)   ||
        has_prefix(buf, len, zip_empty,   sizeof zip_empty)   ||
        has_prefix(buf, len, zip_spanned, sizeof zip_spanned))
        return UA_FMT_ZIP;

    if (has_prefix(buf, len, sevenz_magic, sizeof sevenz_magic))
        return UA_FMT_7Z;

    if (has_prefix(buf, len, xz_magic, sizeof xz_magic))
        return UA_FMT_TAR_XZ;

    /* gzip is a compression container, not an archive format. Whether it holds
     * a tar is only knowable after inflating the first block, which the tar
     * backend does; reporting TAR_GZ here is the honest name for "gzip stream
     * we will try to read as a tar". */
    if (has_prefix(buf, len, gzip_magic, sizeof gzip_magic))
        return UA_FMT_TAR_GZ;

    if (looks_like_tar(buf, len))
        return UA_FMT_TAR;

    return UA_FMT_UNKNOWN;
}

ua_result ua_detect_file(const char *path, ua_format *out)
{
    unsigned char buf[UA_DETECT_BYTES];
    size_t got;
    FILE *fp;

    if (!path || !out) return UA_ERR_BADARG;
    *out = UA_FMT_UNKNOWN;

    fp = fopen(path, "rb");
    if (!fp) return UA_ERR_NOT_FOUND;

    memset(buf, 0, sizeof buf);
    got = fread(buf, 1, sizeof buf, fp);

    if (ferror(fp)) {
        fclose(fp);
        return UA_ERR_IO;
    }
    fclose(fp);

    if (got == 0) return UA_ERR_UNSUPPORTED_FORMAT;   /* zero-byte file */

    *out = ua_detect_buffer(buf, got);

    return (*out == UA_FMT_UNKNOWN) ? UA_ERR_UNSUPPORTED_FORMAT : UA_OK;
}
