/* Platform layer for the console (FreeBSD-derived) and for a POSIX host.
 *
 * The PS5-specific pieces are confined to the UA_TARGET_PS5 blocks so a Linux
 * or macOS developer can run the same harness the console runs. */
#ifndef _WIN32

#include "ua_platform.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#ifdef UA_TARGET_PS5
/* Open question 2, resolved against the SDK.
 *
 * The entry point is exported by libkernel and libkernel_sys but has no header
 * in the SDK, so it is declared here exactly as ps5-payload-dev/shsrv declares
 * it — shsrv being the long-running-payload reference the requirements already
 * name in section 10.
 *
 * The argument is a fixed 3120-byte request struct, NOT a bare string: the
 * kernel reads the full structure regardless of what the length argument says,
 * so passing a short buffer reads off the end of it. */
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
#endif

struct ua_file {
    int fd;
};

struct ua_dir {
    DIR *d;
};

static ua_result errno_to_result(void)
{
    switch (errno) {
    case ENOENT:  return UA_ERR_NOT_FOUND;
    case EEXIST:  return UA_ERR_EXISTS;
    case ENOSPC:  return UA_ERR_NO_SPACE;
    case ENOMEM:  return UA_ERR_NOMEM;
    case ELOOP:   return UA_ERR_UNSAFE_PATH;   /* O_NOFOLLOW tripped */
    default:      return UA_ERR_IO;
    }
}

ua_result ua_plat_lstat(const char *path, ua_stat *st)
{
    struct stat s;

    if (!path || !st) return UA_ERR_BADARG;
    memset(st, 0, sizeof *st);

    if (lstat(path, &s) != 0) return errno_to_result();

    st->size       = (uint64_t)s.st_size;
    st->mtime      = (int64_t)s.st_mtime;
    st->is_dir     = S_ISDIR(s.st_mode) ? 1 : 0;
    st->is_symlink = S_ISLNK(s.st_mode) ? 1 : 0;

    return UA_OK;
}

ua_result ua_plat_mkdirs(const char *path)
{
    char buf[UA_MAX_PATH];
    size_t len, i;

    if (!path) return UA_ERR_BADARG;
    len = strlen(path);
    if (len == 0 || len >= sizeof buf) return UA_ERR_LIMIT;
    memcpy(buf, path, len + 1);

    for (i = 0; ; i++) {
        char saved;
        ua_stat st;

        if (buf[i] != '/' && buf[i] != '\0') continue;
        if (i == 0) continue;                 /* leading "/" of an absolute path */

        saved  = buf[i];
        buf[i] = '\0';

        if (ua_plat_lstat(buf, &st) == UA_OK) {
            /* Refuse to descend through a symlink or a non-directory: this is
             * what stops an entry extracted earlier from redirecting the rest
             * of the job outside the destination. */
            if (!st.is_dir || st.is_symlink) { buf[i] = saved; return UA_ERR_UNSAFE_PATH; }
        } else if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            ua_result r = errno_to_result();
            buf[i] = saved;
            return r;
        }

        buf[i] = saved;
        if (saved == '\0') break;
    }

    return UA_OK;
}

ua_result ua_plat_opendir(const char *path, ua_dir **out)
{
    ua_dir *h;
    DIR *d;

    if (!path || !out) return UA_ERR_BADARG;
    *out = NULL;

    d = opendir(path);
    if (!d) return errno_to_result();

    h = (ua_dir *)malloc(sizeof *h);
    if (!h) { closedir(d); return UA_ERR_NOMEM; }
    h->d = d;
    *out = h;

    return UA_OK;
}

