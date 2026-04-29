#include "sys.h"

int bench_main(void) {
    int futex_word = 1;
    unsigned errors = 0;
    for (unsigned i = 0; i < 250000; i++) {
        long res = sys_futex(&futex_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
        if (res != -11)
            errors++;
    }
    return errors == 0 ? 0 : 1;
}
