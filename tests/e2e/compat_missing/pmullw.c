#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint16_t a[8] __attribute__((aligned(16))) = {1, 2, 3, 4, 5, 6, 7, 8};
    uint16_t b[8] __attribute__((aligned(16))) = {2, 3, 4, 5, 6, 7, 8, 9};
    uint16_t out[8] __attribute__((aligned(16))) = {0};
    uint16_t expected[8] = {2, 6, 12, 20, 30, 42, 56, 72};

    __asm__ volatile(
            "movdqa %1, %%xmm0\n\t"
            "pmullw %2, %%xmm0\n\t"
            "movdqa %%xmm0, %0"
            : "=m"(out)
            : "m"(a), "m"(b)
            : "xmm0");

    if (memcmp(out, expected, sizeof(out)) != 0) {
        fputs("pmullw mismatch\n", stderr);
        return 1;
    }

    puts("pmullw ok");
    return 0;
}
