#include "sys.h"

#define FILE_SIZE (8u * 1024u * 1024u)
#define BLOCK_SIZE 4096u
#define BLOCKS (FILE_SIZE / BLOCK_SIZE)
#define RANDOM_WRITES 4096u

static char buf[BLOCK_SIZE];

static unsigned next_rand(unsigned *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

int bench_main(void) {
    long fd = sys_open_rw_create("/tmp/bench-hot-paths/c-file-random-write.bin");
    if (fd < 0)
        return 1;

    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = (char) (i * 29u + i / 11u);

    for (unsigned i = 0; i < BLOCKS; i++) {
        if (sys_write((int) fd, buf, sizeof(buf)) != (long) sizeof(buf))
            return 2;
    }

    unsigned state = 0x12345678u;
    unsigned checksum = 0;
    for (unsigned i = 0; i < RANDOM_WRITES; i++) {
        unsigned block = next_rand(&state) % BLOCKS;
        buf[0] = (char) block;
        buf[1] = (char) (block >> 8);
        if (sys_lseek((int) fd, (long) (block * BLOCK_SIZE), 0) < 0)
            return 3;
        if (sys_write((int) fd, buf, sizeof(buf)) != (long) sizeof(buf))
            return 4;
        checksum += block;
    }

    sys_close((int) fd);
    write_u32(FILE_SIZE + RANDOM_WRITES * BLOCK_SIZE + checksum);
    return 0;
}
