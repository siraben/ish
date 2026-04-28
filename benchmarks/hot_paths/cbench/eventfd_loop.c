#include "sys.h"

int bench_main(void) {
    int fd = (int) sys_eventfd2(0, 0);
    if (fd < 0)
        return 1;

    unsigned long long one = 1;
    unsigned long long value = 0;
    for (unsigned i = 0; i < 100000; i++) {
        if (sys_write(fd, &one, sizeof(one)) != (long) sizeof(one))
            return 2;
        if (sys_read(fd, &value, sizeof(value)) != (long) sizeof(value))
            return 3;
        if (value != 1)
            return 4;
    }
    sys_close(fd);
    return 0;
}
