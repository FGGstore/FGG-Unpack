/* Host-only platform layer (Section 9 test harness). Never compiled for the
 * console: the whole file is inert unless _WIN32 is defined, so the build can
 * list both platform sources unconditionally.
 *
 * Entry paths arriving from an archive are byte strings. The console stores
 * them verbatim, because FreeBSD filenames are bytes and nothing reinterprets
 * them. Windows filenames are UTF-16, and the ...A entry points convert from
 * the process ANSI codepage, so a UTF-8 name out of a zip would be decoded as
 * CP1252 and stored double-encoded ("café" becoming "cafÃ©"). That makes the
 * harness disagree with the console about what was extracted, which defeats
 * the point of having a harness, so every path here goes through the wide
 * entry points with an explicit UTF-8 decode.
 */
#ifdef _WIN32

#include "ua_platform.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ua_file {
    HANDLE h;
};

/* Windows epoch is 1601-01-01; unix epoch is 1970-01-01. */
#define UA_EPOCH_DELTA_100NS 116444736000000000LL

/* One UTF-8 byte never produces more than one UTF-16 code unit, so a path
 * bounded by UA_MAX_PATH bytes fits in as many wchar_t plus a terminator. */
typedef wchar_t ua_wpath[UA_MAX_PATH + 1];

/* Decodes a path for the wide Win32 entry points.
 *
 * A zip entry is not guaranteed to be UTF-8: names written without the
 * language-encoding flag are nominally CP437, and plenty of writers emit
 * whatever the local codepage was. The console writes those bytes unchanged
 * whatever they are, so refusing them here would make the harness stricter
 * than the target. Invalid UTF-8 therefore falls back to the ANSI codepage,
 * which at least produces a file, and the mismatch is a host artefact only. */
static int path_to_wide(const char *s, wchar_t *out, int out_chars)
{
    int n;

    if (!s) return 0;

    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, out_chars);
    if (n > 0) return 1;

    n = MultiByteToWideChar(CP_ACP, 0, s, -1, out, out_chars);
    return n > 0;
}

static int64_t filetime_to_unix(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart  = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (int64_t)((u.QuadPart - UA_EPOCH_DELTA_100NS) / 10000000ULL);
}

static ua_result last_error_to_result(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:  return UA_ERR_NOT_FOUND;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:  return UA_ERR_EXISTS;
    case ERROR_DISK_FULL:       return UA_ERR_NO_SPACE;
    default:                    return UA_ERR_IO;
    }
}

ua_result ua_plat_lstat(const char *path, ua_stat *st)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    ua_wpath w;

    if (!path || !st) return UA_ERR_BADARG;
    memset(st, 0, sizeof *st);

    if (!path_to_wide(path, w, (int)(sizeof w / sizeof w[0])))
        return UA_ERR_LIMIT;

    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d))
        return last_error_to_result();

    st->size       = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    st->mtime      = filetime_to_unix(&d.ftLastWriteTime);
    st->is_dir     = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->is_symlink = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

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

    for (i = 0; i < len; i++)
        if (buf[i] == '\\') buf[i] = '/';

    /* Start past any drive prefix so "D:" is never passed to CreateDirectory. */
    i = (len >= 2 && buf[1] == ':') ? 2 : 0;

    for (;; i++) {
        char saved;
        ua_stat st;
        ua_wpath w;

        if (buf[i] != '/' && buf[i] != '\0') continue;
        if (i == 0) continue;

        saved  = buf[i];
        buf[i] = '\0';

        if (ua_plat_lstat(buf, &st) == UA_OK) {
            /* An existing component that is not a real directory means someone
             * put something in the way; refuse rather than write through it. */
            if (!st.is_dir || st.is_symlink) { buf[i] = saved; return UA_ERR_UNSAFE_PATH; }
        } else if (!path_to_wide(buf, w, (int)(sizeof w / sizeof w[0]))) {
            buf[i] = saved;
            return UA_ERR_LIMIT;
        } else if (!CreateDirectoryW(w, NULL)) {
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                ua_result r = last_error_to_result();
                buf[i] = saved;
                return r;
            }
        }

        buf[i] = saved;
        if (saved == '\0') break;
    }

    return UA_OK;
}

struct ua_dir {
    HANDLE           h;
    WIN32_FIND_DATAW fd;
    int              pending;   /* FindFirstFile already produced an entry */
};

