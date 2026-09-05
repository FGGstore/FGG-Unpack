/* 7z backend over the LZMA SDK.
 *
 * The SDK's own SzArEx_Extract() decodes an entire folder — a solid block —
 * into one allocated buffer. A 7z holding a single 150 GB disk image is one
 * folder, so that call would try to allocate 150 GB. FR-4 forbids buffering an
 * entry regardless, so this backend does not use it: it drives Lzma2Dec/
 * LzmaDec directly and streams the folder through fixed buffers, which keeps
 * peak memory at the dictionary size plus a couple of hundred KB no matter how
 * large the archive is.
 *
 * What that costs: only single-coder LZMA and LZMA2 folders are supported.
 * Filter chains (BCJ2, delta, ARM64 branch conversion) and encryption are
 * refused by name rather than mis-decoded. Those are rare for the payloads
 * this tool exists to move, and adding one later is a new case in
 * folder_begin(), not a redesign.
 *
 * Files inside one solid folder can only be produced in order, because the
 * decoder state is the folder. That matches the sequential ua_backend
 * interface exactly; a caller that skips a file makes the skipped bytes get
 * decoded and discarded rather than seeking, which is inherent to solid
 * compression and not a defect here.
 */
#include "ua_backend.h"

#include "../platform/ua_platform.h"

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Method IDs from the 7z format specification. */
#define SEVENZ_METHOD_COPY  0x00
#define SEVENZ_METHOD_LZMA  0x030101
#define SEVENZ_METHOD_LZMA2 0x21

/* Fallback when the caller expresses no preference. The real size comes from
 * config chunk_size_kb via ua_backend.chunk_bytes: on a console where each
 * write carries real per-call cost, moving from 256 KB to a few MB removes
 * most of the calls. */
#define SEVENZ_DEFAULT_BUF (256 * 1024)

/* Scratch used when discarding bytes ahead of the wanted file in a solid
 * folder; the sink is not involved, so this never reaches the caller. */
#define SEVENZ_SKIP_BUF (64 * 1024)

typedef enum {
    CODER_NONE = 0,
    CODER_COPY,
    CODER_LZMA,
    CODER_LZMA2
} sevenz_coder;

/* ISeekInStream over stdio. 7zFile.c would do this too, but it reaches for
 * platform file APIs directly, and the rest of this project keeps that
 * behind ua_platform.h. Reading is all this needs. */
typedef struct {
    ISeekInStream vt;
    FILE *fp;
} sevenz_stream;

typedef struct {
    sevenz_stream  stream;
    CLookToRead2   look;
    Byte          *look_buf;

    /* Workers open their own read handle on the archive; only the output
     * descriptor is shared. */
    char           archive_path[UA_MAX_PATH];

    CSzArEx        db;
    ISzAlloc       alloc;
    ISzAlloc       alloc_tmp;
    int            db_open;

    UInt32         cursor;         /* next index next() will return */
    UInt32         current;        /* index next() last returned */
    int            have_current;

    /* Folder decode state. */
    UInt32         folder;         /* folder currently open */
    int            folder_open;
    sevenz_coder   coder;
    UInt64         folder_produced; /* bytes decoded out of this folder */
    UInt64         folder_size;     /* total unpacked size of this folder */
    UInt64         pack_left;       /* packed bytes not yet read */

    CLzma2Dec      lzma2;
    CLzmaDec       lzma;
    int            dec_inited;

    size_t         buf_size;      /* both buffers; from chunk_bytes */
    Byte          *in_buf;
    size_t         in_have;
    size_t         in_pos;

    /* Decoded bytes not yet handed to a caller.
     *
     * A decode round produces whatever it produces, which is routinely more
     * than the current entry still wants — several small files share one
     * round. The surplus has to stay here for the next entry rather than being
     * dropped, or every file after the first in a folder reads from the wrong
     * offset. */
    Byte          *out_buf;
    size_t         out_have;
    size_t         out_pos;
} sevenz_impl;

/* Logical position in the folder: what has been handed out, as opposed to what
 * has been decoded. The two differ by whatever is still buffered. */
#define FOLDER_POS(z) ((z)->folder_produced - (UInt64)((z)->out_have - (z)->out_pos))

/* ---- SDK gap --------------------------------------------------------- */

/* 7z.h declares these two but SDK 23.01 defines them nowhere: they are
 * leftovers from when SzArEx_Extract was the only extraction path. Both are
 * one line over the parsed structures, and defining them here rather than
 * patching the vendored tree keeps third_party/lzma byte-identical to the
 * upstream release. */
UInt64 SzArEx_GetFolderStreamPos(const CSzArEx *p, UInt32 folderIndex,
                                 UInt32 indexInFolder)
{
    return p->dataPos +
           p->db.PackPositions[p->db.FoStartPackStreamIndex[folderIndex] + indexInFolder];
}

int SzArEx_GetFolderFullPackSize(const CSzArEx *p, UInt32 folderIndex,
                                 UInt64 *resSize)
{
    const UInt32 first = p->db.FoStartPackStreamIndex[folderIndex];
    const UInt32 next  = p->db.FoStartPackStreamIndex[(size_t)folderIndex + 1];

    *resSize = p->db.PackPositions[next] - p->db.PackPositions[first];

    return 0;   /* SZ_OK */
}

