#include "sys.h"

#define ITERS 5000000u

volatile unsigned long hot_regs_sink;

int bench_main(void) {
    unsigned long a = 1;
    unsigned long b = 3;
    unsigned long c = 5;
    unsigned long d = 7;
    unsigned long e = 11;
    for (unsigned i = 0; i < ITERS; i++) {
        a += b + i;
        b ^= c + (a >> 7);
        c += d ^ (b << 3);
        d = (d + e) ^ (c >> 5);
        e += a ^ d;
    }
    hot_regs_sink = a ^ b ^ c ^ d ^ e;
    write_ulong(hot_regs_sink);
    sys_write(1, "\n", 1);
    return 0;
}