ua_result ua_plat_opendir(const char *path, ua_dir **out)
{
    wchar_t pattern[UA_MAX_PATH + 3];
    ua_wpath w;
    ua_dir *d;
    size_t n;

    if (!path || !out) return UA_ERR_BADARG;
    *out = NULL;

    if (!path_to_wide(path, w, (int)(sizeof w / sizeof w[0])))
        return UA_ERR_LIMIT;

    /* FindFirstFile enumerates via a wildcard rather than a directory handle. */
    n = wcslen(w);
    if (n + 3 >= sizeof pattern / sizeof pattern[0]) return UA_ERR_LIMIT;
    memcpy(pattern, w, n * sizeof(wchar_t));
    if (n > 0 && pattern[n - 1] != L'\\' && pattern[n - 1] != L'/')
        pattern[n++] = L'\\';
    pattern[n++] = L'*';
    pattern[n] = L'\0';

    d = (ua_dir *)malloc(sizeof *d);
    if (!d) return UA_ERR_NOMEM;

    d->h = FindFirstFileW(pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        ua_result r = last_error_to_result();
        free(d);
        return r;
    }
    d->pending = 1;
    *out = d;

    return UA_OK;
}

ua_result ua_plat_readdir(ua_dir *d, ua_dirent *out)
{
    if (!d || !out) return UA_ERR_BADARG;

    for (;;) {
        int n;

        if (!d->pending) {
            if (!FindNextFileW(d->h, &d->fd)) {
                if (GetLastError() == ERROR_NO_MORE_FILES) return UA_ERR_NOT_FOUND;
                return last_error_to_result();
            }
        }
        d->pending = 0;

        if (d->fd.cFileName[0] == L'.' &&
            (d->fd.cFileName[1] == L'\0' ||
             (d->fd.cFileName[1] == L'.' && d->fd.cFileName[2] == L'\0')))
            continue;

        /* Back to UTF-8, so the rest of the code sees the same byte strings it
         * would on the console. */
        n = WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1,
                                out->name, (int)sizeof out->name, NULL, NULL);
        if (n <= 0) continue;   /* cannot be represented; skip */

        out->is_dir     = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->is_symlink = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        return UA_OK;
    }
}

void ua_plat_closedir(ua_dir *d)
{
    if (!d) return;
    FindClose(d->h);
    free(d);
}

ua_result ua_plat_open_write(const char *path, int overwrite, ua_file **out)
{
    ua_file *f;
    ua_wpath w;
    HANDLE h;

    if (!path || !out) return UA_ERR_BADARG;
    *out = NULL;

    if (!path_to_wide(path, w, (int)(sizeof w / sizeof w[0])))
        return UA_ERR_LIMIT;

    /* FILE_FLAG_OPEN_REPARSE_POINT is the Windows equivalent of O_NOFOLLOW:
     * if the name is a reparse point we get a handle to the link itself and
     * the write fails, instead of silently writing through it. */
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL,
                    overwrite ? CREATE_ALWAYS : CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return last_error_to_result();

    f = (ua_file *)malloc(sizeof *f);
    if (!f) { CloseHandle(h); return UA_ERR_NOMEM; }
    f->h = h;
    *out = f;

    return UA_OK;
}

ua_result ua_plat_write(ua_file *f, const void *buf, size_t len)
{
    const char *p = (const char *)buf;

    if (!f || (!buf && len)) return UA_ERR_BADARG;

    while (len > 0) {
        DWORD chunk = (DWORD)(len > 0x10000000u ? 0x10000000u : len);
        DWORD wrote = 0;

        if (!WriteFile(f->h, p, chunk, &wrote, NULL)) return last_error_to_result();
        if (wrote == 0) return UA_ERR_IO;

        p   += wrote;
        len -= wrote;
    }

    return UA_OK;
}

ua_result ua_plat_close(ua_file *f)
{
    ua_result r = UA_OK;

    if (!f) return UA_ERR_BADARG;
    if (!FlushFileBuffers(f->h)) r = UA_ERR_IO;
    if (!CloseHandle(f->h)) r = UA_ERR_IO;
    free(f);

    return r;
}

