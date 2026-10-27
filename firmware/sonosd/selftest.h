/*
 * selftest.h -- in-process checks that never touch /dev or the network.
 *
 * Runs the real arbiter, the real volume state machine, the mock audio backend
 * and the mock MQTT backend, and reports one line per check. `sonosd --selftest`
 * is what CI and the first power-on after a flash should run.
 */
#ifndef SONOSD_SELFTEST_H
#define SONOSD_SELFTEST_H

/* Returns 0 when every check passed, 1 otherwise. */
int selftest_run(void);

#endif /* SONOSD_SELFTEST_H */