ua_result ua_plat_readdir(ua_dir *d, ua_dirent *out)
{
    struct dirent *e;

    if (!d || !out) return UA_ERR_BADARG;

    for (;;) {
        size_t n;

        errno = 0;
        e = readdir(d->d);
        if (!e) return errno ? errno_to_result() : UA_ERR_NOT_FOUND;

        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;

        n = strlen(e->d_name);
        if (n >= sizeof out->name) continue;   /* cannot be represented; skip */

        memcpy(out->name, e->d_name, n + 1);

        /* d_type is not universally populated; DT_UNKNOWN means the caller has
         * to stat if it needs to know, which the scanner does anyway. */
#ifdef DT_DIR
        out->is_dir     = (e->d_type == DT_DIR);
        out->is_symlink = (e->d_type == DT_LNK);
#else
        out->is_dir     = 0;
        out->is_symlink = 0;
#endif
        return UA_OK;
    }
}

void ua_plat_closedir(ua_dir *d)
{
    if (!d) return;
    closedir(d->d);
    free(d);
}

ua_result ua_plat_open_write(const char *path, int overwrite, ua_file **out)
{
    ua_file *f;
    int flags, fd;

    if (!path || !out) return UA_ERR_BADARG;
    *out = NULL;

    /* O_NOFOLLOW is the whole point: without it, a symlink entry extracted
     * earlier in the same archive could redirect this write anywhere on the
     * console filesystem. */
    flags = O_WRONLY | O_CREAT | O_NOFOLLOW;
    flags |= overwrite ? O_TRUNC : O_EXCL;

    fd = open(path, flags, 0644);
    if (fd < 0) return errno_to_result();

    f = (ua_file *)malloc(sizeof *f);
    if (!f) { close(fd); return UA_ERR_NOMEM; }
    f->fd = fd;
    *out = f;

    return UA_OK;
}

ua_result ua_plat_write(ua_file *f, const void *buf, size_t len)
{
    const char *p = (const char *)buf;

    if (!f || (!buf && len)) return UA_ERR_BADARG;

    while (len > 0) {
        ssize_t n = write(f->fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_to_result();
        }
        if (n == 0) return UA_ERR_IO;
        p   += (size_t)n;
        len -= (size_t)n;
    }

    return UA_OK;
}

ua_result ua_plat_close(ua_file *f)
{
    ua_result r = UA_OK;

    if (!f) return UA_ERR_BADARG;
    if (close(f->fd) != 0) r = errno_to_result();
    free(f);

    return r;
}

void ua_plat_set_mtime(const char *path, int64_t unix_seconds)
{
    struct utimbuf t;

    if (!path || unix_seconds <= 0) return;

    t.actime  = (time_t)unix_seconds;
    t.modtime = (time_t)unix_seconds;
    (void)utime(path, &t);
}

ua_result ua_plat_free_space(const char *dir, uint64_t *out_bytes)
{
    struct statvfs vfs;

    if (!dir || !out_bytes) return UA_ERR_BADARG;
    *out_bytes = 0;

    if (statvfs(dir, &vfs) != 0) return errno_to_result();

    /* f_bavail is what an unprivileged writer can actually use; f_bfree
     * includes the reserved pool and would over-report.
     *
     * Open question 1, partly resolved: statvfs and fstatvfs are declared in
     * the SDK headers and exported by libSceLibcInternal, so this links. If it
     * ever does not, statfs from <sys/mount.h> is the drop-in the SDK's own
     * mntinfo sample uses, with f_bavail * f_bsize. Whether either reports
     * correctly for /mnt/usb* still needs real hardware. */
    *out_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;

    return UA_OK;
}

ua_result ua_plat_symlink(const char *target, const char *link_path)
{
    if (!target || !link_path) return UA_ERR_BADARG;
    if (symlink(target, link_path) != 0) return errno_to_result();
    return UA_OK;
}

ua_result ua_plat_unlink(const char *path)
{
    if (!path) return UA_ERR_BADARG;
    if (unlink(path) != 0) {
        if (rmdir(path) != 0) return errno_to_result();
    }
    return UA_OK;
}

/* ---- threading ------------------------------------------------------- */

struct ua_thread {
    pthread_t     id;
    ua_thread_fn  fn;
    void         *arg;
};

