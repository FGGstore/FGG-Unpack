/* FR-3 path safety.
 *
 * Pure functions, no I/O, no allocation: this is the part of the payload most
 * likely to be handed hostile input, so it is also the part the host harness
 * can test exhaustively.
 */
#ifndef UA_PATH_H
#define UA_PATH_H

#include "../ua_common.h"

/* Validate one archive entry path and write a safe relative form to `out`.
 *
 * `raw_len` is passed explicitly because archive metadata may contain embedded
 * NUL bytes; a mismatch against strlen() is itself a rejection (FR-3).
 *
 * Rejects: empty paths, absolute paths ("/x", "\x"), drive-qualified paths
 * ("C:x"), UNC paths, any ".." component, embedded NULs, over-deep or
 * over-long results. Collapses "." and repeated separators, and normalises
 * "\" to "/" so a Windows-written archive cannot smuggle a separator past the
 * traversal check.
 */
ua_result ua_path_sanitize_entry(const char *raw, size_t raw_len,
                                 char *out, size_t out_sz);

/* Join an already-sanitised relative path onto `dest`. Re-checks containment
 * so a bug in the caller cannot turn into a write outside the destination. */
ua_result ua_path_join_checked(const char *dest, const char *rel,
                               char *out, size_t out_sz);

/* A symlink is safe only if its target stays inside the destination once
 * resolved against the link's own directory. `link_rel` is the sanitised path
 * of the link itself; `target` is the raw target string from the archive. */
ua_result ua_path_check_symlink(const char *link_rel, const char *target);

/* Strip the last component. Returns "" when there is no parent. */
void ua_path_dirname(const char *path, char *out, size_t out_sz);

/* Basename with any extension removed, used to derive a default destination
 * folder when a queue line omits "-> <dest>". Handles the ".tar.gz" and
 * ".tar.xz" double extensions. */
void ua_path_archive_stem(const char *archive_path, char *out, size_t out_sz);

#endif /* UA_PATH_H */
