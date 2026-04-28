#include "sys.h"

#define BLOCK_SIZE (64 * 1024)
#define BLOCKS 256
#define OPS 512

static char write_buf[BLOCK_SIZE];
static char read_buf[BLOCK_SIZE];

static unsigned next_rand(unsigned *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

int bench_main(void) {
    for (unsigned i = 0; i < sizeof(write_buf); i++)
        write_buf[i] = (char) (i * 23u + i / 5u);

    long fd = sys_open_rw_create("/tmp/bench-hot-paths/c-file-pread-pwrite-random-64k.bin");
    if (fd < 0)
        return 1;

    for (int i = 0; i < BLOCKS; i++) {
        long off = (long) i * BLOCK_SIZE;
        if (sys_pwrite((int) fd, write_buf, sizeof(write_buf), off) != (long) sizeof(write_buf))
            return 2;
    }

    unsigned state = 0x12345678u;
    unsigned checksum = 0;
    for (int i = 0; i < OPS; i++) {
        unsigned block = next_rand(&state) % BLOCKS;
        write_buf[0] = (char) block;
        write_buf[1] = (char) (block >> 8);
        if (sys_pwrite((int) fd, write_buf, sizeof(write_buf), (long) block * BLOCK_SIZE) != (long) sizeof(write_buf))
            return 3;
    }

    state = 0x87654321u;
    for (int i = 0; i < OPS; i++) {
        unsigned block = next_rand(&state) % BLOCKS;
        if (sys_pread((int) fd, read_buf, sizeof(read_buf), (long) block * BLOCK_SIZE) != (long) sizeof(read_buf))
            return 4;
        checksum += (unsigned char) read_buf[i & (BLOCK_SIZE - 1)];
    }

    sys_close((int) fd);
    write_u32((BLOCKS + OPS * 2u) * BLOCK_SIZE + checksum);
    return 0;
}
