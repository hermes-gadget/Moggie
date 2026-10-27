#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>

static int g_level = LOG_LEVEL_INFO;
static int g_syslog = 0;

static const char *level_name(int level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "error";
    case LOG_LEVEL_WARN:  return "warn";
    case LOG_LEVEL_INFO:  return "info";
    case LOG_LEVEL_DEBUG: return "debug";
    default:              return "?";
    }
}

static int level_syslog(int level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return LOG_ERR;
    case LOG_LEVEL_WARN:  return LOG_WARNING;
    case LOG_LEVEL_INFO:  return LOG_INFO;
    default:              return LOG_DEBUG;
    }
}

void log_init(int level, int to_syslog, const char *ident)
{
    g_level = level;
    g_syslog = to_syslog ? 1 : 0;
    if (g_syslog)
        openlog(ident ? ident : "sonosd", LOG_PID, LOG_DAEMON);
}

void log_set_level(int level)
{
    if (level < LOG_LEVEL_ERROR)
        level = LOG_LEVEL_ERROR;
    if (level > LOG_LEVEL_DEBUG)
        level = LOG_LEVEL_DEBUG;
    g_level = level;
}

int log_get_level(void)
{
    return g_level;
}

void log_write(int level, const char *file, int line, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    const char *base;
    struct timeval tv;
    struct tm tm;
    char stamp[24];

    if (level > g_level)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Strip the build directory from __FILE__ so a cross build does not leak
     * the builder's paths onto the device console. */
    base = strrchr(file, '/');
    base = base ? base + 1 : file;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));

    fprintf(stderr, "%s %-5s %s:%d %s\n", stamp, level_name(level), base, line, msg);
    if (g_syslog)
        syslog(level_syslog(level), "%s", msg);
}
