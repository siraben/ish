#include "sys.h"

#define FDS 160
#define LOOPS 3000

int bench_main(void) {
    int fds[FDS];
    struct bench_pollfd pollfds[FDS];

    for (int i = 0; i < FDS; i++) {
        fds[i] = (int) sys_eventfd2(0, O_NONBLOCK | O_CLOEXEC);
        if (fds[i] < 0)
            return 1;
        pollfds[i].fd = fds[i];
        pollfds[i].events = POLLOUT;
        pollfds[i].revents = 0;
    }

    unsigned long ready_total = 0;
    for (unsigned i = 0; i < LOOPS; i++) {
        long ready = sys_poll0(pollfds, FDS);
        if (ready != FDS)
            return 2;
        ready_total += (unsigned long) ready;
    }

    for (int i = 0; i < FDS; i++)
        sys_close(fds[i]);
    return ready_total == (unsigned long) FDS * LOOPS ? 0 : 3;
}
