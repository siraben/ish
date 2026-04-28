#include "sys.h"

int bench_main(void) {
    int fd = (int) sys_open_ro("/tmp/bench-hot-paths/dev/null");
    if (fd < 0)
        return 1;

    unsigned long sum = 0;
    for (unsigned i = 0; i < 200000; i++) {
        int dupfd = (int) sys_dup(fd);
        if (dupfd < 0)
            return 2;
        if (sys_fcntl(dupfd, F_SETFD, FD_CLOEXEC) < 0)
            return 3;
        long flags = sys_fcntl(dupfd, F_GETFD, 0);
        if (flags < 0)
            return 4;
        sum += (unsigned long) flags;
        if (sys_close(dupfd) < 0)
            return 5;
    }

    sys_close(fd);
    return sum == 0 ? 6 : 0;
}
