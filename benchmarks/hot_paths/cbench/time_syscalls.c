#include "sys.h"

int bench_main(void) {
    struct bench_timespec ts;
    struct bench_timeval tv;
    unsigned long sum = 0;
    for (unsigned i = 0; i < 250000; i++) {
        if (sys_clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
            return 1;
        if (sys_gettimeofday(&tv) < 0)
            return 2;
        sum += (unsigned long) ts.nsec + (unsigned long) tv.usec;
    }
    if (sum == 0)
        return 3;
    return 0;
}
