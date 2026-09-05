#include "ua_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE        *g_fp;
static char         g_path[UA_MAX_PATH];
static ua_log_level g_level    = UA_LOG_INFO;
static uint64_t     g_max      = 1024 * 1024;
static uint64_t     g_written;
static int          g_echo;

static const char *level_tag(ua_log_level l)
{
    switch (l) {
    case UA_LOG_ERROR: return "ERR ";
    case UA_LOG_WARN:  return "WARN";
    case UA_LOG_INFO:  return "INFO";
    case UA_LOG_DEBUG: return "DBG ";
    }
    return "????";
}

/* The payload can be started at any time and the jailbreak does not survive a
 * reboot, so the log is opened for append and its existing size counted
 * towards the rotation cap. */
static ua_result log_open_current(void)
{
    long pos;

    g_fp = fopen(g_path, "ab");
    if (!g_fp) return UA_ERR_IO;

    if (fseek(g_fp, 0, SEEK_END) == 0 && (pos = ftell(g_fp)) >= 0)
        g_written = (uint64_t)pos;
    else
        g_written = 0;

    return UA_OK;
}

static void log_rotate(void)
{
    char old[UA_MAX_PATH + 4];

    if (g_fp) { fclose(g_fp); g_fp = NULL; }

    snprintf(old, sizeof old, "%s.1", g_path);
    remove(old);
    rename(g_path, old);

    g_written = 0;
    (void)log_open_current();
}

ua_result ua_log_open(const char *path, ua_log_level level, uint64_t max_bytes)
{
    size_t len;

    if (!path) return UA_ERR_BADARG;

    len = strlen(path);
    if (len == 0 || len >= sizeof g_path) return UA_ERR_LIMIT;

    ua_log_close();

    memcpy(g_path, path, len + 1);
    g_level = level;
    g_max   = max_bytes ? max_bytes : (uint64_t)1024 * 1024;

    return log_open_current();
}

void ua_log_close(void)
{
    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }
    g_path[0] = '\0';
    g_written = 0;
}

void ua_log_set_echo(int enabled)
{
    g_echo = enabled;
}

void ua_log_write(ua_log_level level, const char *fmt, ...)
{
    char      line[1024];
    char      stamp[32];
    time_t    now;
    struct tm tmv;
    int       head, body;
    va_list   ap;

    if (level > g_level) return;
    if (!g_fp && !g_echo) return;

    now = time(NULL);
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    if (strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv) == 0)
        /* Zeros rather than question marks: "??-" is a trigraph and would be
         * converted to "~" under strict C11, quietly mangling the fallback. */
        strcpy(stamp, "0000-00-00 00:00:00");

    head = snprintf(line, sizeof line, "%s %s ", stamp, level_tag(level));
    if (head < 0 || (size_t)head >= sizeof line) return;

    va_start(ap, fmt);
    body = vsnprintf(line + head, sizeof line - (size_t)head, fmt, ap);
    va_end(ap);
    if (body < 0) return;

    if (g_echo) {
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }

    if (!g_fp) return;

    fprintf(g_fp, "%s\n", line);
    fflush(g_fp);   /* a payload crash must not lose the line that explains it */

    g_written += (uint64_t)head + (uint64_t)body + 1;
    if (g_written >= g_max) log_rotate();
}
