#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint32_t eax = 5;
    uint32_t edx = 5;
    uint32_t ecx = 9;

    __asm__ volatile("cmpxchgl %%ecx, %%edx"
            : "+a"(eax), "+d"(edx)
            : "c"(ecx)
            : "cc");
    if (eax != 5 || edx != 9) {
        printf("cmpxchg equal failed: eax=%u edx=%u\n", eax, edx);
        return 1;
    }

    eax = 7;
    edx = 5;
    ecx = 11;
    __asm__ volatile("cmpxchgl %%ecx, %%edx"
            : "+a"(eax), "+d"(edx)
            : "c"(ecx)
            : "cc");
    if (eax != 5 || edx != 5) {
        printf("cmpxchg unequal failed: eax=%u edx=%u\n", eax, edx);
        return 1;
    }

    puts("cmpxchg_reg ok");
    return 0;
}
