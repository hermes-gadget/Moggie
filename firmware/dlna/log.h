/* log.h -- level-filtered logging to stderr. No syslog: the renderer is a
 * small endpoint daemon and stderr goes to the same place as sonosd's. */
#ifndef MOGGIE_DLNA_LOG_H
#define MOGGIE_DLNA_LOG_H

enum dlna_log_level {
    DLNA_LOG_ERROR = 0,
    DLNA_LOG_WARN  = 1,
    DLNA_LOG_INFO  = 2,
    DLNA_LOG_DEBUG = 3
};

void log_set_level(int level);
int  log_level(void);
void log_msg(int level, const char *fmt, ...);

#define log_error(...) log_msg(DLNA_LOG_ERROR, __VA_ARGS__)
#define log_warn(...)  log_msg(DLNA_LOG_WARN,  __VA_ARGS__)
#define log_info(...)  log_msg(DLNA_LOG_INFO,  __VA_ARGS__)
#define log_debug(...) log_msg(DLNA_LOG_DEBUG, __VA_ARGS__)

#endif /* MOGGIE_DLNA_LOG_H */
