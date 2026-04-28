#include "sys.h"

#define ITERS 1000000u

volatile unsigned long call_sink;

__attribute__((noinline)) static unsigned long mix_call(unsigned long a, unsigned long b) {
    return (a + 0x9e3779b97f4a7c15ul) ^ (b >> 3);
}

int bench_main(void) {
    unsigned long acc = 0x12345678ul;
    for (unsigned i = 0; i < ITERS; i++)
        acc = mix_call(acc, i);
    call_sink = acc;
    write_ulong(acc);
    sys_write(1, "\n", 1);
    return 0;
}
