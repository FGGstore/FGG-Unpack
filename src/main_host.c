/* Host command-line front end.
 *
 * Same extraction core as the payload, driven from a PC. Its purpose is to let
 * an archive be checked before it is ever copied to the console, which is the
 * cheapest place to find out that something will not work.
 *
 * By default it only inspects: detection plus preflight, writing nothing.
 * Extraction needs --extract, because the whole point of the preflight is to
 * find out what a job would do before it does it.
 */
#include "ua_common.h"
#include "core/ua_extract.h"
#include "core/ua_log.h"
#include "core/ua_path.h"
#include "backend/ua_detect.h"
#include "platform/ua_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf(
        "FGG Unpack (host build)\n"
        "\n"
        "  fgg-unpack <archive> [destination]     inspect only, writes nothing\n"
        "  fgg-unpack --extract <archive> [dest]  actually extract\n"
        "\n"
        "Options:\n"
        "  --extract        perform the extraction, not just the preflight\n"
        "  --overwrite      allow existing files at the destination to be replaced\n"
        "  --symlinks       extract symlink entries (still refused if they escape)\n"
        "  --no-limit       disable the decompression-ratio guard\n"
        "  --margin <mb>    free space required on top of the payload (default 1024)\n"
        "  --threads <n>    7z decode workers; 0 = auto, 1 = single-threaded\n"
        "  --mem <mb>       ceiling on workers x dictionary (default 1024)\n"
        "  -v               verbose log to stderr\n"
        "\n"
        "With no destination, the archive's own directory is used, matching the\n"
        "payload's in-place behaviour.\n");
}

static void print_size(const char *label, uint64_t bytes)
{
    if (bytes >= (1024ull * 1024 * 1024))
        printf("  %-22s %.2f GB\n", label, (double)bytes / 1073741824.0);
    else if (bytes >= 1024 * 1024)
        printf("  %-22s %.1f MB\n", label, (double)bytes / 1048576.0);
    else
        printf("  %-22s %llu bytes\n", label, (unsigned long long)bytes);
}

static void on_progress(void *ud, const ua_job_report *rep)
{
    (void)ud;

    /* Bytes, rate and time remaining rather than an entry count: the archives
     * this matters for hold one enormous entry, where "0/1 entries" would be
     * the only thing shown for an hour. */
    printf("  %3u%%  %8.2f / %.2f GB   %4llu MB/s   %us elapsed",
           rep->percent,
           (double)rep->bytes_done / 1073741824.0,
           (double)rep->bytes_total / 1073741824.0,
           (unsigned long long)(rep->bytes_per_sec / (1024 * 1024)),
           rep->elapsed_seconds);

    if (rep->eta_seconds)
        printf("   eta %us", rep->eta_seconds);

    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    ua_extract_opts opts;
    ua_job_report   rep;
    ua_format       fmt = UA_FMT_UNKNOWN;
    const char     *archive = NULL;
    const char     *dest = NULL;
    char            derived[UA_MAX_PATH];
    int             extract = 0, verbose = 0;
    int             i;
    ua_result       r;

    ua_extract_opts_defaults(&opts);
    opts.stability_wait_seconds = 0;   /* a file named on the command line is
                                        * there deliberately, not mid-upload */

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--extract"))        extract = 1;
        else if (!strcmp(a, "--overwrite")) opts.overwrite = 1;
        else if (!strcmp(a, "--symlinks"))  opts.allow_symlinks = 1;
        else if (!strcmp(a, "--no-limit"))  opts.max_expansion_ratio = 0;
        else if (!strcmp(a, "-v"))          verbose = 1;
        else if (!strcmp(a, "--margin") && i + 1 < argc)
            opts.min_free_margin_mb = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--threads") && i + 1 < argc)
            opts.decode_threads = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--mem") && i + 1 < argc)
            opts.decode_memory_budget_mb = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { printf("unknown option: %s\n\n", a); usage(); return 2; }
        else if (!archive) archive = a;
        else if (!dest)    dest = a;
        else { usage(); return 2; }
    }

    if (!archive) { usage(); return 2; }

    if (verbose) {
        ua_log_open("fgg-unpack-host.log", UA_LOG_DEBUG, 4 * 1024 * 1024);
        ua_log_set_echo(1);
    }

    if (!dest) {
        ua_path_dirname(archive, derived, sizeof derived);
        dest = derived[0] ? derived : ".";
    }

    printf("archive:     %s\n", archive);
    printf("destination: %s\n", dest);

    /* FR-1: by content. Reported separately from the backend so an
     * unimplemented-but-recognised format says so plainly. */
    r = ua_detect_file(archive, &fmt);
    printf("format:      %s\n", ua_format_name(fmt));
    if (r != UA_OK && fmt == UA_FMT_UNKNOWN) {
        printf("\nRefused: %s\n", ua_strerror(r));
        return 1;
    }

    printf("\npreflight:\n");
    r = ua_extract_preflight(archive, dest, &opts, &rep);

    print_size("archive size", rep.archive_size);
    if (rep.entries_total)
        printf("  %-22s %llu\n", "entries", (unsigned long long)rep.entries_total);
    if (rep.entries_total) {
        print_size("uncompressed", rep.bytes_total);
        if (rep.archive_size)
            printf("  %-22s %.2fx\n", "expansion",
                   (double)rep.bytes_total / (double)rep.archive_size);
    }

    /* Only meaningful once the space check actually ran. A preflight that
     * failed earlier — on a bad entry path, say — leaves these zero, and
     * printing "required 0 bytes" would read as a finding rather than as a
     * check that never happened. */
    if (rep.required) {
        print_size("free at destination", rep.free_before);
        print_size("required", rep.required);
    }

    if (r != UA_OK) {
        printf("\nRefused: %s\n", rep.detail[0] ? rep.detail : ua_strerror(r));
        if (rep.failed_entry[0])
            printf("  at entry: %s\n", rep.failed_entry);
        printf("  (%s)\n", ua_strerror(r));
        return 1;
    }

    printf("\npreflight passed\n");

    if (!extract) {
        printf("nothing written. Pass --extract to actually unpack.\n");
        return 0;
    }

    printf("\nextracting:\n");
    r = ua_extract_run(archive, dest, &opts, on_progress, NULL, &rep);

    if (r != UA_OK) {
        printf("\nFailed: %s\n", rep.detail[0] ? rep.detail : ua_strerror(r));
        if (rep.failed_entry[0])
            printf("  at entry: %s\n", rep.failed_entry);
        return 1;
    }

    printf("\ndone: %llu files, %llu directories",
           (unsigned long long)rep.files_written,
           (unsigned long long)rep.dirs_created);
    if (rep.symlinks_skipped)
        printf(", %llu symlinks skipped", (unsigned long long)rep.symlinks_skipped);
    printf("\n");
    print_size("written", rep.bytes_done);

    return 0;
}
