#ifndef ISH_BENCH_SYS_H
#define ISH_BENCH_SYS_H

typedef unsigned long usize;
typedef long isize;
typedef unsigned long bench_addr_t;

struct bench_timespec {
    isize sec;
    isize nsec;
};

struct bench_iovec {
    bench_addr_t base;
    usize len;
};

struct bench_msghdr {
    bench_addr_t msg_name;
    unsigned msg_namelen;
#if defined(__riscv)
    unsigned __pad_msg_namelen;
#endif
    bench_addr_t msg_iov;
    usize msg_iovlen;
    bench_addr_t msg_control;
    usize msg_controllen;
    int msg_flags;
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
static long syscall5(long n, long x0, long x1, long x2, long x3, long x4) {
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a2 asm("a2") = x2;
    register long a3 asm("a3") = x3;
    register long a4 asm("a4") = x4;
    register long a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7) : "memory");
    return a0;
}
static long syscall6(long n, long x0, long x1, long x2, long x3, long x4, long x5) {
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a2 asm("a2") = x2;
    register long a3 asm("a3") = x3;
    register long a4 asm("a4") = x4;
    register long a5 asm("a5") = x5;
    register long a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
}
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_READV 65
#define SYS_WRITEV 66
#define SYS_CLOSE 57
#define SYS_FSTAT 80
#define SYS_GETPID 172
#define SYS_GETPPID 173
#define SYS_GETUID 174
#define SYS_GETTID 178
#define SYS_GETRESUID 148
#define SYS_GETTIMEOFDAY 169
#define SYS_RT_SIGPROCMASK 135
#define SYS_SCHED_YIELD 124
#define SYS_FUTEX 98
#define SYS_EVENTFD2 19
#define SYS_EPOLL_CREATE1 20
#define SYS_EPOLL_CTL 21
#define SYS_EPOLL_PWAIT 22
#define SYS_DUP 23
#define SYS_FCNTL 25
#define SYS_MMAP 222
#define SYS_MUNMAP 215
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_SOCKETPAIR 199
#define SYS_SENDMSG 211
#define SYS_RECVMSG 212
#define SYS_EXIT 93
#define SYS_OPENAT 56
#define SYS_PIPE2 59
#define SYS_LSEEK 62
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
static long syscall5(long n, long x0, long x1, long x2, long x3, long x4) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(x0), "c"(x1), "d"(x2), "S"(x3), "D"(x4) : "memory");
    return r;
}
static long syscall6(long n, long x0, long x1, long x2, long x3, long x4, long x5) {
    long r;
    asm volatile(
        "push %%ebp\n\t"
        "movl %7, %%ebp\n\t"
        "int $0x80\n\t"
        "pop %%ebp"
        : "=a"(r)
        : "a"(n), "b"(x0), "c"(x1), "d"(x2), "S"(x3), "D"(x4), "r"(x5)
        : "memory");
    return r;
}
#define SYS_READ 3
#define SYS_WRITE 4
#define SYS_READV 145
#define SYS_WRITEV 146
#define SYS_CLOSE 6
#define SYS_FSTAT 197
#define SYS_GETPID 20
#define SYS_GETPPID 64
#define SYS_GETUID 199
#define SYS_GETTID 224
#define SYS_GETRESUID 209
#define SYS_GETTIMEOFDAY 78
#define SYS_RT_SIGPROCMASK 175
#define SYS_SCHED_YIELD 158
#define SYS_FUTEX 240
#define SYS_EVENTFD2 328
#define SYS_EPOLL_CREATE1 329
#define SYS_EPOLL_CTL 255
#define SYS_EPOLL_PWAIT 319
#define SYS_DUP 41
#define SYS_FCNTL 55
#define SYS_MMAP 90
#define SYS_MMAP2 192
#define SYS_MUNMAP 91
#define SYS_FORK 2
#define SYS_WAIT4 114
#define SYS_EXIT 1
#define SYS_SOCKETCALL 102
#define SYS_OPEN 5
#define SYS_PIPE 42
#define SYS_LSEEK 19
#define SYS_CLOCK_GETTIME 265
#endif

#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0

#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

#define EPOLL_CTL_ADD 1
#define EPOLLIN 0x001
#define EPOLLOUT 0x004

#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1

#define SIG_BLOCK 0
#define SIG_SETMASK 2
#define SIGCHLD 17

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

static long sys_read(int fd, void *buf, usize n) {
    return syscall3(SYS_READ, fd, (long) buf, n);
}

static long sys_write(int fd, const void *buf, usize n) {
    return syscall3(SYS_WRITE, fd, (long) buf, n);
}

static long sys_readv(int fd, const struct bench_iovec *iov, usize iovcnt) {
    return syscall3(SYS_READV, fd, (long) iov, iovcnt);
}

