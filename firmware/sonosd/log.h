/*
 * log.h -- fixed-buffer logging. No allocation, no printf in the hot path
 * beyond a single vsnprintf into a 256-byte stack buffer.
 */
#ifndef SONOSD_LOG_H
#define SONOSD_LOG_H

enum log_level {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3
};

/* to_syslog: 1 to also send to syslog(3) (busybox syslogd on the appliance). */
void log_init(int level, int to_syslog, const char *ident);
void log_set_level(int level);
int  log_get_level(void);

/* Internal entry point; use the macros below. */
void log_write(int level, const char *file, int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define log_error(...) log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

#endif /* SONOSD_LOG_H */
