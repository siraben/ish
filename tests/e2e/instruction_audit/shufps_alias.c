#include <stdio.h>
#include <string.h>

int main(void) {
    float dst[4] __attribute__((aligned(16))) = {10.0f, 20.0f, 30.0f, 40.0f};
    float src[4] __attribute__((aligned(16))) = {100.0f, 200.0f, 300.0f, 400.0f};
    float out[4] __attribute__((aligned(16))) = {0.0f};
    float expected[4] = {20.0f, 10.0f, 300.0f, 400.0f};

    __asm__ volatile(
            "movaps %1, %%xmm0\n\t"
            "shufps $0xe1, %2, %%xmm0\n\t"
            "movaps %%xmm0, %0"
            : "=m"(out)
            : "m"(dst), "m"(src)
            : "xmm0");

    if (memcmp(out, expected, sizeof(out)) != 0) {
        printf("shufps mismatch: %.1f %.1f %.1f %.1f\n",
                out[0], out[1], out[2], out[3]);
        return 1;
    }

    puts("shufps_alias ok");
    return 0;
}
