#include "sys.h"

#define PATH_COUNT 4096
#define ROUNDS 24

static char paths[PATH_COUNT][64];
static char statbuf[256];

static void init_path(unsigned index) {
    static const char prefix[] = "/tmp/bench-hot-paths/large-miss-";
    unsigned i = 0;
    for (; prefix[i] != '\0'; i++)
        paths[index][i] = prefix[i];

    unsigned value = index;
    char digits[16];
    unsigned n = 0;
    do {
        digits[n++] = (char) ('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n != 0)
        paths[index][i++] = digits[--n];
    paths[index][i] = '\0';
}

int bench_main(void) {
    for (unsigned i = 0; i < PATH_COUNT; i++)
        init_path(i);

    unsigned checksum = 0;
    for (int round = 0; round < ROUNDS; round++) {
        for (unsigned i = 0; i < PATH_COUNT; i++) {
            long ret = sys_fstatat(AT_FDCWD, paths[i], statbuf, AT_SYMLINK_NOFOLLOW);
            if (ret != -2)
                return 1;
            checksum += (unsigned char) paths[i][32] + round;
        }
    }

    write_u32(ROUNDS * PATH_COUNT + checksum);
    return 0;
}