/* ---- stdio stream ---------------------------------------------------- */

static SRes stream_read(ISeekInStreamPtr pp, void *buf, size_t *size)
{
    sevenz_stream *s = Z7_CONTAINER_FROM_VTBL(pp, sevenz_stream, vt);
    size_t want = *size;
    size_t got;

    if (want == 0) return SZ_OK;

    got = fread(buf, 1, want, s->fp);
    *size = got;

    /* A short read at end of file is not an error; ferror distinguishes. */
    return ferror(s->fp) ? SZ_ERROR_READ : SZ_OK;
}

static SRes stream_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
    sevenz_stream *s = Z7_CONTAINER_FROM_VTBL(pp, sevenz_stream, vt);
    int whence;

    switch (origin) {
    case SZ_SEEK_SET: whence = SEEK_SET; break;
    case SZ_SEEK_CUR: whence = SEEK_CUR; break;
    case SZ_SEEK_END: whence = SEEK_END; break;
    default: return SZ_ERROR_PARAM;
    }

#ifdef _WIN32
    if (_fseeki64(s->fp, (__int64)*pos, whence) != 0) return SZ_ERROR_READ;
    *pos = (Int64)_ftelli64(s->fp);
#else
    if (fseeko(s->fp, (off_t)*pos, whence) != 0) return SZ_ERROR_READ;
    *pos = (Int64)ftello(s->fp);
#endif

    return SZ_OK;
}

/* ---- names ----------------------------------------------------------- */

/* 7z stores names as UTF-16. The rest of the project works in bytes, matching
 * the console filesystem, so names are converted to UTF-8 here and validated
 * by ua_path_sanitize_entry like any other backend's. Surrogate pairs are
 * combined; an unpaired surrogate is rejected rather than emitted as invalid
 * UTF-8. */
