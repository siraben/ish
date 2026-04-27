#include "sys.h"

int bench_main(void);
void _start(void) {
    struct bench_timespec start;
    struct bench_timespec end;
    if (sys_clock_gettime(CLOCK_MONOTONIC, &start) < 0)
        sys_exit(111);
    int status = bench_main();
    if (sys_clock_gettime(CLOCK_MONOTONIC, &end) < 0)
        sys_exit(112);
    write_elapsed(start, end);
    sys_exit(status);
}
