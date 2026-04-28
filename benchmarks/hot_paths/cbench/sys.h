#ifndef ISH_BENCH_SYS_H
#define ISH_BENCH_SYS_H

typedef unsigned long usize;
typedef long isize;

struct bench_timespec {
    isize sec;
    isize nsec;
};

#if defined(__riscv)
static long syscall0(long n) {
    register long a7 asm("a7") = n;
    register long a0 asm("a0");
    asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static long syscall1(long n, long x0) {
    register long a0 asm("a0") = x0;
    register long a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}
static long syscall2(long n, long x0, long x1) {
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}
static long syscall3(long n, long x0, long x1, long x2) {
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a2 asm("a2") = x2;
    register long a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static long syscall4(long n, long x0, long x1, long x2, long x3) {
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a2 asm("a2") = x2;
    register long a3 asm("a3") = x3;
    register long a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_CLOSE 57
#define SYS_EXIT 93
#define SYS_OPENAT 56
#define SYS_PIPE2 59
#define SYS_CLOCK_GETTIME 113
#define AT_FDCWD (-100)
#elif defined(__i386__)
static long syscall0(long n) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}
static long syscall1(long n, long x0) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(x0) : "memory");
    return r;
}
static long syscall2(long n, long x0, long x1) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(x0), "c"(x1) : "memory");
    return r;
}
static long syscall3(long n, long x0, long x1, long x2) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(x0), "c"(x1), "d"(x2) : "memory");
    return r;
}
static long syscall4(long n, long x0, long x1, long x2, long x3) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(x0), "c"(x1), "d"(x2), "S"(x3) : "memory");
    return r;
}
#define SYS_READ 3
#define SYS_WRITE 4
#define SYS_CLOSE 6
#define SYS_EXIT 1
#define SYS_OPEN 5
#define SYS_PIPE 42
#define SYS_CLOCK_GETTIME 265
#endif

#define CLOCK_MONOTONIC 1

#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000

static long sys_read(int fd, void *buf, usize n) {
    return syscall3(SYS_READ, fd, (long) buf, n);
}

static long sys_write(int fd, const void *buf, usize n) {
    return syscall3(SYS_WRITE, fd, (long) buf, n);
}

static long sys_close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

static long sys_open_rw_create(const char *path) {
#if defined(__riscv)
    return syscall4(SYS_OPENAT, AT_FDCWD, (long) path, O_RDWR | O_CREAT | O_TRUNC, 0644);
#else
    return syscall3(SYS_OPEN, (long) path, O_RDWR | O_CREAT | O_TRUNC, 0644);
#endif
}

static long sys_open_ro(const char *path) {
#if defined(__riscv)
    return syscall4(SYS_OPENAT, AT_FDCWD, (long) path, O_RDONLY, 0);
#else
    return syscall3(SYS_OPEN, (long) path, O_RDONLY, 0);
#endif
}

static long sys_pipe_pair(int fds[2]) {
#if defined(__riscv)
    return syscall2(SYS_PIPE2, (long) fds, 0);
#else
    return syscall1(SYS_PIPE, (long) fds);
#endif
}

static long sys_clock_gettime(int clock, struct bench_timespec *tp) {
    return syscall2(SYS_CLOCK_GETTIME, clock, (long) tp);
}

__attribute__((noreturn)) static void sys_exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;) {}
}

static void write_u32(unsigned value) {
    char buf[16];
    int i = 0;
    if (value == 0) {
        buf[i++] = '0';
    } else {
        char tmp[16];
        int n = 0;
        while (value != 0) {
            tmp[n++] = (char) ('0' + value % 10);
            value /= 10;
        }
        while (n != 0)
            buf[i++] = tmp[--n];
    }
    buf[i++] = '\n';
    sys_write(1, buf, (usize) i);
}

static void write_uint_padded_9(unsigned value) {
    char buf[9];
    for (int i = 8; i >= 0; i--) {
        buf[i] = (char) ('0' + value % 10);
        value /= 10;
    }
    sys_write(1, buf, sizeof(buf));
}

static void write_ulong(unsigned long value) {
    char buf[32];
    int i = 0;
    if (value == 0) {
        buf[i++] = '0';
    } else {
        char tmp[32];
        int n = 0;
        while (value != 0) {
            tmp[n++] = (char) ('0' + value % 10);
            value /= 10;
        }
        while (n != 0)
            buf[i++] = tmp[--n];
    }
    sys_write(1, buf, (usize) i);
}

static void write_elapsed(struct bench_timespec start, struct bench_timespec end) {
    isize sec = end.sec - start.sec;
    isize nsec = end.nsec - start.nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000;
    }
    sys_write(1, "elapsed_s\t", 10);
    write_ulong((unsigned long) sec);
    sys_write(1, ".", 1);
    write_uint_padded_9((unsigned) nsec);
    sys_write(1, "\n", 1);
}

#endif
