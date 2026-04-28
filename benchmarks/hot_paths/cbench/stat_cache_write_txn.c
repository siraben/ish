#include "sys.h"

#define PATH_COUNT 16
#define ROUNDS 12000

static const char *paths[PATH_COUNT] = {
    "/bin/sh",
    "/etc/passwd",
    "/etc/group",
    "/tmp/bench-hot-paths/stat-cache-a",
    "/tmp/bench-hot-paths/stat-cache-b",
    "/tmp/bench-hot-paths/stat-cache-c",
    "/tmp/bench-hot-paths/no-such-a",
    "/tmp/bench-hot-paths/no-such-b",
    "/usr/lib/libdefinitely-missing-a.so",
    "/usr/lib/libdefinitely-missing-b.so",
    "/root/.config/not-present-a",
    "/root/.config/not-present-b",
    "/var/cache/no-such-index-a",
    "/var/cache/no-such-index-b",
    "/opt/does-not-exist-a",
    "/opt/does-not-exist-b",
};

static char statbuf[256];

static int touch_keep(const char *path) {
    long fd = sys_open_rw_create_keep(path);
    if (fd < 0)
        return (int) fd;
    sys_close((int) fd);
    return 0;
}

int bench_main(void) {
    if (touch_keep("/tmp/bench-hot-paths/stat-cache-a") < 0)
        return 1;
    if (touch_keep("/tmp/bench-hot-paths/stat-cache-b") < 0)
        return 2;
    if (touch_keep("/tmp/bench-hot-paths/stat-cache-c") < 0)
        return 3;
    if (touch_keep("/tmp/bench-hot-paths/stat-cache-trigger") < 0)
        return 4;

    unsigned checksum = 0;
    for (int i = 0; i < ROUNDS; i++) {
        for (int j = 0; j < PATH_COUNT; j++) {
            long ret = sys_fstatat(AT_FDCWD, paths[j], statbuf, AT_SYMLINK_NOFOLLOW);
            if (ret < 0 && ret != -2)
                return 5;
            checksum += (unsigned char) paths[j][1] + (ret == 0 ? (unsigned char) statbuf[j & 127] : 0);
        }
        if (touch_keep("/tmp/bench-hot-paths/stat-cache-trigger") < 0)
            return 6;
    }

    write_u32(ROUNDS * PATH_COUNT + checksum);
    return 0;
}
