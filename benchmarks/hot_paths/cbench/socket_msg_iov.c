#include "sys.h"

#define AF_UNIX 1
#define SOCK_STREAM 1
#define CHUNK_SIZE 1024
#define IOV_COUNT 8
#define ROUNDS 8192

static char send_buf[IOV_COUNT][CHUNK_SIZE];
static char recv_buf[IOV_COUNT][CHUNK_SIZE];
static struct bench_iovec tmp_iov[IOV_COUNT];
static struct bench_msghdr send_msg;
static struct bench_msghdr recv_msg;

static int slice_iov(struct bench_iovec *dst, struct bench_iovec *src, long offset) {
    int out = 0;
    for (int i = 0; i < IOV_COUNT; i++) {
        if (offset >= CHUNK_SIZE) {
            offset -= CHUNK_SIZE;
            continue;
        }
        dst[out].base = src[i].base + (bench_addr_t) offset;
        dst[out].len = CHUNK_SIZE - offset;
        out++;
        offset = 0;
    }
    return out;
}

int bench_main(void) {
    int fds[2];
    if (sys_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        return 1;

    struct bench_iovec send_iov[IOV_COUNT];
    struct bench_iovec recv_iov[IOV_COUNT];
    for (int i = 0; i < IOV_COUNT; i++) {
        for (int j = 0; j < CHUNK_SIZE; j++)
            send_buf[i][j] = (char) (i * 19 + j * 11);
        send_iov[i].base = (bench_addr_t) send_buf[i];
        send_iov[i].len = CHUNK_SIZE;
        recv_iov[i].base = (bench_addr_t) recv_buf[i];
        recv_iov[i].len = CHUNK_SIZE;
    }

    unsigned checksum = 0;
    for (int i = 0; i < ROUNDS; i++) {
        long sent = 0;
        while (sent < CHUNK_SIZE * IOV_COUNT) {
            send_msg.msg_iov = (bench_addr_t) tmp_iov;
            send_msg.msg_iovlen = slice_iov(tmp_iov, send_iov, sent);
            long res = sys_sendmsg(fds[0], &send_msg, 0);
            if (res <= 0)
                return 2;
            sent += res;
        }
        long received = 0;
        while (received < CHUNK_SIZE * IOV_COUNT) {
            recv_msg.msg_iov = (bench_addr_t) tmp_iov;
            recv_msg.msg_iovlen = slice_iov(tmp_iov, recv_iov, received);
            long res = sys_recvmsg(fds[1], &recv_msg, 0);
            if (res <= 0)
                return 3;
            received += res;
        }
        checksum += (unsigned char) recv_buf[i & (IOV_COUNT - 1)][i & (CHUNK_SIZE - 1)];
    }

    sys_close(fds[0]);
    sys_close(fds[1]);
    write_u32(ROUNDS * IOV_COUNT * CHUNK_SIZE * 2u + checksum);
    return 0;
}
