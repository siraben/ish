#include "sys.h"

#define CHUNK_SIZE 4096
#define IOV_COUNT 8
#define ROUNDS 4096

static char write_buf[IOV_COUNT][CHUNK_SIZE];
static char read_buf[IOV_COUNT][CHUNK_SIZE];

int bench_main(void) {
    struct bench_iovec iov[IOV_COUNT];
    for (int i = 0; i < IOV_COUNT; i++) {
        for (int j = 0; j < CHUNK_SIZE; j++)
            write_buf[i][j] = (char) (i * 31 + j * 7);
        iov[i].base = (bench_addr_t) write_buf[i];
        iov[i].len = CHUNK_SIZE;
    }

    long fd = sys_open_rw_create("/tmp/bench-hot-paths/c-file-iov.bin");
    if (fd < 0)
        return 1;

    for (int i = 0; i < ROUNDS; i++) {
        if (sys_writev((int) fd, iov, IOV_COUNT) != CHUNK_SIZE * IOV_COUNT)
            return 2;
    }
    sys_close((int) fd);

    for (int i = 0; i < IOV_COUNT; i++) {
        iov[i].base = (bench_addr_t) read_buf[i];
        iov[i].len = CHUNK_SIZE;
    }

    fd = sys_open_ro("/tmp/bench-hot-paths/c-file-iov.bin");
    if (fd < 0)
        return 3;

    unsigned checksum = 0;
    for (int i = 0; i < ROUNDS; i++) {
        if (sys_readv((int) fd, iov, IOV_COUNT) != CHUNK_SIZE * IOV_COUNT)
            return 4;
        checksum += (unsigned char) read_buf[i & (IOV_COUNT - 1)][i & (CHUNK_SIZE - 1)];
    }
    sys_close((int) fd);

    write_u32(ROUNDS * IOV_COUNT * CHUNK_SIZE * 2u + checksum);
    return 0;
}
