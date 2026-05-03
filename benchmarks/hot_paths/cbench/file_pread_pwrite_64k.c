#include "sys.h"

#define BLOCK_SIZE (64 * 1024)
#define BLOCKS 256

static char write_buf[BLOCK_SIZE];
static char read_buf[BLOCK_SIZE];

int bench_main(void) {
    for (unsigned i = 0; i < sizeof(write_buf); i++)
        write_buf[i] = (char) (i * 17u + i / 13u);

    long fd = sys_open_rw_create("/tmp/bench-hot-paths/c-file-pread-pwrite-64k.bin");
    if (fd < 0)
        return 1;

    for (int i = 0; i < BLOCKS; i++) {
        long off = (long) i * BLOCK_SIZE;
        if (sys_pwrite((int) fd, write_buf, sizeof(write_buf), off) != (long) sizeof(write_buf))
            return 2;
    }

    unsigned checksum = 0;
    for (int i = 0; i < BLOCKS; i++) {
        long off = (long) i * BLOCK_SIZE;
        if (sys_pread((int) fd, read_buf, sizeof(read_buf), off) != (long) sizeof(read_buf))
            return 3;
        checksum += (unsigned char) read_buf[i & (BLOCK_SIZE - 1)];
    }

    sys_close((int) fd);
    write_u32(BLOCKS * BLOCK_SIZE * 2u + checksum);
    return 0;
}
