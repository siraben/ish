#include "sys.h"

#define ROUNDS 20000

static char statbuf[256];

int bench_main(void) {
    long fd = sys_open_rw_create("/tmp/bench-hot-paths/c-fstat-loop.bin");
    if (fd < 0)
        return 1;
    if (sys_write((int) fd, "x", 1) != 1)
        return 2;

    unsigned checksum = 0;
    for (int i = 0; i < ROUNDS; i++) {
        if (sys_fstat((int) fd, statbuf) < 0)
            return 3;
        checksum += (unsigned char) statbuf[i & 127];
    }
    sys_close((int) fd);

    write_u32(ROUNDS + checksum);
    return 0;
}
