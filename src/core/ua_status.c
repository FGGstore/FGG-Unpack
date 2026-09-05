#include "ua_status.h"

#include "ua_log.h"
#include "../platform/ua_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ua_status {
    char           path[UA_MAX_PATH];
    ua_status_rec  rec[UA_STATUS_MAX];
    size_t         n;
};

/* Tab-separated, path last so it can contain anything but a tab or newline.
 * A leading version marker keeps the door open for the v2 resume offset
 * without having to guess at the format now. */
#define UA_STATUS_HEADER "# FGG Unpack queue.status v1"

static int same_key(const ua_status_rec *r, const char *path,
                    uint64_t size, int64_t mtime)
{
    return r->size == size && r->mtime == mtime && !strcmp(r->path, path);
}

static ua_status_rec *find(ua_status *s, const char *path,
                           uint64_t size, int64_t mtime)
{
    size_t i;

    for (i = 0; i < s->n; i++)
        if (same_key(&s->rec[i], path, size, mtime))
            return &s->rec[i];

    return NULL;
}

static void parse_line(ua_status *s, char *line)
{
    ua_status_rec r;
    char *field[5];
    size_t n = 0, len;
    char *p = line;

    memset(&r, 0, sizeof r);

    /* Exactly four tabs; the fifth field is the path and keeps any it holds. */
    while (n < 4) {
        char *tab = strchr(p, '\t');
        if (!tab) return;
        *tab = '\0';
        field[n++] = p;
        p = tab + 1;
    }
    field[4] = p;

    len = strlen(field[4]);
    if (len == 0 || len >= sizeof r.path) return;

    r.result       = (ua_result)atoi(field[0]);
    r.size         = strtoull(field[1], NULL, 10);
    r.mtime        = (int64_t)strtoll(field[2], NULL, 10);
    r.processed_at = (int64_t)strtoll(field[3], NULL, 10);
    memcpy(r.path, field[4], len + 1);

    if (s->n < UA_STATUS_MAX)
        s->rec[s->n++] = r;
}

ua_result ua_status_open(const char *path, ua_status **out)
{
    char line[UA_MAX_PATH + 128];
    ua_status *s;
    size_t n;
    FILE *fp;

    if (!path || !out) return UA_ERR_BADARG;
    *out = NULL;

    n = strlen(path);
    if (n == 0 || n >= UA_MAX_PATH) return UA_ERR_LIMIT;

    s = (ua_status *)calloc(1, sizeof *s);
    if (!s) return UA_ERR_NOMEM;
    memcpy(s->path, path, n + 1);

    fp = fopen(path, "rb");
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            size_t l = strlen(line);

            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
                line[--l] = '\0';

            if (l == 0 || line[0] == '#') continue;

            parse_line(s, line);
        }
        fclose(fp);
        UA_LOG_D("loaded %u status records from %s",
                 (unsigned)s->n, path);
    }

    *out = s;
    return UA_OK;
}

void ua_status_close(ua_status *s)
{
    free(s);
}

int ua_status_seen(const ua_status *s, const char *path,
                   uint64_t size, int64_t mtime)
{
    size_t i;

    if (!s || !path) return 0;

    for (i = 0; i < s->n; i++)
        if (same_key(&s->rec[i], path, size, mtime))
            return 1;

    return 0;
}

int ua_status_result_of(const ua_status *s, const char *path,
                        uint64_t size, int64_t mtime, ua_result *out)
{
    size_t i;

    if (!s || !path) return 0;

    for (i = 0; i < s->n; i++) {
        if (same_key(&s->rec[i], path, size, mtime)) {
            if (out) *out = s->rec[i].result;
            return 1;
        }
    }

    return 0;
}

static ua_result flush(ua_status *s)
{
    char tmp[UA_MAX_PATH + 8];
    size_t i;
    FILE *fp;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", s->path) >= (int)sizeof tmp)
        return UA_ERR_LIMIT;

    fp = fopen(tmp, "wb");
    if (!fp) return UA_ERR_IO;

    fprintf(fp, "%s\n", UA_STATUS_HEADER);

    for (i = 0; i < s->n; i++) {
        const ua_status_rec *r = &s->rec[i];

        fprintf(fp, "%d\t%llu\t%lld\t%lld\t%s\n",
                (int)r->result,
                (unsigned long long)r->size,
                (long long)r->mtime,
                (long long)r->processed_at,
                r->path);
    }

    if (ferror(fp)) { fclose(fp); remove(tmp); return UA_ERR_IO; }
    if (fclose(fp) != 0) { remove(tmp); return UA_ERR_IO; }

    /* Atomic: a crash leaves either the previous file or this one. */
    return ua_plat_replace(tmp, s->path);
}

ua_result ua_status_record(ua_status *s, const char *path, uint64_t size,
                           int64_t mtime, ua_result result)
{
    ua_status_rec *r;
    size_t len;

    if (!s || !path) return UA_ERR_BADARG;

    len = strlen(path);
    if (len == 0 || len >= UA_MAX_PATH) return UA_ERR_LIMIT;

    r = find(s, path, size, mtime);
    if (!r) {
        if (s->n == UA_STATUS_MAX) {
            /* Ring: drop the oldest so the file stays bounded. */
            memmove(&s->rec[0], &s->rec[1], (UA_STATUS_MAX - 1) * sizeof s->rec[0]);
            s->n--;
        }
        r = &s->rec[s->n++];
        memset(r, 0, sizeof *r);
        memcpy(r->path, path, len + 1);
        r->size  = size;
        r->mtime = mtime;
    }

    r->result       = result;
    r->processed_at = ua_plat_now();

    return flush(s);
}

void ua_status_forget(ua_status *s, const char *path)
{
    size_t i = 0;

    if (!s || !path) return;

    while (i < s->n) {
        if (!strcmp(s->rec[i].path, path)) {
            memmove(&s->rec[i], &s->rec[i + 1],
                    (s->n - i - 1) * sizeof s->rec[0]);
            s->n--;
            continue;
        }
        i++;
    }

    (void)flush(s);
}

size_t ua_status_count(const ua_status *s)
{
    return s ? s->n : 0;
}
