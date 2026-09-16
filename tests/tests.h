/*
 * emu-fw test framework: deliberately tiny, no dependencies.
 *
 * Each test is a function; tests are grouped into suites and registered with
 * the runner. The runner prints one PASS/FAIL line per test, details for
 * failures, and a summary; process exit code is the number of failed tests
 * (capped at 125).
 *
 * Tests must be independent of implementation internals: expected values are
 * derived from the hardware specification, never by calling emulator helper
 * functions. Allocation owners clean up after themselves.
 */
#ifndef EMU_TESTS_H
#define EMU_TESTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} t_test;

typedef struct {
    const char *suite;
    const t_test *tests;
    size_t count;
} t_suite;

/* Called by suite registration helpers; do not call directly. */
void t_add_suite(const t_suite *suite);

/* Runs all registered suites; returns number of failed tests. */
int t_run_all(void);

/* Assertions (usable only inside test functions). */
void t_fail(const char *what, const char *file, int line);
void t_check(int cond, const char *what, const char *file, int line);
void t_check_u64(uint64_t got, uint64_t want, const char *what,
                 const char *file, int line);
void t_check_i64(int64_t got, int64_t want, const char *what,
                 const char *file, int line);
void t_check_str(const char *got, const char *want, const char *what,
                 const char *file, int line);

#define T_CHECK(cond)        t_check((cond) ? 1 : 0, #cond, __FILE__, __LINE__)
#define T_CHECK_EQ(got, want)  t_check_i64((int64_t)(got), (int64_t)(want), \
                                           #got " == " #want, __FILE__, __LINE__)
#define T_CHECK_EQ_U(got, want) t_check_u64((uint64_t)(got), (uint64_t)(want), \
                                            #got " == " #want, __FILE__, __LINE__)
#define T_CHECK_STR(got, want) t_check_str((got), (want), \
                                           #got " == " #want, __FILE__, __LINE__)
#define T_FAIL(what)         t_fail(what, __FILE__, __LINE__)

/* Suite declaration helpers: each suite file exposes a register function. */
#define T_SUITE_BEGIN(name) \
    static const t_test name##_tests[]; \
    void t_register_##name(void) { \
        static const t_suite suite = { #name, name##_tests, \
                                       sizeof(name##_tests)/sizeof(name##_tests[0]) }; \
        t_add_suite(&suite); \
    } \
    static const t_test name##_tests[] = {

#define T_SUITE_END \
    { NULL, NULL } };

/* Core enablement macros (defined by CMake for enabled cores). */
#if defined(__cplusplus)
#error "test framework is C only"
#endif

#endif /* EMU_TESTS_H */
