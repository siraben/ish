#include "sys.h"

#define SIZE (512 * 1024)
#define ROUNDS 64

static unsigned char src[SIZE];
static unsigned char dst[SIZE];

int bench_main(void) {
    for (unsigned i = 0; i < SIZE; i++)
        src[i] = (unsigned char) (i * 31u + i / 17u);

    unsigned checksum = 0;
    for (unsigned round = 0; round < ROUNDS; round++) {
        for (unsigned i = 0; i < SIZE; i++)
            dst[i] = (unsigned char) (src[i] + round);
        for (unsigned i = 0; i < SIZE; i += 64)
            checksum += dst[i];
    }

    write_u32(SIZE * ROUNDS + checksum);
    return 0;
}
