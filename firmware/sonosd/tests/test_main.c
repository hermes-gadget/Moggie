#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "tests.h"

#include <stdio.h>
#include <string.h>

int t_checks;
int t_failures;

static const char *g_suite = "(none)";

void t_suite(const char *name)
{
    g_suite = name;
    printf("\n-- %s\n", name);
}

void t_assert(int cond, const char *expr, const char *file, int line)
{
    t_checks++;
    if (!cond) {
        t_failures++;
        printf("FAIL %s:%d [%s] %s\n", file, line, g_suite, expr);
    }
}

void t_eq_long(long want, long got, const char *expr, const char *file, int line)
{
    t_checks++;
    if (want != got) {
        t_failures++;
        printf("FAIL %s:%d [%s] %s: want %ld, got %ld\n",
               file, line, g_suite, expr, want, got);
    }
}

void t_eq_str(const char *want, const char *got, const char *expr, const char *file, int line)
{
    t_checks++;
    if (!want || !got || strcmp(want, got) != 0) {
        t_failures++;
        printf("FAIL %s:%d [%s] %s: want \"%s\", got \"%s\"\n",
               file, line, g_suite, expr, want ? want : "(null)", got ? got : "(null)");
    }
}

int t_summary(void)
{
    printf("\n%d checks, %d failure(s)\n", t_checks, t_failures);
    if (t_failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}

int main(void)
{
    printf("sonosd unit tests\n");

    suite_arbiter();
    suite_volume();
    suite_jsonlite();
    suite_config();
    suite_bootstate();

    return t_summary();
}
