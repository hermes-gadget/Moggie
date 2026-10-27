/*
 * test_harness.h -- the whole test framework: three counters and an assert.
 *
 * No framework, no allocation, no dependencies. A failing check is recorded and
 * the run continues, so one bug does not hide the other nine.
 */
#ifndef SONOSD_TEST_HARNESS_H
#define SONOSD_TEST_HARNESS_H

extern int t_checks;
extern int t_failures;

void t_suite(const char *name);
void t_assert(int cond, const char *expr, const char *file, int line);
void t_eq_long(long want, long got, const char *expr, const char *file, int line);
void t_eq_str(const char *want, const char *got, const char *expr, const char *file, int line);
int  t_summary(void);

#define T_ASSERT(c)        t_assert((c) ? 1 : 0, #c, __FILE__, __LINE__)
#define T_EQ(want, got)    t_eq_long((long)(want), (long)(got), #got, __FILE__, __LINE__)
#define T_STREQ(want, got) t_eq_str((want), (got), #got, __FILE__, __LINE__)

#endif /* SONOSD_TEST_HARNESS_H */
