#include "ua_config.h"

#include "ua_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The FTP upload target in practice, so it is what the payload watches out of
 * the box. Anything else is a watchpath= line in config.ini. */
#define UA_DEFAULT_WATCHPATH "/data/homebrew"

void ua_config_defaults(ua_config *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);

    c->debug                  = 1;
    c->scan_depth             = 1;
    c->poll_interval_seconds  = 10;
    c->stability_wait_seconds = 15;
    c->delete_after_extract   = 0;   /* deliberately off: see v2 4.4 */
    c->notify                 = 1;
    c->overwrite              = 0;
    c->min_free_margin_mb     = 1024;
    c->chunk_size_kb          = 256;
    c->http_port              = 9022;

    c->allow_symlinks         = 0;
    c->max_expansion_ratio    = 200;
    c->decode_threads         = 0;      /* auto: CPU count, capped */
    c->decode_memory_budget_mb = 1024;
    c->progress_percent_step  = 10;

    /* The default watch path is only used when the file names none. The first
     * watchpath= key replaces it rather than adding to it, so a user who
     * configures their own paths does not silently keep scanning ours. */
    snprintf(c->watchpath[0], sizeof c->watchpath[0], "%s", UA_DEFAULT_WATCHPATH);
    c->n_watchpath = 1;
}

/* ---- value parsing --------------------------------------------------- */

static int parse_uint(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    int digits = 0;

    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;

    for (; *s >= '0' && *s <= '9'; s++) {
        if (v > (0xffffffffffffffffULL - 9) / 10) return 0;   /* overflow */
        v = v * 10 + (unsigned long long)(*s - '0');
        digits++;
    }
    while (*s == ' ' || *s == '\t') s++;

    if (!digits || *s != '\0') return 0;

    *out = v;
    return 1;
}

/* Accepts 1/0, and also true/false/yes/no because people write those. */
static int parse_bool(const char *s, int *out)
{
    unsigned long long v;

    if (!s) return 0;

    if (parse_uint(s, &v)) { *out = v != 0; return 1; }

    if (!strcmp(s, "true")  || !strcmp(s, "yes") || !strcmp(s, "on"))  { *out = 1; return 1; }
    if (!strcmp(s, "false") || !strcmp(s, "no")  || !strcmp(s, "off")) { *out = 0; return 1; }

    return 0;
}

static ua_result set_uint(unsigned *dst, const char *value,
                          unsigned lo, unsigned hi)
{
    unsigned long long v;

    if (!parse_uint(value, &v)) return UA_ERR_BADARG;
    if (v < lo || v > hi) return UA_ERR_BADARG;

    *dst = (unsigned)v;
    return UA_OK;
}

ua_result ua_config_set(ua_config *c, const char *key, const char *value)
{
    unsigned long long v;

    if (!c || !key || !value) return UA_ERR_BADARG;

    if (!strcmp(key, "watchpath")) {
        size_t n = strlen(value);

        if (n == 0 || n >= UA_MAX_PATH) return UA_ERR_BADARG;
        if (c->n_watchpath >= UA_MAX_WATCHPATHS) return UA_ERR_LIMIT;

        /* The first configured path displaces the built-in default. */
        if (c->n_watchpath == 1 &&
            !strcmp(c->watchpath[0], UA_DEFAULT_WATCHPATH))
            c->n_watchpath = 0;

        memcpy(c->watchpath[c->n_watchpath], value, n + 1);
        c->n_watchpath++;
        return UA_OK;
    }

    if (!strcmp(key, "debug"))                return parse_bool(value, &c->debug) ? UA_OK : UA_ERR_BADARG;
    if (!strcmp(key, "notify"))               return parse_bool(value, &c->notify) ? UA_OK : UA_ERR_BADARG;
    if (!strcmp(key, "overwrite"))            return parse_bool(value, &c->overwrite) ? UA_OK : UA_ERR_BADARG;
    if (!strcmp(key, "delete_after_extract")) return parse_bool(value, &c->delete_after_extract) ? UA_OK : UA_ERR_BADARG;
    if (!strcmp(key, "allow_symlinks"))       return parse_bool(value, &c->allow_symlinks) ? UA_OK : UA_ERR_BADARG;

    if (!strcmp(key, "scan_depth"))            return set_uint(&c->scan_depth, value, 1, 2);
    if (!strcmp(key, "poll_interval_seconds")) return set_uint(&c->poll_interval_seconds, value, 1, 3600);
    if (!strcmp(key, "stability_wait_seconds"))return set_uint(&c->stability_wait_seconds, value, 0, 86400);
    if (!strcmp(key, "chunk_size_kb"))         return set_uint(&c->chunk_size_kb, value, 4, 8192);
    /* Deprecated: the web UI was cut from scope. Still parsed, silently, so an
     * existing config.ini does not start logging a warning every startup. */
    if (!strcmp(key, "http_port"))             return set_uint(&c->http_port, value, 0, 65535);
    if (!strcmp(key, "max_expansion_ratio"))   return set_uint(&c->max_expansion_ratio, value, 0, 1000000);
    if (!strcmp(key, "progress_percent_step")) return set_uint(&c->progress_percent_step, value, 0, 100);
    if (!strcmp(key, "decode_threads"))         return set_uint(&c->decode_threads, value, 0, 64);
    if (!strcmp(key, "decode_memory_budget_mb"))return set_uint(&c->decode_memory_budget_mb, value, 64, 65536);

    if (!strcmp(key, "min_free_margin_mb")) {
        if (!parse_uint(value, &v)) return UA_ERR_BADARG;
        if (v > 0xffffffffULL) return UA_ERR_BADARG;
        c->min_free_margin_mb = v;
        return UA_OK;
    }

    return UA_ERR_NOT_FOUND;   /* unknown key */
}

