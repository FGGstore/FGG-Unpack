/* Everything the extractor needs from the operating system.
 *
 * The console is FreeBSD-derived, so ua_plat_posix.c covers both the PS5 build
 * and a Linux/macOS host; ua_plat_win.c exists only so the Section 8 test
 * harness runs on the developer machine. Console-specific behaviour that has
 * no host equivalent (notifications) degrades to a log line rather than an
 * error, so the same test can run in both places.
 */
#ifndef UA_PLATFORM_H
#define UA_PLATFORM_H

#include "../ua_common.h"

typedef struct {
    uint64_t size;
    int64_t  mtime;       /* unix seconds */
    int      is_dir;
    int      is_symlink;
} ua_stat;

/* Does not follow symlinks: a planted link must be visible as a link, not as
 * whatever it points at. */
ua_result ua_plat_lstat(const char *path, ua_stat *st);

/* Creates every missing component. Fails if an existing component is a symlink
 * or a non-directory, which is what stops an attacker-planted link from
 * redirecting the rest of the extraction. */
ua_result ua_plat_mkdirs(const char *path);

/* Directory enumeration, for the watch-path scan (v2 4.1). Entries come back
 * in whatever order the filesystem gives them; the caller sorts if it cares. */
typedef struct ua_dir ua_dir;

typedef struct {
    char name[UA_MAX_PATH];   /* basename only, never a path */
    int  is_dir;
    int  is_symlink;
} ua_dirent;

ua_result ua_plat_opendir(const char *path, ua_dir **out);

/* UA_ERR_NOT_FOUND marks the end of the directory, not a failure. "." and ".."
 * are filtered out here so no caller has to remember to. */
ua_result ua_plat_readdir(ua_dir *d, ua_dirent *out);
void      ua_plat_closedir(ua_dir *d);

/* ua_file itself is declared in ua_common.h, so the backend interface can pass
 * one along without pulling in the whole platform surface. */

/* Opens for writing without ever following a final symlink. `overwrite == 0`
 * fails with UA_ERR_EXISTS when the path is taken. */
ua_result ua_plat_open_write(const char *path, int overwrite, ua_file **out);
ua_result ua_plat_write(ua_file *f, const void *buf, size_t len);
ua_result ua_plat_close(ua_file *f);

/* Best-effort: a failure here never fails the job (FR-5 treats timestamps as
 * nice-to-have, unlike structure). */
void ua_plat_set_mtime(const char *path, int64_t unix_seconds);

/* Free bytes on the filesystem holding `dir`. `dir` must already exist.
 * Open question 1 in the requirements: this is the call to verify against
 * /mnt/usb* on real hardware. */
ua_result ua_plat_free_space(const char *dir, uint64_t *out_bytes);

/* Returns UA_ERR_UNSUPPORTED_FORMAT where the platform has no symlinks
 * (Windows without developer mode). Callers treat that as "skip", not "fail". */
ua_result ua_plat_symlink(const char *target, const char *link_path);

ua_result ua_plat_unlink(const char *path);

/* ---- threading -------------------------------------------------------
 *
 * Used only by the parallel 7z decoder. Everything else in the payload is
 * single-threaded, and FR-8 keeps it to one job at a time; these threads split
 * the work *within* one job, they do not run jobs concurrently.
 *
 * pthreads are available on the console: pthread.h ships in the SDK and
 * libkernel exports the symbols, which prospero-clang links by default.
 */
typedef struct ua_thread ua_thread;
typedef struct ua_mutex  ua_mutex;

typedef void (*ua_thread_fn)(void *arg);

ua_result ua_plat_thread_start(ua_thread_fn fn, void *arg, ua_thread **out);
void      ua_plat_thread_join(ua_thread *t);

ua_result ua_plat_mutex_create(ua_mutex **out);
void      ua_plat_mutex_lock(ua_mutex *m);
void      ua_plat_mutex_unlock(ua_mutex *m);
void      ua_plat_mutex_destroy(ua_mutex *m);

/* Online CPUs, or 1 when that cannot be determined. */
int ua_plat_cpu_count(void);

/* Opens an existing file for writing without truncating it, so several
 * workers can each hold their own handle and write disjoint ranges. */
ua_result ua_plat_open_rw(const char *path, ua_file **out);

/* Writes at an absolute offset without disturbing any other handle's position.
 * This is what makes parallel output safe: workers never share a file
 * position. */
ua_result ua_plat_write_at(ua_file *f, const void *buf, size_t len,
                           uint64_t offset);

/* Atomically replaces `dst` with `tmp`, overwriting whatever was there.
 *
 * This is what keeps queue.status intact across a crash (v2 section 7,
 * idempotent startup): a reader sees either the whole old file or the whole
 * new one, never a torn record. Both paths must be on the same filesystem. */
ua_result ua_plat_replace(const char *tmp, const char *dst);

int64_t ua_plat_now(void);
void    ua_plat_sleep_ms(int ms);

/* Console notification. Open question 2: the real API surface and its rate
 * limit are unknown, so callers must already be rate-limited before they get
 * here. On the host this prints to stdout. */
void ua_plat_notify(const char *msg);

#endif /* UA_PLATFORM_H */
