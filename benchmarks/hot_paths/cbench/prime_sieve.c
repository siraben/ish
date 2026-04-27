#include "sys.h"

#define LIMIT 1000000
static unsigned char composite[LIMIT + 1];

int bench_main(void) {
    unsigned count = 0;
    for (unsigned p = 2; p <= LIMIT; p++) {
        if (composite[p])
            continue;
        count++;
        if (p * p <= LIMIT) {
            for (unsigned q = p * p; q <= LIMIT; q += p)
                composite[q] = 1;
        }
    }
    write_u32(count);
    return 0;
}
