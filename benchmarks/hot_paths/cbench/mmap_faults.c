#include "sys.h"

int bench_main(void) {
    const usize page = 4096;
    const usize size = 32 * 1024 * 1024;
    unsigned long sum = 0;
    for (unsigned round = 0; round < 8; round++) {
        long addr = sys_mmap_anon(size);
        if (addr < 0 && addr > -4096)
            return 1;
        volatile unsigned char *p = (volatile unsigned char *) addr;
        for (usize off = 0; off < size; off += page) {
            p[off] = (unsigned char) (round + off);
            sum += p[off];
        }
        if (sys_munmap((void *) addr, size) < 0)
            return 2;
    }
    if (sum == 0)
        return 3;
    return 0;
}
