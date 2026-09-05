/* FGG Unpack — shared types and result codes.
 *
 * Everything under src/core and src/backend is portable C11 with no PS5
 * dependency, so the host test harness (tests/) exercises the same code that
 * ships in fgg-unpack.elf. Anything that needs the console goes behind
 * src/platform/ua_platform.h.
 */
#ifndef UA_COMMON_H
#define UA_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* FreeBSD PATH_MAX is 1024; the console filesystem inherits that. Entry paths
 * are bounded by the same value so a long name inside an archive can never
 * overflow a join against a legal destination. */
#define UA_MAX_PATH 1024

/* Guard against pathological archives before they become a denial of service.
 * Not in the requirements draft; see docs/spec-gaps.md. */
#define UA_MAX_PATH_COMPONENTS 64
#define UA_MAX_ENTRIES         2000000ull

typedef enum {
    UA_OK = 0,
    UA_ERR_IO,                 /* read/write/stat failed; check errno */
    UA_ERR_NOMEM,
    UA_ERR_BADARG,
    UA_ERR_UNSUPPORTED_FORMAT, /* FR-1 rejected the magic bytes */
    UA_ERR_CORRUPT,            /* archive truncated or self-inconsistent */
    UA_ERR_UNSAFE_PATH,        /* FR-3 violation */
    UA_ERR_NO_SPACE,           /* FR-2 free-space preflight failed */
    UA_ERR_EXISTS,             /* destination file present, overwrite=0 */
    UA_ERR_NOT_FOUND,          /* also: backend iterator exhausted */
    UA_ERR_NOT_STABLE,         /* source still being written */
    UA_ERR_LIMIT,              /* entry count / expansion ratio / path depth */
    UA_ERR_CANCELLED,
    UA_ERR_INTERNAL
} ua_result;

const char *ua_strerror(ua_result r);

/* Archive formats recognised by content, never by extension (FR-1). */
typedef enum {
    UA_FMT_UNKNOWN = 0,
    UA_FMT_ZIP,
    UA_FMT_TAR,
    UA_FMT_TAR_GZ,
    UA_FMT_TAR_XZ,
    UA_FMT_7Z
} ua_format;

const char *ua_format_name(ua_format f);

/* An open file, owned by the platform layer. Declared here rather than in
 * ua_platform.h because the backend interface passes one through without ever
 * needing to know what it is. */
typedef struct ua_file ua_file;

typedef enum {
    UA_ENTRY_FILE = 0,
    UA_ENTRY_DIR,
    UA_ENTRY_SYMLINK,
    UA_ENTRY_OTHER   /* device nodes, fifos, hard links: refused, not extracted */
} ua_entry_kind;

typedef struct {
    char          path[UA_MAX_PATH];        /* raw, as stored in the archive */
    char          link_target[UA_MAX_PATH]; /* symlinks only */
    uint64_t      size;                     /* uncompressed bytes */
    uint64_t      compressed_size;
    int64_t       mtime;                    /* unix seconds, 0 = not carried */
    ua_entry_kind kind;
} ua_entry;

#endif /* UA_COMMON_H */
