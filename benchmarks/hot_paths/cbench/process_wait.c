#include "sys.h"

int bench_main(void) {
    unsigned long sum = 0;
    for (unsigned i = 0; i < 250; i++) {
        long pid = sys_fork_like();
        if (pid < 0)
            return 1;
        if (pid == 0)
            sys_exit(0);

        int status = 0;
        long waited = sys_wait4(pid, &status, 0, 0);
        if (waited != pid)
            return 2;
        sum += (unsigned long) status + (unsigned long) waited;
    }
    return sum == 0 ? 3 : 0;
}
