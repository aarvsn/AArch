/* emu-fw test runner: registers suites for enabled cores and runs them. */
#include "tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_MAX_SUITES 64

static t_suite g_suites[T_MAX_SUITES];
static size_t g_nsuites;

void t_add_suite(const t_suite *suite)
{
    if (g_nsuites >= T_MAX_SUITES) {
        /* overflow must never silently drop test suites */
        fprintf(stderr, "FATAL: suite capacity %d exceeded\n", T_MAX_SUITES);
        exit(1);
    }
    g_suites[g_nsuites++] = *suite;
}

static int g_failures;
static const char *g_current_suite;
static const char *g_current_test;
static int g_current_failed;

void t_fail(const char *what, const char *file, int line)
{
    t_check(0, what, file, line);
}

void t_check(int cond, const char *what, const char *file, int line)
{
    if (!cond) {
        if (!g_current_failed) {
            printf("  FAIL %s/%s\n", g_current_suite, g_current_test);
            g_current_failed = 1;
        }
        printf("    %s:%d: %s\n", file, line, what);
        g_failures++;
    }
}

void t_check_u64(uint64_t got, uint64_t want, const char *what,
                 const char *file, int line)
{
    if (got != want) {
        if (!g_current_failed) {
            printf("  FAIL %s/%s\n", g_current_suite, g_current_test);
            g_current_failed = 1;
        }
        printf("    %s:%d: %s\n      got  0x%016llX (%llu)\n      want 0x%016llX (%llu)\n",
               file, line, what,
               (unsigned long long)got, (unsigned long long)got,
               (unsigned long long)want, (unsigned long long)want);
        g_failures++;
    }
}

void t_check_i64(int64_t got, int64_t want, const char *what,
                 const char *file, int line)
{
    if (got != want) {
        if (!g_current_failed) {
            printf("  FAIL %s/%s\n", g_current_suite, g_current_test);
            g_current_failed = 1;
        }
        printf("    %s:%d: %s\n      got  %lld (0x%016llX)\n      want %lld (0x%016llX)\n",
               file, line, what,
               (long long)got, (unsigned long long)(uint64_t)got,
               (long long)want, (unsigned long long)(uint64_t)want);
        g_failures++;
    }
}

void t_check_str(const char *got, const char *want, const char *what,
                 const char *file, int line)
{
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        if (!g_current_failed) {
            printf("  FAIL %s/%s\n", g_current_suite, g_current_test);
            g_current_failed = 1;
        }
        printf("    %s:%d: %s\n      got  \"%s\"\n      want \"%s\"\n",
               file, line, what, got ? got : "(null)", want ? want : "(null)");
        g_failures++;
    }
}

static void run_suite(const t_suite *s)
{
    for (size_t i = 0; i < s->count; i++) {
        if (s->tests[i].name == NULL || s->tests[i].fn == NULL)
            break;
        g_current_suite = s->suite;
        g_current_test = s->tests[i].name;
        g_current_failed = 0;
        s->tests[i].fn();
        if (!g_current_failed)
            printf("  PASS %s/%s\n", s->suite, s->tests[i].name);
    }
}

int t_run_all(void)
{
    int total = 0;
    for (size_t i = 0; i < g_nsuites; i++) {
        const t_suite *s = &g_suites[i];
        size_t n = 0;
        while (n < s->count && s->tests[n].name != NULL)
            n++;
        printf("SUITE %s (%zu tests)\n", s->suite, n);
        run_suite(s);
        total += (int)n;
    }
    printf("------------------------------------------------------------\n");
    printf("total: %d tests, %d failed assertions\n", total, g_failures);
    return g_failures > 125 ? 125 : g_failures;
}

/* Suite registration (cores compiled in only when enabled). */
void t_register_common(void);
void t_register_state(void);
void t_register_romdetect(void);
void t_register_skeletons(void);
#if EMU_BUILD_MGBX
void t_register_mgbx(void);
#endif
#if EMU_BUILD_BEATLE_NES_REDUX
void t_register_nes(void);
#endif
#if EMU_BUILD_SUPERSNES
void t_register_supersnes(void);
#endif
#if EMU_BUILD_MGBAX
void t_register_mgbax(void);
#endif
#if EMU_BUILD_FINALBURN
void t_register_finalburn_m68k(void);
void t_register_finalburn_z80(void);
void t_register_finalburn_vdp(void);
void t_register_finalburn_audio(void);
void t_register_finalburn_state(void);
#endif

int main(void)
{
    t_register_common();
    t_register_state();
    t_register_romdetect();
    t_register_skeletons();
#if EMU_BUILD_MGBX
    t_register_mgbx();
#endif
#if EMU_BUILD_BEATLE_NES_REDUX
    t_register_nes();
#endif
#if EMU_BUILD_SUPERSNES
    t_register_supersnes();
#endif
#if EMU_BUILD_MGBAX
    t_register_mgbax();
#endif
#if EMU_BUILD_FINALBURN
    t_register_finalburn_m68k();
    t_register_finalburn_z80();
    t_register_finalburn_vdp();
    t_register_finalburn_audio();
    t_register_finalburn_state();
#endif
    return t_run_all();
}
