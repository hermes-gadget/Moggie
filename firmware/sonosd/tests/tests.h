/* Suites and the entry point. Declared here so -Wmissing-prototypes is happy. */
#ifndef SONOSD_TESTS_H
#define SONOSD_TESTS_H

void suite_arbiter(void);
void suite_volume(void);
void suite_jsonlite(void);
void suite_config(void);
void suite_bootstate(void);

#endif /* SONOSD_TESTS_H */
