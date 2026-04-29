#include "sys.h"

int bench_main(void) {
    int efd = (int) sys_eventfd2(0, O_NONBLOCK | O_CLOEXEC);
    if (efd < 0)
        return 1;
    int epfd = (int) sys_epoll_create1(O_CLOEXEC);
    if (epfd < 0)
        return 2;

    struct bench_epoll_event event = {.events = EPOLLIN, .data = 0x1234};
    if (sys_epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &event) < 0)
        return 3;

    unsigned long long one = 1;
    unsigned long long value = 0;
    struct bench_epoll_event out[1];
    for (unsigned i = 0; i < 50000; i++) {
        if (sys_write(efd, &one, sizeof(one)) != (long) sizeof(one))
            return 4;
        long ready = sys_epoll_wait0(epfd, out, 1, 0);
        if (ready != 1 || out[0].data != 0x1234)
            return 5;
        if (sys_read(efd, &value, sizeof(value)) != (long) sizeof(value))
            return 6;
    }
    sys_close(epfd);
    sys_close(efd);
    return 0;
}
