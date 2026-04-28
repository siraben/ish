#include "sys.h"

static char buf[4096];

int bench_main(void) {
    int fds[2];
    if (sys_pipe_pair(fds) < 0)
        return 1;
    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = (char) (i ^ (i >> 3));
    for (int i = 0; i < 4096; i++) {
        if (sys_write(fds[1], buf, sizeof(buf)) != (long) sizeof(buf))
            return 2;
        if (sys_read(fds[0], buf, sizeof(buf)) != (long) sizeof(buf))
            return 3;
    }
    sys_close(fds[0]);
    sys_close(fds[1]);
    write_u32(4096 * 4096u);
    return 0;
}
