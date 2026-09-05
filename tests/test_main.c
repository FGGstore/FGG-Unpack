#include "../src/core/ua_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ua_tests_run = 0;
int ua_tests_failed = 0;

void test_path_all(void);
void test_detect_all(void);
void test_extract_all(void);
void test_7z_all(void);
void test_config_all(void);
void test_driver_all(void);

int main(int argc, char **argv)
{
    int verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    /* The extractor logs through ua_log; sending it to a file keeps the test
     * output readable while leaving a full trace to inspect after a failure. */
    ua_log_open("build/tmp/test.log", verbose ? UA_LOG_DEBUG : UA_LOG_INFO,
                4 * 1024 * 1024);
    ua_log_set_echo(verbose);

    test_path_all();
    test_detect_all();
    test_extract_all();
    test_7z_all();
    test_config_all();
    test_driver_all();

    ua_log_close();

    printf("\n%d checks, %d failed\n", ua_tests_run, ua_tests_failed);
    if (ua_tests_failed == 0) printf("build/tmp/test.log has the job trace\n");

    return ua_tests_failed == 0 ? 0 : 1;
}
