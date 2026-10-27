#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static int g_level = DLNA_LOG_INFO;

void log_set_level(int level)
{
    if (level < DLNA_LOG_ERROR)
        level = DLNA_LOG_ERROR;
    if (level > DLNA_LOG_DEBUG)
        level = DLNA_LOG_DEBUG;
    g_level = level;
}

int log_level(void)
{
    return g_level;
}

void log_msg(int level, const char *fmt, ...)
{
    static const char *const names[] = { "error", "warn", "info", "debug" };
    va_list ap;

    if (level > g_level)
        return;
    if (level < 0)
        level = 0;

    fprintf(stderr, "dlna: %s: ", names[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
