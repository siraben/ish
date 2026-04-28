#include "sys.h"

int bench_main(void) {
    unsigned checksum = 0;
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 320; x++) {
            int cr = (x * 3 * 1024) / 320 - 2 * 1024;
            int ci = (y * 2 * 1024) / 240 - 1024;
            int zr = 0;
            int zi = 0;
            int iter = 0;
            while (iter < 96) {
                int zr2 = (zr * zr) / 1024;
                int zi2 = (zi * zi) / 1024;
                if (zr2 + zi2 > 4 * 1024)
                    break;
                int nzi = (2 * zr * zi) / 1024 + ci;
                zr = zr2 - zi2 + cr;
                zi = nzi;
                iter++;
            }
            checksum = checksum * 33u + (unsigned) iter;
        }
    }
    write_u32(checksum);
    return 0;
}
