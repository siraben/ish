#include <stdio.h>

int main(void) {
    unsigned edx = 0xffffffff;

    __asm__ volatile(
            "xorl %%edx, %%edx\n\t"
            ".byte 0x8c, 0xda\n\t"
            "movl %%edx, %0"
            : "=r"(edx)
            :
            : "edx");

    (void) edx;
    puts("mov_ds ok");
    return 0;
}
