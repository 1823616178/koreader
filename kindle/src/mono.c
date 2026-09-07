#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "mono.h"

#include <time.h>

long mono_now_s(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long)ts.tv_sec;
    return (long)time(NULL);
}

long mono_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    return (long)time(NULL) * 1000L;
}
