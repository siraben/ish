#include "sys.h"

static char buf[4096];

int bench_main(void) {
    long fd = sys_open_ro("/tmp/bench-hot-paths/dev/urandom");
    if (fd < 0)
        return 1;

    unsigned checksum = 0;
    for (unsigned i = 0; i < 4096; i++) {
        if (sys_read((int) fd, buf, sizeof(buf)) != (long) sizeof(buf))
            return 2;
        checksum += (unsigned char) buf[i & 4095];
    }

    sys_close((int) fd);
    write_u32(4096u * 4096u + checksum);
    return 0;
}
