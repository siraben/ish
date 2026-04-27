#include "sys.h"

#define ITERS 8000000u

volatile unsigned long branch_sink;

int bench_main(void) {
    unsigned long acc = 0;
    for (unsigned i = 0; i < ITERS; i++) {
        if ((i & 1) == 0)
            acc += i ^ 0x9e3779b97f4a7c15ul;
        else
            acc ^= i + 0xbf58476d1ce4e5b9ul;
    }
    branch_sink = acc;
    write_ulong(acc);
    sys_write(1, "\n", 1);
    return 0;
}
