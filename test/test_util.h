#ifndef KHB_TEST_UTIL_H
#define KHB_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "khabibdb.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
/*
 * Minimal test harness. Test functions must be `static void fn(void)` —
 * the CHECK macros return early on failure.
 */

static int khb_tests_run;
static int khb_tests_failed;
static int khb_current_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);  \
            khb_current_failed = 1;                                          \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long _a = (long long)(a);                                       \
        long long _b = (long long)(b);                                       \
        if (_a != _b) {                                                      \
            printf("  FAIL %s:%d: %s == %s  (%lld vs %lld)\n",               \
                   __FILE__, __LINE__, #a, #b, _a, _b);                      \
            khb_current_failed = 1;                                          \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_STATUS(expr, expected)                                         \
    do {                                                                     \
        khb_status _s = (expr);                                              \
        if (_s != (expected)) {                                              \
            printf("  FAIL %s:%d: %s -> %s, expected %s\n",                  \
                   __FILE__, __LINE__, #expr,                                \
                   khb_strerror(_s), khb_strerror(expected));                \
            khb_current_failed = 1;                                          \
            return;                                                          \
        }                                                                    \
    } while (0)

#define FAILF(...)                                                           \
    do {                                                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                        \
        printf(__VA_ARGS__);                                                 \
        printf("\n");                                                        \
        khb_current_failed = 1;                                              \
        return;                                                              \
    } while (0)

#define RUN_TEST(fn)                                                         \
    do {                                                                     \
        khb_current_failed = 0;                                              \
        khb_tests_run++;                                                     \
        printf("- %s\n", #fn);                                               \
        fn();                                                                \
        if (khb_current_failed) khb_tests_failed++;                          \
    } while (0)

#define TEST_SUMMARY()                                                       \
    (printf("%d run, %d failed\n", khb_tests_run, khb_tests_failed),         \
     khb_tests_failed ? 1 : 0)

/* ------------------------------------------------------- temp-file helpers */

/*
 * static inline, not plain static: an unused plain-static function in a
 * header trips -Wunused-function under -Werror in test files that don't
 * happen to call it.
 */

static inline const char *khb_tmpdir(void)
{
    const char *d = getenv("TMPDIR");
    return (d != NULL && d[0] != '\0') ? d : "/tmp";
}

/* Unique per tag, per process, per call — repeated or parallel runs never
 * collide on the same file. */
static inline void khb_temp_path(char *out, size_t cap, const char *tag)
{
    static int  seq;
    const char *d   = khb_tmpdir();
    size_t      n   = strlen(d);
    const char *sep = (n > 0 && d[n - 1] == '/') ? "" : "/";

    snprintf(out, cap, "%s%skhb_%s_%d_%d.db", d, sep, tag, (int)getpid(), seq++);
}

/* Removes the db and its journal sibling. The journal doesn't exist until
 * stage 4; handling it now means these tests need no edits later. */
static inline void khb_temp_remove(const char *path)
{
    char jrnl[PATH_MAX + 16];       /* room for the "-journal" suffix */
    unlink(path);
    snprintf(jrnl, sizeof jrnl, "%s-journal", path);
    unlink(jrnl);
}

#endif /* KHB_TEST_UTIL_H */