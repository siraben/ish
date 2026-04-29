#include "sys.h"

int bench_main(void) {
    unsigned long sum = 0;
    for (unsigned i = 0; i < 1000000; i++) {
        sum += (unsigned long) sys_getpid();
        sum += (unsigned long) sys_getuid();
    }
    if (sum == 0)
        return 1;
    return 0;
}
