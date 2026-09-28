#include "debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int enabled;
static struct timespec started;

void debug_enable(void)
{
    clock_gettime(CLOCK_MONOTONIC, &started);
    enabled = 1;
}

int debug_enabled(void)
{
    return enabled;
}

void debug_log(const char *format, ...)
{
    struct timespec now;
    long seconds, nanos;
    va_list args;

    if (!enabled)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    seconds = now.tv_sec - started.tv_sec;
    nanos = now.tv_nsec - started.tv_nsec;
    if (nanos < 0) {
        seconds--;
        nanos += 1000000000L;
    }
    fprintf(stderr, "[debug +%ld.%03ld] ", seconds, nanos / 1000000L);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}
