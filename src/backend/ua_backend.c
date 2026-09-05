#include "ua_backend.h"

#include <stdio.h>
#include <string.h>

ua_result ua_backend_open(ua_format fmt, const char *path,
                          size_t chunk_bytes, ua_backend *be)
{
    ua_result r;

    if (!path || !be) return UA_ERR_BADARG;

    memset(be, 0, sizeof *be);

    /* Clamped rather than trusted: the config allows 4 KB to 8 MB, but this is
     * also reachable from the host CLI and from tests. */
    if (chunk_bytes < 4096) chunk_bytes = 4096;
    if (chunk_bytes > 32u * 1024 * 1024) chunk_bytes = 32u * 1024 * 1024;
    be->chunk_bytes = chunk_bytes;

    switch (fmt) {
    case UA_FMT_ZIP:
        be->v = &ua_backend_zip;
        break;

    case UA_FMT_7Z:
        be->v = &ua_backend_7z;
        break;

    /* Still to come. Named individually so the log says which format was asked
     * for rather than a generic refusal. */
    case UA_FMT_TAR:
    case UA_FMT_TAR_GZ:
    case UA_FMT_TAR_XZ:
        snprintf(be->detail, sizeof be->detail,
                 "%s is not implemented in this build", ua_format_name(fmt));
        return UA_ERR_UNSUPPORTED_FORMAT;

    case UA_FMT_UNKNOWN:
    default:
        snprintf(be->detail, sizeof be->detail, "unrecognised archive format");
        return UA_ERR_UNSUPPORTED_FORMAT;
    }

    r = be->v->open(be, path);
    if (r != UA_OK) be->v = NULL;

    return r;
}

void ua_backend_close(ua_backend *be)
{
    if (!be || !be->v) return;

    be->v->close(be);
    be->v = NULL;
}
