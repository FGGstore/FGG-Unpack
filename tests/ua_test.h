/* Minimal assertion harness. Keeping it dependency-free means the same tests
 * build with MSVC on the host and with the PS5 SDK's clang on the console. */
#ifndef UA_TEST_H
#define UA_TEST_H

#include <stdio.h>
#include <string.h>

extern int ua_tests_run;
extern int ua_tests_failed;

#define UA_CHECK(cond, ...)                                                   \
    do {                                                                      \
        ua_tests_run++;                                                       \
        if (!(cond)) {                                                        \
            ua_tests_failed++;                                                \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define UA_EQ_INT(actual, expect, ...)                                        \
    do {                                                                      \
        long long a_ = (long long)(actual), e_ = (long long)(expect);         \
        ua_tests_run++;                                                       \
        if (a_ != e_) {                                                       \
            ua_tests_failed++;                                                \
            printf("  FAIL %s:%d: got %lld want %lld: ",                      \
                   __FILE__, __LINE__, a_, e_);                               \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define UA_EQ_STR(actual, expect)                                             \
    do {                                                                      \
        const char *a_ = (actual), *e_ = (expect);                            \
        ua_tests_run++;                                                       \
        if (strcmp(a_, e_) != 0) {                                            \
            ua_tests_failed++;                                                \
            printf("  FAIL %s:%d: got \"%s\" want \"%s\"\n",                  \
                   __FILE__, __LINE__, a_, e_);                               \
        }                                                                     \
    } while (0)

#endif /* UA_TEST_H */
