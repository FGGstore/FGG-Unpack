/* Zip backend over miniz.
 *
 * miniz was chosen for v1 over libarchive (requirements Section 3, open
 * question 3) because it is four self-contained C files with no build system
 * of its own, so it cross-compiles against the PS5 SDK toolchain without the
 * autotools work libarchive needs. It covers zip, and its inflate is what the
 * tar.gz backend will use, so the M4 formats are reachable without a new
 * dependency. Only tar.xz and 7z need anything more.
 */
#include "ua_backend.h"

#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A symlink target is stored as the entry body. Anything longer than a path is
 * not a target we would accept anyway. */
#define ZIP_MAX_LINK_TARGET UA_MAX_PATH

typedef struct {
    mz_zip_archive zip;
    mz_uint        num_files;
    mz_uint        cursor;        /* next index next() will look at */
    mz_uint        current;       /* index next() last returned */
    int            have_current;
} zip_impl;

static void zip_record_error(ua_backend *be, zip_impl *z, const char *what)
{
    const char *detail = mz_zip_get_error_string(mz_zip_get_last_error(&z->zip));
    snprintf(be->detail, sizeof be->detail, "%s: %s", what, detail ? detail : "?");
}

/* Unix mode bits live in the high half of the external attributes, and only
 * when the archive says it was made on a Unix-like system. */
static int zip_entry_is_symlink(const mz_zip_archive_file_stat *st)
{
    mz_uint host = (st->m_version_made_by >> 8) & 0xff;
    mz_uint mode;

    if (host != 3 /* Unix */ && host != 19 /* OS X */) return 0;

    mode = (st->m_external_attr >> 16) & 0xffff;
    return (mode & 0xf000) == 0xa000;   /* S_IFLNK */
}

/* ---- sink plumbing -------------------------------------------------- */

typedef struct {
    ua_sink_fn sink;
    void      *ud;
    ua_result  err;
} zip_sink_ctx;

static size_t zip_write_cb(void *opaque, mz_uint64 file_ofs,
                           const void *buf, size_t n)
{
    zip_sink_ctx *c = (zip_sink_ctx *)opaque;

    (void)file_ofs;   /* miniz delivers chunks strictly in order */

    if (c->err != UA_OK) return 0;

    c->err = c->sink(c->ud, buf, n);
    if (c->err != UA_OK) return 0;   /* short write aborts the entry */

    return n;
}

/* Collects a small entry body into a fixed buffer, used for symlink targets. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} zip_mem_ctx;

static ua_result zip_mem_sink(void *ud, const void *buf, size_t len)
{
    zip_mem_ctx *m = (zip_mem_ctx *)ud;

    if (m->len + len >= m->cap) return UA_ERR_LIMIT;
    memcpy(m->buf + m->len, buf, len);
    m->len += len;

    return UA_OK;
}

/* ---- vtable --------------------------------------------------------- */

static ua_result zip_open(ua_backend *be, const char *path)
{
    zip_impl *z;

    z = (zip_impl *)calloc(1, sizeof *z);
    if (!z) return UA_ERR_NOMEM;

    if (!mz_zip_reader_init_file(&z->zip, path, 0)) {
        const char *detail = mz_zip_get_error_string(mz_zip_get_last_error(&z->zip));
        snprintf(be->detail, sizeof be->detail, "open: %s", detail ? detail : "?");
        free(z);
        return UA_ERR_CORRUPT;
    }

    z->num_files = mz_zip_reader_get_num_files(&z->zip);
    be->impl = z;

    return UA_OK;
}

static ua_result zip_reset(ua_backend *be)
{
    zip_impl *z = (zip_impl *)be->impl;

    z->cursor       = 0;
    z->have_current = 0;

    return UA_OK;
}

