#include <stdint.h>

#include "kernel/vdso.h"

const char vdso_data[VDSO_PAGES * (1 << 12)] __attribute__((aligned(4096)));