/* ---- file parsing ---------------------------------------------------- */

static void trim(char *s)
{
    size_t n = strlen(s);
    size_t i = 0;

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';

    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

ua_result ua_config_load(const char *path, ua_config *c, int *created)
{
    char line[UA_MAX_PATH + 128];
    unsigned lineno = 0;
    FILE *fp;

    if (!path || !c) return UA_ERR_BADARG;
    if (created) *created = 0;

    ua_config_defaults(c);

    fp = fopen(path, "rb");
    if (!fp) {
        /* v2 4.4: created from a bundled template on first run. Failing to
         * write it is not fatal — the defaults are already loaded. */
        if (ua_config_write_template(path) == UA_OK) {
            if (created) *created = 1;
            UA_LOG_I("wrote default config to %s", path);
        } else {
            UA_LOG_W("no config at %s and could not create one; using defaults", path);
        }
        return UA_OK;
    }

    while (fgets(line, sizeof line, fp)) {
        char *eq, *key, *value;
        ua_result r;

        lineno++;
        trim(line);

        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        /* Section headers are accepted and ignored: the key space is flat, but
         * writing [general] above it is a natural thing to do. */
        if (line[0] == '[') continue;

        eq = strchr(line, '=');
        if (!eq) {
            UA_LOG_W("config %s:%u: no '=', ignoring", path, lineno);
            continue;
        }

        *eq = '\0';
        key = line;
        value = eq + 1;
        trim(key);
        trim(value);

        r = ua_config_set(c, key, value);
        if (r == UA_ERR_NOT_FOUND)
            UA_LOG_W("config %s:%u: unknown key \"%s\", ignoring", path, lineno, key);
        else if (r != UA_OK)
            UA_LOG_W("config %s:%u: bad value for \"%s\": \"%s\", keeping default",
                     path, lineno, key, value);
    }

    fclose(fp);

    if (c->n_watchpath == 0) {
        UA_LOG_W("config %s defines no usable watchpath", path);
    }

    return UA_OK;
}

ua_result ua_config_write_template(const char *path)
{
    FILE *fp = fopen(path, "wb");

    if (!fp) return UA_ERR_IO;

    fprintf(fp,
        "# FGG Unpack configuration\n"
        "#\n"
        "# Every key is optional; anything omitted uses the value shown.\n"
        "# Unknown keys and unparseable values are logged and ignored rather\n"
        "# than preventing startup.\n"
        "\n"
        "# Verbose log entries in debug.log.\n"
        "debug=1\n"
        "\n"
        "# Directories scanned for archives. Repeat the key for more than one.\n"
        "# The first watchpath= replaces this default rather than adding to it.\n"
        "watchpath=%s\n"
        "\n"
        "# 1 = only files directly in a watch path, 2 = one nested level too.\n"
        "scan_depth=1\n"
        "\n"
        "# How often to rescan, in seconds (1-3600).\n"
        "poll_interval_seconds=10\n"
        "\n"
        "# An archive is ignored until its size and mtime have both been\n"
        "# unchanged for this long, so a file still arriving over FTP is not\n"
        "# grabbed half-written.\n"
        "stability_wait_seconds=15\n"
        "\n"
        "# Delete the archive after a verified successful extraction.\n"
        "# Off by default on purpose: deleting the source removes any ability\n"
        "# to retry, and the archive may be the only intact copy on the console.\n"
        "delete_after_extract=0\n"
        "\n"
        "# System notifications on job accepted, completed and failed.\n"
        "notify=1\n"
        "\n"
        "# Overwrite files that already exist at the destination.\n"
        "overwrite=0\n"
        "\n"
        "# Free space required on top of the estimated extraction size, in MB.\n"
        "min_free_margin_mb=1024\n"
        "\n"
        "# Read/write buffer size in KB.\n"
        "chunk_size_kb=256\n"
        "\n"
        "# --- safety limits (not in the requirements; see docs/spec-gaps.md)\n"
        "\n"
        "# Extract symlink entries. Off by default: a symlink is a way to\n"
        "# redirect entries extracted after it, and nothing here needs one.\n"
        "allow_symlinks=0\n"
        "\n"
        "# Refuse an archive that claims to expand to more than this multiple\n"
        "# of its own size. 0 disables the check.\n"
        "max_expansion_ratio=200\n"
        "\n"
        "# Emit a progress notification every N percent. 0 disables progress\n"
        "# notifications entirely.\n"
        "progress_percent_step=10\n",
        UA_DEFAULT_WATCHPATH);

    if (ferror(fp)) { fclose(fp); return UA_ERR_IO; }

    return fclose(fp) == 0 ? UA_OK : UA_ERR_IO;
}

void ua_config_to_extract_opts(const ua_config *c, ua_extract_opts *o)
{
    if (!c || !o) return;

    ua_extract_opts_defaults(o);

    o->overwrite              = c->overwrite;
    o->min_free_margin_mb     = c->min_free_margin_mb;
    o->chunk_size_kb          = c->chunk_size_kb;
    o->stability_wait_seconds = c->stability_wait_seconds;
    o->allow_symlinks         = c->allow_symlinks;
    o->max_expansion_ratio    = c->max_expansion_ratio;
    o->progress_percent_step  = c->progress_percent_step;
    o->decode_threads         = c->decode_threads;
    o->decode_memory_budget_mb = c->decode_memory_budget_mb;
}
