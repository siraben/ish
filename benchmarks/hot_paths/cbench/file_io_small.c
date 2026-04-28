#include "sys.h"

static char buf[512];
static char read_buf[512];

int bench_main(void) {
    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = (char) (i * 13u + i / 5u);

    long fd = sys_open_rw_create("/tmp/bench-hot-paths/c-file-io-small.bin");
    if (fd < 0)
        return 1;

    for (int i = 0; i < 8192; i++) {
        if (sys_write((int) fd, buf, sizeof(buf)) != (long) sizeof(buf))
            return 2;
    }
    sys_close((int) fd);

    fd = sys_open_ro("/tmp/bench-hot-paths/c-file-io-small.bin");
    if (fd < 0)
        return 3;
    unsigned checksum = 0;
    for (int i = 0; i < 8192; i++) {
        if (sys_read((int) fd, read_buf, sizeof(read_buf)) != (long) sizeof(read_buf))
            return 4;
        checksum += (unsigned char) read_buf[i & 511];
    }
    sys_close((int) fd);

    write_u32(8192u * 512u * 2u + checksum);
    return 0;
}
