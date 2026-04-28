#include "sys.h"

#define BLOCK_SIZE (64 * 1024)
#define BLOCKS 256

static char src_buf[BLOCK_SIZE];
static char copy_buf[BLOCK_SIZE];

int bench_main(void) {
    for (unsigned i = 0; i < sizeof(src_buf); i++)
        src_buf[i] = (char) (i * 29u + i / 11u);

    long src = sys_open_rw_create("/tmp/bench-hot-paths/c-file-copy-64k-src.bin");
    if (src < 0)
        return 1;

    for (int i = 0; i < BLOCKS; i++) {
        if (sys_write((int) src, src_buf, sizeof(src_buf)) != (long) sizeof(src_buf))
            return 2;
    }
    sys_close((int) src);

    src = sys_open_ro("/tmp/bench-hot-paths/c-file-copy-64k-src.bin");
    if (src < 0)
        return 3;
    long dst = sys_open_rw_create("/tmp/bench-hot-paths/c-file-copy-64k-dst.bin");
    if (dst < 0)
        return 4;

    unsigned checksum = 0;
    for (int i = 0; i < BLOCKS; i++) {
        if (sys_read((int) src, copy_buf, sizeof(copy_buf)) != (long) sizeof(copy_buf))
            return 5;
        checksum += (unsigned char) copy_buf[i & (BLOCK_SIZE - 1)];
        if (sys_write((int) dst, copy_buf, sizeof(copy_buf)) != (long) sizeof(copy_buf))
            return 6;
    }

    sys_close((int) dst);
    sys_close((int) src);

    write_u32(BLOCKS * BLOCK_SIZE * 3u + checksum);
    return 0;
}