static long sys_writev(int fd, const struct bench_iovec *iov, usize iovcnt) {
    return syscall3(SYS_WRITEV, fd, (long) iov, iovcnt);
}

static long sys_close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

static long sys_fstat(int fd, void *statbuf) {
    return syscall2(SYS_FSTAT, fd, (long) statbuf);
}

static long sys_getpid(void) {
    return syscall0(SYS_GETPID);
}

static long sys_getppid(void) {
    return syscall0(SYS_GETPPID);
}

static long sys_getuid(void) {
    return syscall0(SYS_GETUID);
}

static long sys_gettid(void) {
    return syscall0(SYS_GETTID);
}

static long sys_getresuid(unsigned *ruid, unsigned *euid, unsigned *suid) {
    return syscall3(SYS_GETRESUID, (long) ruid, (long) euid, (long) suid);
}

static long sys_dup(int fd) {
    return syscall1(SYS_DUP, fd);
}

static long sys_fcntl(int fd, int cmd, long arg) {
    return syscall3(SYS_FCNTL, fd, cmd, arg);
}

static long sys_rt_sigprocmask(int how, const unsigned long long *set, unsigned long long *oldset, usize size) {
    return syscall4(SYS_RT_SIGPROCMASK, how, (long) set, (long) oldset, size);
}

static long sys_sched_yield(void) {
    return syscall0(SYS_SCHED_YIELD);
}

static long sys_fork_like(void) {
#if defined(__riscv)
    return syscall5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
#else
    return syscall0(SYS_FORK);
#endif
}

static long sys_wait4(long pid, int *status, int options, void *rusage) {
    return syscall4(SYS_WAIT4, pid, (long) status, options, (long) rusage);
}

static long sys_socketpair(int domain, int type, int protocol, int fds[2]) {
#if defined(__riscv)
    return syscall4(SYS_SOCKETPAIR, domain, type, protocol, (long) fds);
#else
    long args[4] = {domain, type, protocol, (long) fds};
    return syscall2(SYS_SOCKETCALL, 8, (long) args);
#endif
}

static long sys_sendmsg(int fd, struct bench_msghdr *msg, int flags) {
#if defined(__riscv)
    return syscall3(SYS_SENDMSG, fd, (long) msg, flags);
#else
    long args[3] = {fd, (long) msg, flags};
    return syscall2(SYS_SOCKETCALL, 16, (long) args);
#endif
}

static long sys_recvmsg(int fd, struct bench_msghdr *msg, int flags) {
#if defined(__riscv)
    return syscall3(SYS_RECVMSG, fd, (long) msg, flags);
#else
    long args[3] = {fd, (long) msg, flags};
    return syscall2(SYS_SOCKETCALL, 17, (long) args);
#endif
}


static long sys_lseek(int fd, long offset, int whence) {
    return syscall3(SYS_LSEEK, fd, offset, whence);
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

struct bench_timeval {
    isize sec;
    isize usec;
};

static long sys_gettimeofday(struct bench_timeval *tv) {
    return syscall2(SYS_GETTIMEOFDAY, (long) tv, 0);
}

static long sys_futex(int *uaddr, int op, int val, const struct bench_timespec *timeout, int *uaddr2, int val3) {
    return syscall6(SYS_FUTEX, (long) uaddr, op, val, (long) timeout, (long) uaddr2, val3);
}

struct bench_epoll_event {
    unsigned events;
    unsigned long long data;
} __attribute__((packed));

static long sys_eventfd2(unsigned initval, int flags) {
    return syscall2(SYS_EVENTFD2, initval, flags);
}

static long sys_epoll_create1(int flags) {
    return syscall1(SYS_EPOLL_CREATE1, flags);
}

static long sys_epoll_ctl(int epfd, int op, int fd, struct bench_epoll_event *event) {
    return syscall4(SYS_EPOLL_CTL, epfd, op, fd, (long) event);
}

static long sys_epoll_wait0(int epfd, struct bench_epoll_event *events, int maxevents, int timeout) {
#if defined(__riscv)
    return syscall6(SYS_EPOLL_PWAIT, epfd, (long) events, maxevents, timeout, 0, 0);
#else
    return syscall6(SYS_EPOLL_PWAIT, epfd, (long) events, maxevents, timeout, 0, 8);
#endif
}

static long sys_mmap_anon(usize size) {
#if defined(__riscv)
    return syscall6(SYS_MMAP, 0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    long args[6] = {0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0};
    return syscall1(SYS_MMAP, (long) args);
#endif
}

static long sys_munmap(void *addr, usize size) {
    return syscall2(SYS_MUNMAP, (long) addr, size);
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