static ua_result zip_next(ua_backend *be, ua_entry *out)
{
    zip_impl *z = (zip_impl *)be->impl;
    mz_zip_archive_file_stat st;
    mz_uint needed;

    z->have_current = 0;

    if (z->cursor >= z->num_files) return UA_ERR_NOT_FOUND;   /* clean end */

    memset(out, 0, sizeof *out);

    if (!mz_zip_reader_file_stat(&z->zip, z->cursor, &st)) {
        zip_record_error(be, z, "stat");
        return UA_ERR_CORRUPT;
    }

    /* m_filename in the stat struct is capped at MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE
     * and silently truncated, so the name is fetched separately.
     *
     * The zero-size call is not an optimisation: mz_zip_reader_get_filename
     * clamps the length to the buffer *before* returning it, so a call that
     * passes a buffer can never report a name larger than that buffer. Asking
     * with no buffer is the only way to see the true length, and the
     * difference between the two is what distinguishes "long name" from
     * "silently renamed to something shorter". */
    needed = mz_zip_reader_get_filename(&z->zip, z->cursor, NULL, 0);
    if (needed == 0) {
        zip_record_error(be, z, "filename");
        return UA_ERR_CORRUPT;
    }
    if (needed > sizeof out->path) {
        snprintf(be->detail, sizeof be->detail,
                 "entry name is %u bytes, limit is %u",
                 (unsigned)(needed - 1), (unsigned)sizeof out->path - 1);
        return UA_ERR_LIMIT;
    }

    if (mz_zip_reader_get_filename(&z->zip, z->cursor, out->path,
                                   (mz_uint)sizeof out->path) != needed) {
        zip_record_error(be, z, "filename");
        return UA_ERR_CORRUPT;
    }

    /* No decryption on-console: encrypted entries are out of scope (Section 2),
     * and failing here is far clearer than producing garbage files. */
    if (st.m_is_encrypted) {
        snprintf(be->detail, sizeof be->detail, "entry is encrypted");
        return UA_ERR_UNSUPPORTED_FORMAT;
    }
    if (!st.m_is_supported) {
        snprintf(be->detail, sizeof be->detail,
                 "compression method %u unsupported", (unsigned)st.m_method);
        return UA_ERR_UNSUPPORTED_FORMAT;
    }

    out->size            = st.m_uncomp_size;
    out->compressed_size = st.m_comp_size;
    out->mtime           = (int64_t)st.m_time;

    if (st.m_is_directory) {
        out->kind = UA_ENTRY_DIR;
    } else if (zip_entry_is_symlink(&st)) {
        zip_mem_ctx  m;
        zip_sink_ctx c;

        out->kind = UA_ENTRY_SYMLINK;

        if (st.m_uncomp_size >= ZIP_MAX_LINK_TARGET) {
            snprintf(be->detail, sizeof be->detail, "symlink target too long");
            return UA_ERR_LIMIT;
        }

        m.buf = out->link_target;
        m.cap = sizeof out->link_target;
        m.len = 0;
        c.sink = zip_mem_sink;
        c.ud   = &m;
        c.err  = UA_OK;

        if (!mz_zip_reader_extract_to_callback(&z->zip, z->cursor,
                                               zip_write_cb, &c, 0)) {
            if (c.err != UA_OK) return c.err;
            zip_record_error(be, z, "symlink target");
            return UA_ERR_CORRUPT;
        }
        out->link_target[m.len] = '\0';
    } else {
        out->kind = UA_ENTRY_FILE;
    }

    z->current      = z->cursor;
    z->have_current = 1;
    z->cursor++;

    return UA_OK;
}

static ua_result zip_extract_current(ua_backend *be, ua_sink_fn sink, void *ud)
{
    zip_impl *z = (zip_impl *)be->impl;
    zip_sink_ctx c;

    if (!z->have_current) return UA_ERR_INTERNAL;

    c.sink = sink;
    c.ud   = ud;
    c.err  = UA_OK;

    /* extract_to_callback streams: miniz inflates into its own bounded window
     * and hands us chunks, so peak memory does not scale with entry size
     * (FR-4). */
    if (!mz_zip_reader_extract_to_callback(&z->zip, z->current,
                                           zip_write_cb, &c, 0)) {
        /* A sink refusal is the more specific reason; miniz only reports that
         * the write callback failed. */
        if (c.err != UA_OK) return c.err;
        zip_record_error(be, z, "extract");
        return UA_ERR_CORRUPT;
    }

    return c.err;
}

static void zip_close(ua_backend *be)
{
    zip_impl *z = (zip_impl *)be->impl;

    if (!z) return;

    mz_zip_reader_end(&z->zip);
    free(z);
    be->impl = NULL;
}

const ua_backend_vtable ua_backend_zip = {
    "zip/miniz",
    zip_open,
    zip_reset,
    zip_next,
    zip_extract_current,
    zip_close,
    /* No parallel path: deflate is a single dependent stream with no restart
     * points, so an entry cannot be decoded out of order. Zip archives get
     * their parallelism from having many entries, which is a different feature
     * and one FR-8 rules out anyway. */
    NULL
};
