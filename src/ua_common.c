#include "ua_common.h"

const char *ua_strerror(ua_result r)
{
    switch (r) {
    case UA_OK:                     return "ok";
    case UA_ERR_IO:                 return "i/o error";
    case UA_ERR_NOMEM:              return "out of memory";
    case UA_ERR_BADARG:             return "bad argument";
    case UA_ERR_UNSUPPORTED_FORMAT: return "unsupported archive format";
    case UA_ERR_CORRUPT:            return "archive corrupt or truncated";
    case UA_ERR_UNSAFE_PATH:        return "unsafe entry path";
    case UA_ERR_NO_SPACE:           return "not enough free space";
    case UA_ERR_EXISTS:             return "destination exists";
    case UA_ERR_NOT_FOUND:          return "not found";
    case UA_ERR_NOT_STABLE:         return "source still changing";
    case UA_ERR_LIMIT:              return "archive exceeds a safety limit";
    case UA_ERR_CANCELLED:          return "cancelled";
    case UA_ERR_INTERNAL:           return "internal error";
    }
    return "unknown error";
}

const char *ua_format_name(ua_format f)
{
    switch (f) {
    case UA_FMT_ZIP:    return "zip";
    case UA_FMT_TAR:    return "tar";
    case UA_FMT_TAR_GZ: return "tar.gz";
    case UA_FMT_TAR_XZ: return "tar.xz";
    case UA_FMT_7Z:     return "7z";
    case UA_FMT_UNKNOWN:
    default:            return "unknown";
    }
}