struct ua_mutex {
    pthread_mutex_t m;
};

static void *thread_trampoline(void *p)
{
    ua_thread *t = (ua_thread *)p;
    t->fn(t->arg);
    return NULL;
}

ua_result ua_plat_thread_start(ua_thread_fn fn, void *arg, ua_thread **out)
{
    ua_thread *t;

    if (!fn || !out) return UA_ERR_BADARG;
    *out = NULL;

    t = (ua_thread *)malloc(sizeof *t);
    if (!t) return UA_ERR_NOMEM;

    t->fn = fn;
    t->arg = arg;

    if (pthread_create(&t->id, NULL, thread_trampoline, t) != 0) {
        free(t);
        return UA_ERR_IO;
    }

    *out = t;
    return UA_OK;
}

void ua_plat_thread_join(ua_thread *t)
{
    if (!t) return;
    pthread_join(t->id, NULL);
    free(t);
}

ua_result ua_plat_mutex_create(ua_mutex **out)
{
    ua_mutex *m;

    if (!out) return UA_ERR_BADARG;
    *out = NULL;

    m = (ua_mutex *)malloc(sizeof *m);
    if (!m) return UA_ERR_NOMEM;

    if (pthread_mutex_init(&m->m, NULL) != 0) { free(m); return UA_ERR_IO; }

    *out = m;
    return UA_OK;
}

void ua_plat_mutex_lock(ua_mutex *m)   { if (m) pthread_mutex_lock(&m->m); }
void ua_plat_mutex_unlock(ua_mutex *m) { if (m) pthread_mutex_unlock(&m->m); }

void ua_plat_mutex_destroy(ua_mutex *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

int ua_plat_cpu_count(void)
{
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
#endif
    return 1;
}

ua_result ua_plat_open_rw(const char *path, ua_file **out)
{
    ua_file *f;
    int fd;

    if (!path || !out) return UA_ERR_BADARG;
    *out = NULL;

    /* No O_TRUNC and no O_CREAT: the file is created by the main thread first,
     * and every worker only writes inside its own range. */
    fd = open(path, O_WRONLY | O_NOFOLLOW);
    if (fd < 0) return errno_to_result();

    f = (ua_file *)malloc(sizeof *f);
    if (!f) { close(fd); return UA_ERR_NOMEM; }
    f->fd = fd;
    *out = f;

    return UA_OK;
}

ua_result ua_plat_write_at(ua_file *f, const void *buf, size_t len,
                           uint64_t offset)
{
    const char *p = (const char *)buf;

    if (!f || (!buf && len)) return UA_ERR_BADARG;

    while (len > 0) {
        ssize_t n = pwrite(f->fd, p, len, (off_t)offset);

        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_to_result();
        }
        if (n == 0) return UA_ERR_IO;

        p      += (size_t)n;
        len    -= (size_t)n;
        offset += (uint64_t)n;
    }

    return UA_OK;
}

ua_result ua_plat_replace(const char *tmp, const char *dst)
{
    if (!tmp || !dst) return UA_ERR_BADARG;

    /* POSIX rename() over an existing file is atomic within a filesystem. */
    if (rename(tmp, dst) != 0) return errno_to_result();

    return UA_OK;
}

int64_t ua_plat_now(void)
{
    return (int64_t)time(NULL);
}

void ua_plat_sleep_ms(int ms)
{
    struct timespec ts;

    if (ms <= 0) return;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void ua_plat_notify(const char *msg)
{
    if (!msg) return;

#ifdef UA_TARGET_PS5
    {
        notify_request_t req;

        memset(&req, 0, sizeof req);
        /* Truncates rather than overflows; a notification is not worth a crash
         * on a payload whose first non-functional requirement is stability. */
        snprintf(req.message, sizeof req.message, "%s", msg);

        sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    }
#else
    printf("[notify] %s\n", msg);
    fflush(stdout);
#endif
}

#endif /* !_WIN32 */
