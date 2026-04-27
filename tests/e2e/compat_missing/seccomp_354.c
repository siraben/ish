#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __NR_seccomp
#define __NR_seccomp 354
#endif

#define SECCOMP_SET_MODE_FILTER 1

int main(void) {
    errno = 0;
    long ret = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, 0);
    if (ret != 0) {
        perror("seccomp");
        return 1;
    }

    puts("seccomp ok");
    return 0;
}
