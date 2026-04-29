#include "sys.h"

int bench_main(void) {
    unsigned ruid = 0;
    unsigned euid = 0;
    unsigned suid = 0;
    unsigned long sum = 0;

    for (unsigned i = 0; i < 200000; i++) {
        long pid = sys_getpid();
        long ppid = sys_getppid();
        long tid = sys_gettid();
        long uid = sys_getuid();
        if (sys_getresuid(&ruid, &euid, &suid) < 0)
            return 1;
        sum += (unsigned long) pid + (unsigned long) ppid + (unsigned long) tid + (unsigned long) uid;
        sum += ruid + euid + suid;
    }
    return sum == 0 ? 2 : 0;
}