void ua_plat_set_mtime(const char *path, int64_t unix_seconds)
{
    ua_wpath w;
    HANDLE h;
    ULARGE_INTEGER u;
    FILETIME ft;

    if (!path || unix_seconds <= 0) return;
    if (!path_to_wide(path, w, (int)(sizeof w / sizeof w[0]))) return;

    h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    u.QuadPart = (uint64_t)(unix_seconds * 10000000LL + UA_EPOCH_DELTA_100NS);
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

ua_result ua_plat_free_space(const char *dir, uint64_t *out_bytes)
{
    ULARGE_INTEGER avail;
    ua_wpath w;

    if (!dir || !out_bytes) return UA_ERR_BADARG;
    *out_bytes = 0;

    if (!path_to_wide(dir, w, (int)(sizeof w / sizeof w[0])))
        return UA_ERR_LIMIT;

    if (!GetDiskFreeSpaceExW(w, &avail, NULL, NULL))
        return last_error_to_result();

    *out_bytes = avail.QuadPart;

    return UA_OK;
}

ua_result ua_plat_symlink(const char *target, const char *link_path)
{
    (void)target;
    (void)link_path;
    /* Creating symlinks on Windows needs elevation or developer mode. The
     * default configuration skips symlink entries anyway, so the host harness
     * reports "unsupported" and the caller records a skip. */
    return UA_ERR_UNSUPPORTED_FORMAT;
}

ua_result ua_plat_unlink(const char *path)
{
    ua_wpath w;

    if (!path) return UA_ERR_BADARG;
    if (!path_to_wide(path, w, (int)(sizeof w / sizeof w[0])))
        return UA_ERR_LIMIT;

    if (!DeleteFileW(w)) {
        if (!RemoveDirectoryW(w)) return last_error_to_result();
    }
    return UA_OK;
}

/* ---- threading ------------------------------------------------------- */

struct ua_thread {
    HANDLE        h;
    ua_thread_fn  fn;
    void         *arg;
};

struct ua_mutex {
    CRITICAL_SECTION cs;
};

static DWORD WINAPI thread_trampoline(LPVOID p)
{
    ua_thread *t = (ua_thread *)p;
    t->fn(t->arg);
    return 0;
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

    t->h = CreateThread(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!t->h) { free(t); return UA_ERR_IO; }

    *out = t;
    return UA_OK;
}

void ua_plat_thread_join(ua_thread *t)
{
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

ua_result ua_plat_mutex_create(ua_mutex **out)
{
    ua_mutex *m;

    if (!out) return UA_ERR_BADARG;
    *out = NULL;

    m = (ua_mutex *)malloc(sizeof *m);
    if (!m) return UA_ERR_NOMEM;

    InitializeCriticalSection(&m->cs);
    *out = m;

    return UA_OK;
}

void ua_plat_mutex_lock(ua_mutex *m)   { if (m) EnterCriticalSection(&m->cs); }
void ua_plat_mutex_unlock(ua_mutex *m) { if (m) LeaveCriticalSection(&m->cs); }

void ua_plat_mutex_destroy(ua_mutex *m)
{
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

int ua_plat_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

ua_result ua_plat_open_rw(const char *path, ua_file **out)
{
    ua_file *f;
    ua_wpath w;
    HANDLE h;

    if (!path || !out) return UA_ERR_BADARG;
    *out = NULL;

    if (!path_to_wide(path, w, (int)(sizeof w / sizeof w[0])))
        return UA_ERR_LIMIT;

    /* FILE_SHARE_WRITE so several workers can hold their own handle at once;
     * they only ever write disjoint ranges. */
    h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return last_error_to_result();

    f = (ua_file *)malloc(sizeof *f);
    if (!f) { CloseHandle(h); return UA_ERR_NOMEM; }
    f->h = h;
    *out = f;

    return UA_OK;
}

ua_result ua_plat_write_at(ua_file *f, const void *buf, size_t len,
                           uint64_t offset)
{
    const char *p = (const char *)buf;

    if (!f || (!buf && len)) return UA_ERR_BADARG;

    while (len > 0) {
        DWORD chunk = (DWORD)(len > 0x10000000u ? 0x10000000u : len);
        DWORD wrote = 0;
        OVERLAPPED ov;

        /* On a synchronous handle an OVERLAPPED still supplies the offset,
         * which is the Windows equivalent of pwrite: the handle's own file
         * pointer is untouched, so concurrent workers cannot disturb one
         * another. */
        memset(&ov, 0, sizeof ov);
        ov.Offset     = (DWORD)(offset & 0xffffffffu);
        ov.OffsetHigh = (DWORD)(offset >> 32);

        if (!WriteFile(f->h, p, chunk, &wrote, &ov)) return last_error_to_result();
        if (wrote == 0) return UA_ERR_IO;

        p      += wrote;
        len    -= wrote;
        offset += wrote;
    }

    return UA_OK;
}

ua_result ua_plat_replace(const char *tmp, const char *dst)
{
    ua_wpath wtmp, wdst;

    if (!tmp || !dst) return UA_ERR_BADARG;

    if (!path_to_wide(tmp, wtmp, (int)(sizeof wtmp / sizeof wtmp[0])) ||
        !path_to_wide(dst, wdst, (int)(sizeof wdst / sizeof wdst[0])))
        return UA_ERR_LIMIT;

    /* Plain rename() fails on Windows when the destination exists.
     * MOVEFILE_REPLACE_EXISTING is the equivalent of the POSIX behaviour. */
    if (!MoveFileExW(wtmp, wdst, MOVEFILE_REPLACE_EXISTING))
        return last_error_to_result();

    return UA_OK;
}

int64_t ua_plat_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return filetime_to_unix(&ft);
}

void ua_plat_sleep_ms(int ms)
{
    Sleep((DWORD)(ms < 0 ? 0 : ms));
}

void ua_plat_notify(const char *msg)
{
    printf("[notify] %s\n", msg ? msg : "");
    fflush(stdout);
}

#endif /* _WIN32 */
