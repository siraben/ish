#include "sys.h"

#define FDS 384
#define ROUNDS 20000

int bench_main(void) {
    int fds[FDS];
    for (unsigned i = 0; i < FDS; i++) {
        fds[i] = (int) sys_open_ro("/tmp/bench-hot-paths/dev/null");
        if (fds[i] < 0)
            return 1;
    }

    unsigned long sum = 0;
    for (unsigned i = 0; i < ROUNDS; i++) {
        unsigned slot = FDS - 1 - (i & 7);
        if (sys_close(fds[slot]) < 0)
            return 2;
        fds[slot] = (int) sys_open_ro("/tmp/bench-hot-paths/dev/null");
        if (fds[slot] < 0)
            return 3;
        sum += (unsigned long) fds[slot];
    }

    for (unsigned i = 0; i < FDS; i++)
        sys_close(fds[i]);
    return sum == 0 ? 4 : 0;
}
