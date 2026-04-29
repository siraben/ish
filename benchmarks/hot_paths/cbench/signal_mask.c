#include "sys.h"

int bench_main(void) {
    unsigned long long empty = 0;
    unsigned long long old = 0;
    unsigned long sum = 0;

    for (unsigned i = 0; i < 200000; i++) {
        if (sys_rt_sigprocmask(SIG_BLOCK, &empty, &old, sizeof(old)) < 0)
            return 1;
        if (sys_rt_sigprocmask(SIG_SETMASK, &old, 0, sizeof(old)) < 0)
            return 2;
        sum += (unsigned long) old + i;
    }
    return sum == 0 ? 3 : 0;
}
