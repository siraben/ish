#include "sys.h"

static const char *paths[] = {
    "/usr/lib/libdefinitely-missing-a.so",
    "/usr/lib/libdefinitely-missing-b.so",
    "/usr/local/lib/python3.99/nope.py",
    "/etc/apk/cache/not-present",
    "/root/.config/not-present",
    "/tmp/bench-hot-paths/no-such-file",
    "/var/cache/no-such-index",
    "/opt/does-not-exist",
};

int bench_main(void) {
    char statbuf[256];
    unsigned long sum = 0;
    for (unsigned i = 0; i < 200000; i++) {
        const char *path = paths[i % (sizeof(paths) / sizeof(paths[0]))];
        long ret = sys_fstatat(AT_FDCWD, path, statbuf, AT_SYMLINK_NOFOLLOW);
        if (ret != -2)
            return 1;
        sum += (unsigned long) path[1] + i;
    }
    return sum == 0 ? 2 : 0;
}