static int utf16_to_utf8(const UInt16 *src, char *dst, size_t dst_sz)
{
    size_t o = 0;

    for (; *src; src++) {
        unsigned cp = *src;

        if (cp >= 0xd800 && cp <= 0xdbff) {
            unsigned lo = src[1];
            if (lo < 0xdc00 || lo > 0xdfff) return 0;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            src++;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return 0;
        }

        if (cp < 0x80) {
            if (o + 1 >= dst_sz) return 0;
            dst[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= dst_sz) return 0;
            dst[o++] = (char)(0xc0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            if (o + 3 >= dst_sz) return 0;
            dst[o++] = (char)(0xe0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            dst[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            if (o + 4 >= dst_sz) return 0;
            dst[o++] = (char)(0xf0 | (cp >> 18));
            dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            dst[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }

    dst[o] = '\0';
    return 1;
}

/* ---- folder decoding ------------------------------------------------- */

static void folder_end(sevenz_impl *z)
{
    if (!z->folder_open) return;

    if (z->coder == CODER_LZMA2) Lzma2Dec_Free(&z->lzma2, &z->alloc);
    else if (z->coder == CODER_LZMA) LzmaDec_Free(&z->lzma, &z->alloc);

    z->folder_open = 0;
    z->dec_inited = 0;
    z->coder = CODER_NONE;
}

/* Parses a folder's coder chain and classifies it, without touching the
 * decoder or the input stream.
 *
 * Split out from folder_begin so an undecodable folder can be rejected during
 * preflight, from next(), rather than half way through extraction. FR-2 says
 * a failed preflight leaves nothing written, and folder_begin runs too late
 * for that — by then the destination directory exists.
 */
static ua_result folder_classify(ua_backend *be, sevenz_impl *z, UInt32 folder,
                                 CSzFolder *f_out, const Byte **data_out,
                                 sevenz_coder *coder_out)
{
    const CSzAr *ar = &z->db.db;
    const Byte *coder_data;
    CSzData sd;

    coder_data = ar->CodersData + ar->FoCodersOffsets[folder];
    sd.Data = coder_data;
    sd.Size = ar->FoCodersOffsets[(size_t)folder + 1] - ar->FoCodersOffsets[folder];

    if (SzGetNextFolderItem(f_out, &sd) != SZ_OK) {
        snprintf(be->detail, sizeof be->detail, "cannot read folder %u", folder);
        return UA_ERR_CORRUPT;
    }

    if (f_out->NumCoders != 1 || f_out->NumPackStreams != 1) {
        /* A filter chain: BCJ2 has four inputs, delta and branch filters add a
         * second coder. Supporting them means running the chain, which the
         * streaming design can do but does not yet. */
        snprintf(be->detail, sizeof be->detail,
                 "folder uses a %u-coder filter chain, which this build cannot decode",
                 (unsigned)f_out->NumCoders);
        return UA_ERR_UNSUPPORTED_FORMAT;
    }

    switch (f_out->Coders[0].MethodID) {
    case SEVENZ_METHOD_LZMA2: *coder_out = CODER_LZMA2; break;
    case SEVENZ_METHOD_LZMA:  *coder_out = CODER_LZMA;  break;
    case SEVENZ_METHOD_COPY:  *coder_out = CODER_COPY;  break;
    default:
        snprintf(be->detail, sizeof be->detail,
                 "unsupported 7z compression method %08lx",
                 (unsigned long)f_out->Coders[0].MethodID);
        return UA_ERR_UNSUPPORTED_FORMAT;
    }

    *data_out = coder_data;

    return UA_OK;
}

/* Opens `folder` for streaming: parses its coder chain, positions the input at
 * its packed data and initialises the decoder. */
static ua_result folder_begin(ua_backend *be, sevenz_impl *z, UInt32 folder)
{
    const CSzAr *ar = &z->db.db;
    const Byte *coder_data;
    CSzFolder f;
    UInt64 pos;
    ua_result r;

    folder_end(z);

    r = folder_classify(be, z, folder, &f, &coder_data, &z->coder);
    if (r != UA_OK) return r;

    z->folder_size     = SzAr_GetFolderUnpackSize(ar, folder);
    z->folder_produced = 0;
    z->in_have = z->in_pos = 0;
    z->out_have = z->out_pos = 0;

    if (SzArEx_GetFolderFullPackSize(&z->db, folder, &z->pack_left) != 0) {
        snprintf(be->detail, sizeof be->detail, "bad packed size for folder %u", folder);
        return UA_ERR_CORRUPT;
    }

    pos = SzArEx_GetFolderStreamPos(&z->db, folder, 0);
    {
        Int64 p = (Int64)pos;
        if (stream_seek(&z->stream.vt, &p, SZ_SEEK_SET) != SZ_OK) {
            snprintf(be->detail, sizeof be->detail, "cannot seek to folder data");
            return UA_ERR_IO;
        }
    }

    if (z->coder == CODER_LZMA2) {
        const Byte *props = coder_data + f.Coders[0].PropsOffset;

        if (f.Coders[0].PropsSize != 1) {
            snprintf(be->detail, sizeof be->detail, "bad LZMA2 properties");
            return UA_ERR_CORRUPT;
        }
        Lzma2Dec_Construct(&z->lzma2);
        /* Allocates the dictionary the archive asks for: LZMA2:28 means a
         * 256 MB window, which is the real memory cost of decoding these. */
        if (Lzma2Dec_Allocate(&z->lzma2, props[0], &z->alloc) != SZ_OK) {
            snprintf(be->detail, sizeof be->detail,
                     "cannot allocate the LZMA2 dictionary this archive needs");
            return UA_ERR_NOMEM;
        }
        Lzma2Dec_Init(&z->lzma2);
    } else if (z->coder == CODER_LZMA) {
        const Byte *props = coder_data + f.Coders[0].PropsOffset;

        if (f.Coders[0].PropsSize != LZMA_PROPS_SIZE) {
            snprintf(be->detail, sizeof be->detail, "bad LZMA properties");
            return UA_ERR_CORRUPT;
        }
        LzmaDec_Construct(&z->lzma);
        if (LzmaDec_Allocate(&z->lzma, props, LZMA_PROPS_SIZE, &z->alloc) != SZ_OK) {
            snprintf(be->detail, sizeof be->detail,
                     "cannot allocate the LZMA dictionary this archive needs");
            return UA_ERR_NOMEM;
        }
        LzmaDec_Init(&z->lzma);
    }

    z->folder = folder;
    z->folder_open = 1;
    z->dec_inited = 1;

    return UA_OK;
}

/* Makes sure at least one decoded byte is buffered, decoding more if needed.
 *
 * Leaves out_pos/out_have describing what is available. Returns with nothing
 * buffered only when the folder is genuinely finished.
 *
 * The loop matters: a decode round can legitimately consume input and produce
 * nothing at all — LZMA2 chunk headers and dictionary resets both do it — and
 * treating a single empty round as end-of-stream would truncate the entry.
 */
static ua_result folder_fill(ua_backend *be, sevenz_impl *z)
{
    if (z->out_pos < z->out_have) return UA_OK;   /* still buffered */

    z->out_pos = z->out_have = 0;

    while (z->folder_produced < z->folder_size) {
        UInt64 left = z->folder_size - z->folder_produced;

        /* Refill the packed-input buffer when it runs dry. */
        if (z->in_pos == z->in_have) {
            size_t want = z->buf_size;

            if (z->pack_left == 0) {
                snprintf(be->detail, sizeof be->detail,
                         "packed data ends %llu bytes early",
                         (unsigned long long)left);
                return UA_ERR_CORRUPT;
            }

            if ((UInt64)want > z->pack_left) want = (size_t)z->pack_left;

            z->in_have = want;
            if (stream_read(&z->stream.vt, z->in_buf, &z->in_have) != SZ_OK) {
                snprintf(be->detail, sizeof be->detail, "read error in packed data");
                return UA_ERR_IO;
            }
            if (z->in_have == 0) {
                snprintf(be->detail, sizeof be->detail, "packed data ends early");
                return UA_ERR_CORRUPT;
            }
            z->in_pos = 0;
            z->pack_left -= z->in_have;
        }

        if (z->coder == CODER_COPY) {
            size_t n = z->in_have - z->in_pos;

            if ((UInt64)n > left) n = (size_t)left;
            if (n > z->buf_size) n = z->buf_size;

            memcpy(z->out_buf, z->in_buf + z->in_pos, n);
            z->in_pos += n;
            z->folder_produced += n;
            z->out_have = n;

            return UA_OK;
        }

        {
            SizeT dst_len = (SizeT)z->buf_size;
            SizeT src_len = z->in_have - z->in_pos;
            ELzmaStatus status;
            SRes res;

            if ((UInt64)dst_len > left) dst_len = (SizeT)left;

            if (z->coder == CODER_LZMA2) {
                res = Lzma2Dec_DecodeToBuf(&z->lzma2, z->out_buf, &dst_len,
                                           z->in_buf + z->in_pos, &src_len,
                                           LZMA_FINISH_ANY, &status);
            } else {
                res = LzmaDec_DecodeToBuf(&z->lzma, z->out_buf, &dst_len,
                                          z->in_buf + z->in_pos, &src_len,
                                          LZMA_FINISH_ANY, &status);
            }

            if (res != SZ_OK) {
                snprintf(be->detail, sizeof be->detail,
                         "LZMA decode failed at %llu of %llu bytes",
                         (unsigned long long)z->folder_produced,
                         (unsigned long long)z->folder_size);
                return UA_ERR_CORRUPT;
            }

            z->in_pos += src_len;
            z->folder_produced += dst_len;

            if (dst_len != 0) {
                z->out_have = dst_len;
                return UA_OK;
            }

            /* Neither consumed nor produced: the decoder cannot progress with
             * what it has, and looping again would spin forever. */
            if (src_len == 0) {
                snprintf(be->detail, sizeof be->detail,
                         "decoder stalled %llu bytes before the end",
                         (unsigned long long)left);
                return UA_ERR_CORRUPT;
            }
        }
    }

    return UA_OK;   /* folder finished */
}

/* Decodes and discards up to `target`, used to reach a file that starts part
 * way into a solid folder. There is no cheaper way: a solid block has no entry
 * point per file. */
static ua_result folder_skip_to(ua_backend *be, sevenz_impl *z, UInt64 target)
{
    while (FOLDER_POS(z) < target) {
        UInt64 need;
        size_t avail, n;
        ua_result r = folder_fill(be, z);

        if (r != UA_OK) return r;

        avail = z->out_have - z->out_pos;
        if (avail == 0) {
            snprintf(be->detail, sizeof be->detail, "folder ended before the entry");
            return UA_ERR_CORRUPT;
        }

        need = target - FOLDER_POS(z);
        n = ((UInt64)avail > need) ? (size_t)need : avail;
        z->out_pos += n;
    }

    return UA_OK;
}

/* ---- vtable ---------------------------------------------------------- */

static ua_result sevenz_open(ua_backend *be, const char *path)
{
    sevenz_impl *z;
    size_t plen;
    SRes res;

    plen = strlen(path);
    if (plen >= UA_MAX_PATH) return UA_ERR_LIMIT;

    z = (sevenz_impl *)calloc(1, sizeof *z);
    if (!z) return UA_ERR_NOMEM;

    memcpy(z->archive_path, path, plen + 1);

    z->alloc.Alloc     = SzAlloc;
    z->alloc.Free      = SzFree;
    z->alloc_tmp.Alloc = SzAllocTemp;
    z->alloc_tmp.Free  = SzFreeTemp;

    z->stream.fp = fopen(path, "rb");
    if (!z->stream.fp) {
        snprintf(be->detail, sizeof be->detail, "cannot open archive");
        free(z);
        return UA_ERR_NOT_FOUND;
    }
    z->stream.vt.Read = stream_read;
    z->stream.vt.Seek = stream_seek;

    z->buf_size = be->chunk_bytes ? be->chunk_bytes : SEVENZ_DEFAULT_BUF;

    z->look_buf = (Byte *)malloc(z->buf_size);
    z->in_buf   = (Byte *)malloc(z->buf_size);
    z->out_buf  = (Byte *)malloc(z->buf_size);
    if (!z->look_buf || !z->in_buf || !z->out_buf) {
        free(z->look_buf); free(z->in_buf); free(z->out_buf);
        fclose(z->stream.fp);
        free(z);
        return UA_ERR_NOMEM;
    }

    LookToRead2_CreateVTable(&z->look, False);
    z->look.buf         = z->look_buf;
    z->look.bufSize     = z->buf_size;
    z->look.realStream  = &z->stream.vt;
    LookToRead2_INIT(&z->look)

    /* The CRC table is global to the SDK and must exist before any archive is
     * parsed. Idempotent, so calling it per open is harmless. */
    CrcGenerateTable();

    SzArEx_Init(&z->db);
    res = SzArEx_Open(&z->db, &z->look.vt, &z->alloc, &z->alloc_tmp);
    if (res != SZ_OK) {
        snprintf(be->detail, sizeof be->detail,
                 res == SZ_ERROR_UNSUPPORTED ? "unsupported 7z feature (encryption?)"
                                             : "cannot read 7z header");
        SzArEx_Free(&z->db, &z->alloc);
        free(z->look_buf); free(z->in_buf); free(z->out_buf);
        fclose(z->stream.fp);
        free(z);
        return res == SZ_ERROR_UNSUPPORTED ? UA_ERR_UNSUPPORTED_FORMAT : UA_ERR_CORRUPT;
    }
    z->db_open = 1;

    be->impl = z;
    return UA_OK;
}

static ua_result sevenz_reset(ua_backend *be)
{
    sevenz_impl *z = (sevenz_impl *)be->impl;

    z->cursor = 0;
    z->have_current = 0;
    folder_end(z);

    return UA_OK;
}

static ua_result sevenz_next(ua_backend *be, ua_entry *out)
{
    sevenz_impl *z = (sevenz_impl *)be->impl;
    UInt16 name16[UA_MAX_PATH];
    size_t name_len;

    z->have_current = 0;

    if (z->cursor >= z->db.NumFiles) return UA_ERR_NOT_FOUND;

    memset(out, 0, sizeof *out);

    name_len = SzArEx_GetFileNameUtf16(&z->db, z->cursor, NULL);
    if (name_len == 0 || name_len > UA_MAX_PATH) {
        snprintf(be->detail, sizeof be->detail,
                 "entry name is %u UTF-16 units, limit is %u",
                 (unsigned)name_len, (unsigned)UA_MAX_PATH);
        return UA_ERR_LIMIT;
    }
    SzArEx_GetFileNameUtf16(&z->db, z->cursor, name16);

    if (!utf16_to_utf8(name16, out->path, sizeof out->path)) {
        snprintf(be->detail, sizeof be->detail, "entry name is not valid UTF-16");
        return UA_ERR_UNSAFE_PATH;
    }

    if (SzArEx_IsDir(&z->db, z->cursor)) {
        out->kind = UA_ENTRY_DIR;
    } else {
        UInt32 folder = z->db.FileToFolder[z->cursor];

        out->kind = UA_ENTRY_FILE;
        out->size = SzArEx_GetFileSize(&z->db, z->cursor);

        /* Checked here, during the preflight walk, so an archive this build
         * cannot decode is refused before anything is created. Deferring it to
         * extract_current would mean the destination directory already exists
         * by the time we find out, which FR-2 forbids. */
        if (folder != (UInt32)-1) {
            CSzFolder    f;
            const Byte  *data;
            sevenz_coder coder;
            ua_result    r = folder_classify(be, z, folder, &f, &data, &coder);

            if (r != UA_OK) return r;
        }

        /* Unix mode lives in the high half of the attributes when 0x8000 is
         * set, the same convention zip uses. */
        if (SzBitWithVals_Check(&z->db.Attribs, z->cursor)) {
            UInt32 attr = z->db.Attribs.Vals[z->cursor];

            if (attr & 0x8000) {
                unsigned mode = (attr >> 16) & 0xffff;
                if ((mode & 0xf000) == 0xa000) out->kind = UA_ENTRY_SYMLINK;
            }
        }
    }

    if (SzBitWithVals_Check(&z->db.MTime, z->cursor)) {
        const CNtfsFileTime *ft = &z->db.MTime.Vals[z->cursor];
        UInt64 t = ((UInt64)ft->High << 32) | ft->Low;

        /* NTFS epoch is 1601-01-01 in 100 ns units. */
        if (t >= 116444736000000000ULL)
            out->mtime = (int64_t)((t - 116444736000000000ULL) / 10000000ULL);
    }

    z->current = z->cursor;
    z->have_current = 1;
    z->cursor++;

    return UA_OK;
}

static ua_result sevenz_extract_current(ua_backend *be, ua_sink_fn sink, void *ud)
{
    sevenz_impl *z = (sevenz_impl *)be->impl;
    UInt32 folder;
    UInt64 file_start, want;
    ua_result r;

    if (!z->have_current) return UA_ERR_INTERNAL;

    if (SzArEx_IsDir(&z->db, z->current)) return UA_OK;

    want = SzArEx_GetFileSize(&z->db, z->current);
    if (want == 0) return UA_OK;

    folder = z->db.FileToFolder[z->current];
    if (folder == (UInt32)-1) {
        snprintf(be->detail, sizeof be->detail, "entry has no data stream");
        return UA_ERR_CORRUPT;
    }

    /* Offset of this file within its folder. */
    file_start = z->db.UnpackPositions[z->current]
               - z->db.UnpackPositions[z->db.FolderToFile[folder]];

    /* Reopen the folder when moving to a new one, or when this file starts
     * before where the decoder already is — the decoder cannot rewind, so the
     * only way back is to start the folder again. */
    if (!z->folder_open || z->folder != folder || FOLDER_POS(z) > file_start) {
        r = folder_begin(be, z, folder);
        if (r != UA_OK) return r;
    }

    r = folder_skip_to(be, z, file_start);
    if (r != UA_OK) return r;

    while (want > 0) {
        size_t avail, n;

        r = folder_fill(be, z);
        if (r != UA_OK) return r;

        avail = z->out_have - z->out_pos;
        if (avail == 0) {
            snprintf(be->detail, sizeof be->detail,
                     "archive ends %llu bytes before the entry does",
                     (unsigned long long)want);
            return UA_ERR_CORRUPT;
        }

        n = ((UInt64)avail > want) ? (size_t)want : avail;

        r = sink(ud, z->out_buf + z->out_pos, n);
        if (r != UA_OK) return r;

        /* Only what the sink took is consumed; the rest stays buffered for the
         * next entry in this folder. */
        z->out_pos += n;
        want -= n;
    }

    return UA_OK;
}

static void sevenz_close(ua_backend *be)
{
    sevenz_impl *z = (sevenz_impl *)be->impl;

    if (!z) return;

    folder_end(z);
    if (z->db_open) SzArEx_Free(&z->db, &z->alloc);
    if (z->stream.fp) fclose(z->stream.fp);

    free(z->look_buf);
    free(z->in_buf);
    free(z->out_buf);
    free(z);

    be->impl = NULL;
}

/* ---- parallel decode -------------------------------------------------
 *
 * An LZMA2 stream is a series of chunks. Most depend on the dictionary window
 * the previous ones built, but a chunk with reset mode 3 resets the dictionary
 * and can therefore be decoded from cold. Those are the only points where the
 * stream can be split.
 *
 * Real archives have plenty: a 31 GB image archive measured 611 of them, about
 * one per 268 MB of output. So the ceiling on parallelism is the core count,
 * not the format.
 *
 * Only single-file folders are parallelised. A worker writes at an absolute
 * file offset, which is trivial when the folder is one file and fiddly when it
 * is many; the case that actually takes an hour is always the single huge
 * file, so the complexity is not worth carrying for the rest.
 */

typedef struct {
    UInt64 pack_off;     /* absolute offset in the archive */
    UInt64 unpack_off;   /* output offset within the folder */
} sevenz_split;

/* Walks chunk headers without decoding, collecting dictionary resets.
 *
 * Reading only headers means this costs one small read per chunk rather than
 * decompressing 31 GB to find out where the seams are. */
static ua_result scan_splits(ua_backend *be, sevenz_impl *z,
                             UInt64 pack_start, UInt64 pack_size,
                             sevenz_split **out, size_t *out_n)
{
    sevenz_split *list = NULL;
    size_t n = 0, cap = 0;
    UInt64 pos = pack_start;
    UInt64 end = pack_start + pack_size;
    UInt64 unpacked = 0;

    *out = NULL;
    *out_n = 0;

    while (pos < end) {
        const UInt64 chunk_start = pos;
        Byte head[6];
        size_t got = sizeof head;
        Int64 seek = (Int64)pos;
        unsigned control, reset;
        UInt64 u, p;

        if (stream_seek(&z->stream.vt, &seek, SZ_SEEK_SET) != SZ_OK) break;
        if (stream_read(&z->stream.vt, head, &got) != SZ_OK || got < 1) break;

        control = head[0];
        if (control == 0) break;                 /* end of stream */

        if (control == 1 || control == 2) {      /* uncompressed chunk */
            if (got < 3) break;
            u = ((UInt64)head[1] << 8 | head[2]) + 1;
            p = u;
            reset = (control == 1) ? 3 : 0;      /* control 1 also resets */
            pos += 3 + p;
        } else if (control >= 0x80) {            /* LZMA chunk */
            if (got < 5) break;
            u = (((UInt64)(control & 0x1f)) << 16)
                + ((UInt64)head[1] << 8 | head[2]) + 1;
            p = ((UInt64)head[3] << 8 | head[4]) + 1;
            reset = (control >> 5) & 3;

            /* A props byte follows when the state is reset. It carries the
             * LZMA lc/lp/pb settings, which the LZMA2 decoder reads from the
             * stream itself -- it is NOT the dictionary-size property that
             * Lzma2Dec_Allocate wants, and confusing the two makes allocation
             * fail. The dictionary size comes from the folder's coder info. */
            pos += (reset >= 2 ? 6 : 5) + p;
        } else {
            break;                               /* malformed */
        }

        /* Only a dictionary reset makes a chunk decodable from cold, and so
         * only those are candidate split points. */
        if (reset == 3) {
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 64;
                sevenz_split *g = (sevenz_split *)realloc(list, ncap * sizeof *g);
                if (!g) { free(list); return UA_ERR_NOMEM; }
                list = g;
                cap = ncap;
            }
            list[n].pack_off   = chunk_start;
            list[n].unpack_off = unpacked;
            n++;
        }

        unpacked += u;
    }

    (void)be;

    if (n == 0) { free(list); return UA_ERR_NOT_FOUND; }

    *out = list;
    *out_n = n;

    return UA_OK;
}

/* Written by every worker, read by the coordinating thread while they run. */
typedef struct {
    ua_mutex *lock;
    uint64_t  written;
    unsigned  finished;
} sevenz_shared;

typedef struct {
    const char *archive;
    ua_file    *out;          /* shared; positioned writes only */

    UInt64 pack_off;
    UInt64 unpack_off;
    UInt64 unpack_len;
    size_t buf_size;
    Byte   props;

    sevenz_shared *shared;

    ua_result result;
    char      detail[128];
} sevenz_task;

static void sevenz_worker(void *arg)
{
    sevenz_task *t = (sevenz_task *)arg;
    sevenz_stream st;
    CLzma2Dec dec;
    ISzAlloc alloc;
    Byte *inbuf = NULL, *outbuf = NULL;
    UInt64 produced = 0, write_at;
    size_t in_have = 0, in_pos = 0;
    int dec_ready = 0;

    t->result = UA_OK;
    alloc.Alloc = SzAlloc;
    alloc.Free = SzFree;

    st.fp = fopen(t->archive, "rb");
    if (!st.fp) {
        t->result = UA_ERR_IO;
        snprintf(t->detail, sizeof t->detail, "worker cannot open archive");
        return;
    }
    st.vt.Read = stream_read;
    st.vt.Seek = stream_seek;

    inbuf  = (Byte *)malloc(t->buf_size);
    outbuf = (Byte *)malloc(t->buf_size);
    if (!inbuf || !outbuf) { t->result = UA_ERR_NOMEM; goto done; }

    Lzma2Dec_Construct(&dec);
    if (Lzma2Dec_Allocate(&dec, t->props, &alloc) != SZ_OK) {
        t->result = UA_ERR_NOMEM;
        snprintf(t->detail, sizeof t->detail, "worker cannot allocate dictionary");
        goto done;
    }
    dec_ready = 1;
    Lzma2Dec_Init(&dec);

    {
        Int64 seek = (Int64)t->pack_off;
        if (stream_seek(&st.vt, &seek, SZ_SEEK_SET) != SZ_OK) {
            t->result = UA_ERR_IO;
            goto done;
        }
    }

    write_at = t->unpack_off;

    while (produced < t->unpack_len) {
        SizeT dst_len, src_len;
        ELzmaStatus status;
        UInt64 left = t->unpack_len - produced;

        if (in_pos == in_have) {
            in_have = t->buf_size;
            if (stream_read(&st.vt, inbuf, &in_have) != SZ_OK || in_have == 0) {
                t->result = UA_ERR_CORRUPT;
                snprintf(t->detail, sizeof t->detail, "worker input ended early");
                goto done;
            }
            in_pos = 0;
        }

        dst_len = (SizeT)t->buf_size;
        if ((UInt64)dst_len > left) dst_len = (SizeT)left;
        src_len = in_have - in_pos;

        if (Lzma2Dec_DecodeToBuf(&dec, outbuf, &dst_len, inbuf + in_pos,
                                 &src_len, LZMA_FINISH_ANY, &status) != SZ_OK) {
            t->result = UA_ERR_CORRUPT;
            snprintf(t->detail, sizeof t->detail, "worker decode failed");
            goto done;
        }

        in_pos += src_len;

        if (dst_len) {
            if (ua_plat_write_at(t->out, outbuf, dst_len, write_at) != UA_OK) {
                t->result = UA_ERR_IO;
                snprintf(t->detail, sizeof t->detail, "worker write failed");
                goto done;
            }
            write_at += dst_len;
            produced += dst_len;

            ua_plat_mutex_lock(t->shared->lock);
            t->shared->written += dst_len;
            ua_plat_mutex_unlock(t->shared->lock);
        } else if (src_len == 0) {
            t->result = UA_ERR_CORRUPT;
            snprintf(t->detail, sizeof t->detail, "worker stalled");
            goto done;
        }
    }

done:
    if (dec_ready) Lzma2Dec_Free(&dec, &alloc);
    free(inbuf);
    free(outbuf);
    fclose(st.fp);

    /* Last thing done, so the coordinator only stops polling once every
     * worker has released its handles. */
    ua_plat_mutex_lock(t->shared->lock);
    t->shared->finished++;
    ua_plat_mutex_unlock(t->shared->lock);
}

static ua_result sevenz_extract_parallel(ua_backend *be, const char *path,
                                         const ua_parallel_opts *po,
                                         uint64_t *written)
{
    unsigned threads = po->threads;

    /* The output is written through po->out now; the path is only still in the
     * signature because a backend without a shared descriptor might want it. */
    (void)path;
    sevenz_impl  *z = (sevenz_impl *)be->impl;
    sevenz_split *splits = NULL;
    sevenz_task  *tasks = NULL;
    ua_thread   **workers = NULL;
    sevenz_shared shared;
    CSzFolder     f;
    const Byte   *coder_data;
    sevenz_coder  coder;
    size_t        n_splits = 0;
    UInt64        pack_start, pack_size, folder_size;
    UInt32        folder;
    Byte          props = 0;
    unsigned      i, used;
    ua_result     r;

    if (written) *written = 0;
    memset(&shared, 0, sizeof shared);

    if (!z->have_current || threads < 2) return UA_ERR_NOT_FOUND;
    if (SzArEx_IsDir(&z->db, z->current)) return UA_ERR_NOT_FOUND;

    folder = z->db.FileToFolder[z->current];
    if (folder == (UInt32)-1) return UA_ERR_NOT_FOUND;

    /* Only a folder holding exactly this one file. */
    if (z->db.FolderToFile[folder] != z->current ||
        z->db.FolderToFile[folder + 1] != z->current + 1)
        return UA_ERR_NOT_FOUND;

    if (folder_classify(be, z, folder, &f, &coder_data, &coder) != UA_OK)
        return UA_ERR_NOT_FOUND;
    if (coder != CODER_LZMA2) return UA_ERR_NOT_FOUND;
    if (f.Coders[0].PropsSize != 1) return UA_ERR_NOT_FOUND;

    props = coder_data[f.Coders[0].PropsOffset];
    folder_size = SzAr_GetFolderUnpackSize(&z->db.db, folder);
    pack_start = SzArEx_GetFolderStreamPos(&z->db, folder, 0);
    if (SzArEx_GetFolderFullPackSize(&z->db, folder, &pack_size) != 0)
        return UA_ERR_NOT_FOUND;

    r = scan_splits(be, z, pack_start, pack_size, &splits, &n_splits);
    if (r != UA_OK || n_splits < 2) {
        free(splits);
        return UA_ERR_NOT_FOUND;   /* nothing to parallelise; caller streams */
    }

    if (threads > n_splits) threads = (unsigned)n_splits;

    /* Every worker allocates its own dictionary, so the real cost is
     * threads x window. A 256 MB window and eight workers would be 2 GB on a
     * console, which is not a trade anyone asked for; reduce the workers until
     * it fits the budget instead. */
    {
        UInt32 dict = (props >= 40)
                    ? 0xFFFFFFFFu
                    : (((UInt32)2 | (props & 1)) << (props / 2 + 11));
        uint64_t budget = (uint64_t)po->memory_budget_mb * 1024u * 1024u;
        unsigned fits;

        if (budget < dict) {
            snprintf(be->detail, sizeof be->detail,
                     "decode_memory_budget_mb is below the %u MB dictionary this "
                     "archive needs", (unsigned)(dict / (1024 * 1024)));
            free(splits);
            return UA_ERR_NOT_FOUND;   /* sequential path allocates just one */
        }

        fits = (unsigned)(budget / dict);
        if (threads > fits) threads = fits;
        if (threads < 2) { free(splits); return UA_ERR_NOT_FOUND; }
    }

    tasks   = (sevenz_task *)calloc(threads, sizeof *tasks);
    workers = (ua_thread **)calloc(threads, sizeof *workers);
    if (!tasks || !workers) { r = UA_ERR_NOMEM; goto cleanup; }

    if (ua_plat_mutex_create(&shared.lock) != UA_OK) { r = UA_ERR_NOMEM; goto cleanup; }

    /* Split the reset points into contiguous runs, one per worker. */
    for (i = 0; i < threads; i++) {
        size_t first = (size_t)((uint64_t)n_splits * i / threads);
        size_t next  = (size_t)((uint64_t)n_splits * (i + 1) / threads);

        tasks[i].archive        = z->archive_path;
        tasks[i].out           = po->out;
        tasks[i].pack_off       = splits[first].pack_off;
        tasks[i].unpack_off     = splits[first].unpack_off;
        tasks[i].unpack_len     = (next < n_splits ? splits[next].unpack_off
                                                   : folder_size)
                                  - splits[first].unpack_off;
        tasks[i].props          = props;
        tasks[i].buf_size       = z->buf_size;
        tasks[i].shared         = &shared;
        tasks[i].result         = UA_OK;
    }

    used = 0;
    for (i = 0; i < threads; i++) {
        if (tasks[i].unpack_len == 0) continue;
        if (ua_plat_thread_start(sevenz_worker, &tasks[i], &workers[used]) != UA_OK)
            break;
        used++;
    }

    if (used == 0) { r = UA_ERR_NOT_FOUND; goto cleanup; }

    /* Poll while the workers run rather than blocking straight into join.
     * A parallel decode of a large image takes many minutes, and reporting
     * nothing until it ends would be a step backwards from the sequential
     * path it replaces. */
    for (;;) {
        uint64_t done;
        unsigned fin;

        ua_plat_mutex_lock(shared.lock);
        done = shared.written;
        fin  = shared.finished;
        ua_plat_mutex_unlock(shared.lock);

        if (po->on_bytes) po->on_bytes(po->ud, done);
        if (fin >= used) break;

        ua_plat_sleep_ms(250);
    }

    for (i = 0; i < used; i++) ua_plat_thread_join(workers[i]);

    if (written) *written = shared.written;

    r = UA_OK;
    for (i = 0; i < threads; i++) {
        if (tasks[i].result != UA_OK) {
            r = tasks[i].result;
            snprintf(be->detail, sizeof be->detail, "%s", tasks[i].detail);
            break;
        }
    }

    /* Every worker was bounded by its own range, so overproduction is
     * structurally impossible; this catches a short one. */
    if (r == UA_OK && written && *written != folder_size) {
        snprintf(be->detail, sizeof be->detail,
                 "parallel decode produced %llu of %llu bytes",
                 (unsigned long long)*written, (unsigned long long)folder_size);
        r = UA_ERR_CORRUPT;
    }

cleanup:
    if (shared.lock) ua_plat_mutex_destroy(shared.lock);
    free(workers);
    free(tasks);
    free(splits);

    return r;
}

const ua_backend_vtable ua_backend_7z = {
    "7z/lzma-sdk",
    sevenz_open,
    sevenz_reset,
    sevenz_next,
    sevenz_extract_current,
    sevenz_close,
    sevenz_extract_parallel
};
