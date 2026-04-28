#include "sys.h"

int bench_main(void) {
    unsigned long sum = 0;
    for (unsigned i = 0; i < 50000; i++) {
        if (sys_sched_yield() < 0)
            return 1;
        sum += i;
    }
    return sum == 0 ? 2 : 0;
}
