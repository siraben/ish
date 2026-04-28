#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "emu/interrupt.h"
#include "kernel/memory.h"
#include "kernel/signal.h"
#include "kernel/task.h"

dword_t syscall_stub(void) {
    return _ENOSYS;
}
// While identical, this version of the stub doesn't log below. Use this for
// syscalls that are optional (i.e. fallback on something else) but called
// frequently.
dword_t syscall_silent_stub(void) {
    return _ENOSYS;
}
dword_t syscall_success_stub(void) {
    return 0;
}

#if GUEST_RISCV64
static sqword_t rv_ret32(dword_t value) {
    return (sqword_t) (int32_t) value;
}

static sqword_t rv_stub(qword_t UNUSED(a0), qword_t UNUSED(a1), qword_t UNUSED(a2),
        qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) {
    return rv_ret32(syscall_stub());
}

static sqword_t rv_success_stub(qword_t UNUSED(a0), qword_t UNUSED(a1), qword_t UNUSED(a2),
        qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) {
    return rv_ret32(syscall_success_stub());
}

struct riscv_hwprobe_pair_ {
    sqword_t key;
    qword_t value;
};

static sqword_t rv_sys_riscv_hwprobe(qword_t pairs_addr, qword_t pair_count,
        qword_t UNUSED(cpu_count), qword_t UNUSED(cpus_addr), qword_t flags,
        qword_t UNUSED(a5)) {
    if (flags != 0)
        return _EINVAL;
    for (qword_t i = 0; i < pair_count; i++) {
        addr_t pair_addr = pairs_addr + i * sizeof(struct riscv_hwprobe_pair_);
        struct riscv_hwprobe_pair_ pair;
        if (user_get(pair_addr, pair))
            return _EFAULT;
        pair.key = -1;
        pair.value = 0;
        if (user_put(pair_addr, pair))
            return _EFAULT;
    }
    return 0;
}

struct rv_clone_args_ {
    qword_t flags;
    qword_t pidfd;
    qword_t child_tid;
    qword_t parent_tid;
    qword_t exit_signal;
    qword_t stack;
    qword_t stack_size;
    qword_t tls;
    qword_t set_tid;
    qword_t set_tid_size;
    qword_t cgroup;
};

#define RV_CLONE_DETACHED_ 0x00400000u
#define RV_CSIGNAL_ 0xffu

static sqword_t rv_sys_clone3(qword_t args_addr, qword_t size,
        qword_t UNUSED(a2), qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) {
    struct rv_clone_args_ args = {};
    if (size < 64)
        return _EINVAL;
    if (size > sizeof(args))
        size = sizeof(args);
    if (user_read(args_addr, &args, size))
        return _EFAULT;
    if ((args.flags >> 32) != 0 || args.exit_signal > RV_CSIGNAL_)
        return _EINVAL;
    if (args.flags & RV_CLONE_DETACHED_)
        return _EINVAL;
    if (args.pidfd || args.set_tid || args.set_tid_size || args.cgroup)
        return _ENOSYS;

    addr_t stack = args.stack;
    if (stack != 0)
        stack += args.stack_size;
    return rv_ret32(sys_clone((dword_t) (args.flags | args.exit_signal), stack,
            args.parent_tid, args.tls, args.child_tid));
}

static int_t timespec_to_ms(struct timespec_ timeout) {
    if (timeout.sec < 0 || timeout.nsec < 0 || timeout.nsec >= 1000000000)
        return _EINVAL;
    qword_t ms = (qword_t) timeout.sec * 1000 + ((qword_t) timeout.nsec + 999999) / 1000000;
    if (ms > INT32_MAX)
        return INT32_MAX;
    return (int_t) ms;
}

static sqword_t rv_sys_epoll_pwait2(qword_t epoll_f, qword_t events_addr,
        qword_t max_events, qword_t timeout_addr, qword_t sigmask_addr, qword_t sigsetsize) {
    int_t timeout = -1;
    if (timeout_addr != 0) {
        struct timespec_ timeout_ts;
        if (user_get(timeout_addr, timeout_ts))
            return _EFAULT;
        timeout = timespec_to_ms(timeout_ts);
        if (timeout < 0)
            return timeout;
    }
    return rv_ret32(sys_epoll_pwait((fd_t) epoll_f, (addr_t) events_addr,
            (int_t) max_events, timeout, (addr_t) sigmask_addr, (dword_t) sigsetsize));
}

#define RV_FUTEX2_SIZE_MASK_ 0x3u
#define RV_FUTEX2_SIZE_U32_ 0x2u
#define RV_FUTEX2_PRIVATE_ 0x80u

static bool rv_futex2_flags_supported(qword_t flags) {
    return (flags & ~(RV_FUTEX2_SIZE_MASK_ | RV_FUTEX2_PRIVATE_)) == 0 &&
        (flags & RV_FUTEX2_SIZE_MASK_) == RV_FUTEX2_SIZE_U32_;
}

static sqword_t rv_sys_futex_wake(qword_t uaddr, qword_t mask, qword_t nr,
        qword_t flags, qword_t UNUSED(a4), qword_t UNUSED(a5)) {
    if (!rv_futex2_flags_supported(flags))
        return _EINVAL;
    if (mask == 0)
        return _EINVAL;
    return rv_ret32(sys_futex((addr_t) uaddr, 1 | 128, (dword_t) nr, 0, 0, 0));
}

static sqword_t rv_sys_futex_wait(qword_t uaddr, qword_t val, qword_t mask,
        qword_t flags, qword_t timeout_addr, qword_t clockid) {
    if (!rv_futex2_flags_supported(flags))
        return _EINVAL;
    if (mask == 0)
        return _EINVAL;
    if (clockid != CLOCK_MONOTONIC_ && clockid != CLOCK_REALTIME_)
        return _EINVAL;
    return rv_ret32(sys_futex((addr_t) uaddr, 0 | 128, (dword_t) val,
            (addr_t) timeout_addr, 0, 0));
}

struct rv_futex_waitv_ {
    qword_t val;
    qword_t uaddr;
    dword_t flags;
    dword_t reserved;
};

static sqword_t rv_sys_futex_waitv(qword_t waiters_addr, qword_t nr_futexes,
        qword_t flags, qword_t timeout_addr, qword_t clockid, qword_t UNUSED(a5)) {
    if (flags != 0 || nr_futexes == 0)
        return _EINVAL;
    if (nr_futexes != 1)
        return _ENOSYS;
    struct rv_futex_waitv_ waiter;
    if (user_get(waiters_addr, waiter))
        return _EFAULT;
    if (waiter.reserved != 0 || !rv_futex2_flags_supported(waiter.flags))
        return _EINVAL;
    if (clockid != CLOCK_MONOTONIC_ && clockid != CLOCK_REALTIME_)
        return _EINVAL;
    dword_t err = sys_futex((addr_t) waiter.uaddr, 0 | 128, (dword_t) waiter.val,
            (addr_t) timeout_addr, 0, 0);
    return err == 0 ? 0 : rv_ret32(err);
}

static sqword_t rv_sys_futex_requeue(qword_t waiters_addr, qword_t flags,
        qword_t nr_wake, qword_t nr_requeue, qword_t UNUSED(a4), qword_t UNUSED(a5)) {
    if (flags != 0)
        return _EINVAL;
    struct rv_futex_waitv_ waiters[2];
    if (user_read(waiters_addr, waiters, sizeof(waiters)))
        return _EFAULT;
    for (int i = 0; i < 2; i++) {
        if (waiters[i].reserved != 0 || !rv_futex2_flags_supported(waiters[i].flags))
            return _EINVAL;
    }
    return rv_ret32(sys_futex((addr_t) waiters[0].uaddr, 3 | 128, (dword_t) nr_wake,
            (addr_t) nr_requeue, (addr_t) waiters[1].uaddr, 0));
}

#define RV_WRAP0(name) \
    static sqword_t rv_##name(qword_t UNUSED(a0), qword_t UNUSED(a1), qword_t UNUSED(a2), \
            qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) { \
        return rv_ret32(name()); \
    }
#define RV_WRAP1(name, t0) \
    static sqword_t rv_##name(qword_t a0, qword_t UNUSED(a1), qword_t UNUSED(a2), \
            qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) { \
        return rv_ret32(name((t0) a0)); \
    }
#define RV_WRAP2(name, t0, t1) \
    static sqword_t rv_##name(qword_t a0, qword_t a1, qword_t UNUSED(a2), \
            qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) { \
        return rv_ret32(name((t0) a0, (t1) a1)); \
    }
#define RV_WRAP3(name, t0, t1, t2) \
    static sqword_t rv_##name(qword_t a0, qword_t a1, qword_t a2, \
            qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) { \
        return rv_ret32(name((t0) a0, (t1) a1, (t2) a2)); \
    }
#define RV_WRAP4(name, t0, t1, t2, t3) \
    static sqword_t rv_##name(qword_t a0, qword_t a1, qword_t a2, qword_t a3, \
            qword_t UNUSED(a4), qword_t UNUSED(a5)) { \
        return rv_ret32(name((t0) a0, (t1) a1, (t2) a2, (t3) a3)); \
    }
#define RV_WRAP5(name, t0, t1, t2, t3, t4) \
    static sqword_t rv_##name(qword_t a0, qword_t a1, qword_t a2, qword_t a3, \
            qword_t a4, qword_t UNUSED(a5)) { \
        return rv_ret32(name((t0) a0, (t1) a1, (t2) a2, (t3) a3, (t4) a4)); \
    }
#define RV_WRAP6(name, t0, t1, t2, t3, t4, t5) \
    static sqword_t rv_##name(qword_t a0, qword_t a1, qword_t a2, qword_t a3, \
            qword_t a4, qword_t a5) { \
        return rv_ret32(name((t0) a0, (t1) a1, (t2) a2, (t3) a3, (t4) a4, (t5) a5)); \
    }
#define RV_WRAP_ADDR1(name, t0) \
    static sqword_t rv_##name(qword_t a0, qword_t UNUSED(a1), qword_t UNUSED(a2), \
            qword_t UNUSED(a3), qword_t UNUSED(a4), qword_t UNUSED(a5)) { \
        return (sqword_t) name((t0) a0); \
    }
#define RV_WRAP_ADDR6(name, t0, t1, t2, t3, t4, t5) \
    static sqword_t rv_##name(qword_t a0, qword_t a1, qword_t a2, qword_t a3, \
            qword_t a4, qword_t a5) { \
        return (sqword_t) name((t0) a0, (t1) a1, (t2) a2, (t3) a3, (t4) a4, (t5) a5); \
    }

RV_WRAP2(sys_getcwd, addr_t, dword_t)
RV_WRAP2(sys_eventfd2, uint_t, int_t)
RV_WRAP1(sys_epoll_create, int_t)
RV_WRAP4(sys_epoll_ctl, fd_t, int_t, fd_t, addr_t)
RV_WRAP6(sys_epoll_pwait, fd_t, addr_t, int_t, int_t, addr_t, dword_t)
RV_WRAP1(sys_dup, fd_t)
RV_WRAP3(sys_dup3, fd_t, fd_t, int_t)
RV_WRAP3(sys_fcntl, fd_t, dword_t, dword_t)
RV_WRAP3(sys_ioctl, fd_t, dword_t, dword_t)
RV_WRAP3(sys_ioprio_set, int_t, int_t, int_t)
RV_WRAP3(sys_ioprio_get, int_t, int_t, int_t)
RV_WRAP4(sys_mknodat, fd_t, addr_t, mode_t_, dev_t_)
RV_WRAP3(sys_mkdirat, fd_t, addr_t, mode_t_)
RV_WRAP3(sys_unlinkat, fd_t, addr_t, int_t)
RV_WRAP3(sys_symlinkat, addr_t, fd_t, addr_t)
RV_WRAP4(sys_linkat, fd_t, addr_t, fd_t, addr_t)
RV_WRAP4(sys_renameat, fd_t, addr_t, fd_t, addr_t)
RV_WRAP2(sys_umount2, addr_t, dword_t)
RV_WRAP5(sys_mount, addr_t, addr_t, addr_t, dword_t, addr_t)
RV_WRAP2(sys_statfs, addr_t, addr_t)
RV_WRAP3(sys_truncate64, addr_t, dword_t, dword_t)
RV_WRAP3(sys_ftruncate64, fd_t, dword_t, dword_t)
RV_WRAP6(sys_fallocate, fd_t, dword_t, dword_t, dword_t, dword_t, dword_t)
RV_WRAP4(sys_faccessat, fd_t, addr_t, mode_t_, dword_t)
RV_WRAP1(sys_chdir, addr_t)
RV_WRAP1(sys_fchdir, fd_t)
RV_WRAP1(sys_chroot, addr_t)
RV_WRAP2(sys_fchmod, fd_t, dword_t)
RV_WRAP5(sys_fchownat, fd_t, addr_t, dword_t, dword_t, int)
RV_WRAP3(sys_fchown32, fd_t, dword_t, dword_t)
RV_WRAP3(sys_fchmodat, fd_t, addr_t, dword_t)
RV_WRAP4(sys_openat, fd_t, addr_t, dword_t, mode_t_)
RV_WRAP4(sys_openat2, fd_t, addr_t, addr_t, dword_t)
RV_WRAP1(sys_close, fd_t)
RV_WRAP3(sys_close_range, dword_t, dword_t, dword_t)
RV_WRAP2(sys_flock, fd_t, dword_t)
RV_WRAP2(sys_pipe2, addr_t, int_t)
RV_WRAP3(sys_getdents64, fd_t, addr_t, dword_t)
RV_WRAP3(sys_lseek, fd_t, dword_t, dword_t)
RV_WRAP3(sys_read, fd_t, addr_t, dword_t)
RV_WRAP3(sys_write, fd_t, addr_t, dword_t)
RV_WRAP3(sys_readv, fd_t, addr_t, dword_t)
RV_WRAP3(sys_writev, fd_t, addr_t, dword_t)
RV_WRAP4(sys_pread, fd_t, addr_t, dword_t, off_t_)
RV_WRAP4(sys_pwrite, fd_t, addr_t, dword_t, off_t_)
RV_WRAP4(sys_preadv, fd_t, addr_t, dword_t, off_t_)
RV_WRAP4(sys_pwritev, fd_t, addr_t, dword_t, off_t_)
RV_WRAP4(sys_sendfile64, fd_t, fd_t, addr_t, dword_t)
RV_WRAP6(sys_pselect, fd_t, addr_t, addr_t, addr_t, addr_t, addr_t)
RV_WRAP5(sys_ppoll, addr_t, dword_t, addr_t, addr_t, dword_t)
RV_WRAP4(sys_signalfd4, fd_t, addr_t, dword_t, int_t)
RV_WRAP6(sys_splice, fd_t, addr_t, fd_t, addr_t, dword_t, dword_t)
RV_WRAP4(sys_readlinkat, fd_t, addr_t, addr_t, dword_t)
RV_WRAP4(sys_fstatat64, fd_t, addr_t, addr_t, dword_t)
RV_WRAP2(sys_fstat64, fd_t, addr_t)
RV_WRAP2(sys_fstatfs, fd_t, addr_t)
RV_WRAP1(sys_fsync, fd_t)
RV_WRAP4(sys_utimensat, fd_t, addr_t, addr_t, dword_t)
RV_WRAP2(sys_capget, addr_t, addr_t)
RV_WRAP2(sys_capset, addr_t, addr_t)
RV_WRAP1(sys_personality, dword_t)
RV_WRAP1(sys_exit, dword_t)
RV_WRAP1(sys_exit_group, dword_t)
RV_WRAP4(sys_waitid, int_t, pid_t_, addr_t, int_t)
RV_WRAP1(sys_set_tid_address, addr_t)
RV_WRAP6(sys_futex, addr_t, dword_t, dword_t, addr_t, addr_t, dword_t)
RV_WRAP2(sys_set_robust_list, addr_t, dword_t)
RV_WRAP3(sys_get_robust_list, pid_t_, addr_t, addr_t)
RV_WRAP2(sys_nanosleep, addr_t, addr_t)
RV_WRAP2(sys_getitimer, int_t, addr_t)
RV_WRAP3(sys_setitimer, int_t, addr_t, addr_t)
RV_WRAP3(sys_timer_create, dword_t, addr_t, addr_t)
RV_WRAP4(sys_timer_settime, dword_t, int_t, addr_t, addr_t)
RV_WRAP2(sys_timer_gettime, dword_t, addr_t)
RV_WRAP1(sys_timer_getoverrun, dword_t)
RV_WRAP1(sys_timer_delete, dword_t)
RV_WRAP2(sys_timerfd_create, int_t, int_t)
RV_WRAP4(sys_timerfd_settime, fd_t, int_t, addr_t, addr_t)
RV_WRAP2(sys_timerfd_gettime, fd_t, addr_t)
RV_WRAP2(sys_clock_settime, dword_t, addr_t)
RV_WRAP2(sys_clock_gettime, dword_t, addr_t)
RV_WRAP2(sys_clock_getres, dword_t, addr_t)
RV_WRAP4(sys_clock_nanosleep, dword_t, int_t, addr_t, addr_t)
RV_WRAP3(sys_syslog, int_t, addr_t, int_t)
RV_WRAP2(sys_sched_getparam, pid_t_, addr_t)
RV_WRAP3(sys_sched_setscheduler, pid_t_, int_t, addr_t)
RV_WRAP1(sys_sched_getscheduler, pid_t_)
RV_WRAP3(sys_sched_setaffinity, pid_t_, dword_t, addr_t)
RV_WRAP3(sys_sched_getaffinity, pid_t_, dword_t, addr_t)
RV_WRAP0(sys_sched_yield)
RV_WRAP1(sys_sched_get_priority_max, int_t)
RV_WRAP2(sys_kill, pid_t_, dword_t)
RV_WRAP2(sys_tkill, pid_t_, dword_t)
RV_WRAP3(sys_tgkill, pid_t_, pid_t_, dword_t)
RV_WRAP2(sys_sigaltstack, addr_t, addr_t)
RV_WRAP2(sys_rt_sigsuspend, addr_t, uint_t)
RV_WRAP4(sys_rt_sigaction, dword_t, addr_t, addr_t, dword_t)
RV_WRAP4(sys_rt_sigprocmask, dword_t, addr_t, addr_t, dword_t)
RV_WRAP1(sys_rt_sigpending, addr_t)
RV_WRAP4(sys_rt_sigtimedwait, addr_t, addr_t, addr_t, uint_t)
RV_WRAP0(sys_rt_sigreturn)
RV_WRAP3(sys_setpriority, int_t, pid_t_, int_t)
RV_WRAP2(sys_getpriority, int_t, pid_t_)
RV_WRAP3(sys_reboot, int_t, int_t, int_t)
RV_WRAP2(sys_setregid, uid_t_, uid_t_)
RV_WRAP1(sys_setgid, uid_t)
RV_WRAP1(sys_setfsgid, uid_t)
RV_WRAP2(sys_setreuid, uid_t_, uid_t_)
RV_WRAP1(sys_setuid, uid_t)
RV_WRAP1(sys_setfsuid, uid_t)
RV_WRAP3(sys_setresuid, uid_t_, uid_t_, uid_t_)
RV_WRAP3(sys_getresuid, addr_t, addr_t, addr_t)
RV_WRAP3(sys_setresgid, uid_t_, uid_t_, uid_t_)
RV_WRAP3(sys_getresgid, addr_t, addr_t, addr_t)
RV_WRAP1(sys_times, addr_t)
RV_WRAP2(sys_setpgid, pid_t_, pid_t_)
RV_WRAP1(sys_getpgid, pid_t_)
RV_WRAP0(sys_setsid)
RV_WRAP0(sys_getsid)
RV_WRAP2(sys_getgroups, dword_t, addr_t)
RV_WRAP2(sys_setgroups, dword_t, addr_t)
RV_WRAP1(sys_uname, addr_t)
RV_WRAP2(sys_sethostname, addr_t, dword_t)
RV_WRAP2(sys_getrusage, dword_t, addr_t)
RV_WRAP5(sys_xattr_stub, addr_t, addr_t, addr_t, dword_t, dword_t)
RV_WRAP2(sys_getrlimit64, dword_t, addr_t)
RV_WRAP2(sys_setrlimit64, dword_t, addr_t)
RV_WRAP0(sys_getpid)
RV_WRAP0(sys_getppid)
RV_WRAP0(sys_getuid)
RV_WRAP0(sys_geteuid)
RV_WRAP0(sys_getgid)
RV_WRAP0(sys_getegid)
RV_WRAP0(sys_gettid)
RV_WRAP1(sys_umask, dword_t)
RV_WRAP5(sys_prctl, dword_t, uint_t, uint_t, uint_t, uint_t)
RV_WRAP2(sys_gettimeofday, addr_t, addr_t)
RV_WRAP2(sys_settimeofday, addr_t, addr_t)
RV_WRAP1(sys_sysinfo, addr_t)
RV_WRAP3(sys_socket, dword_t, dword_t, dword_t)
RV_WRAP4(sys_socketpair, dword_t, dword_t, dword_t, addr_t)
RV_WRAP3(sys_bind, fd_t, addr_t, uint_t)
RV_WRAP2(sys_listen, fd_t, int_t)
RV_WRAP3(sys_accept, fd_t, addr_t, addr_t)
RV_WRAP4(sys_accept4, fd_t, addr_t, addr_t, int_t)
RV_WRAP3(sys_connect, fd_t, addr_t, uint_t)
RV_WRAP3(sys_getsockname, fd_t, addr_t, addr_t)
RV_WRAP3(sys_getpeername, fd_t, addr_t, addr_t)
RV_WRAP6(sys_sendto, fd_t, addr_t, dword_t, dword_t, addr_t, dword_t)
RV_WRAP6(sys_recvfrom, fd_t, addr_t, dword_t, dword_t, addr_t, addr_t)
RV_WRAP5(sys_setsockopt, fd_t, dword_t, dword_t, addr_t, dword_t)
RV_WRAP5(sys_getsockopt, fd_t, dword_t, dword_t, addr_t, dword_t)
RV_WRAP2(sys_shutdown, fd_t, dword_t)
RV_WRAP3(sys_sendmsg, fd_t, addr_t, int_t)
RV_WRAP3(sys_recvmsg, fd_t, addr_t, int_t)
RV_WRAP4(sys_sendmmsg, fd_t, addr_t, uint_t, int_t)
RV_WRAP5(sys_recvmmsg, fd_t, addr_t, uint_t, int_t, addr_t)
RV_WRAP_ADDR1(sys_brk, addr_t)
RV_WRAP2(sys_munmap, addr_t, uint_t)
RV_WRAP4(sys_mremap, addr_t, dword_t, dword_t, dword_t)
RV_WRAP5(sys_clone, dword_t, addr_t, addr_t, addr_t, addr_t)
RV_WRAP3(sys_execve, addr_t, addr_t, addr_t)
RV_WRAP_ADDR6(sys_mmap_riscv64, addr_t, qword_t, qword_t, qword_t, qword_t, qword_t)
RV_WRAP3(sys_mprotect, addr_t, uint_t, int_t)
RV_WRAP3(sys_msync, addr_t, dword_t, int_t)
RV_WRAP2(sys_mlock, addr_t, dword_t)
RV_WRAP3(sys_madvise, addr_t, dword_t, dword_t)
RV_WRAP6(sys_mbind, addr_t, dword_t, int_t, addr_t, dword_t, uint_t)
RV_WRAP4(sys_wait4, pid_t_, addr_t, dword_t, addr_t)
RV_WRAP4(sys_prlimit64, pid_t_, dword_t, addr_t, addr_t)
RV_WRAP5(sys_renameat2, fd_t, addr_t, fd_t, addr_t, int_t)
RV_WRAP3(sys_seccomp, dword_t, dword_t, addr_t)
RV_WRAP1(sys_unshare, dword_t)
RV_WRAP3(sys_getrandom, addr_t, dword_t, dword_t)
RV_WRAP3(sys_getcpu, addr_t, addr_t, addr_t)
RV_WRAP6(sys_copy_file_range, fd_t, addr_t, fd_t, addr_t, dword_t, uint_t)
RV_WRAP5(sys_statx, fd_t, addr_t, int_t, uint_t, addr_t)
RV_WRAP4(sys_fchmodat2, fd_t, addr_t, dword_t, dword_t)

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
#endif
syscall_t syscall_table[] = {
    // Default all current Linux asm-generic/riscv64 syscall slots to ENOSYS.
    // Real kernel-only facilities (io_uring, namespaces, modules, LSM,
    // fanotify, BPF, keyrings, etc.) cannot be faithfully implemented in this
    // userspace kernel; supported syscalls override this initializer below.
    [0 ... 471] = rv_stub,
    [5 ... 16] = rv_sys_xattr_stub, // xattrs: backing fs metadata model is not complete.
    [17]  = rv_sys_getcwd,
    [19]  = rv_sys_eventfd2,
    [20]  = rv_sys_epoll_create,
    [21]  = rv_sys_epoll_ctl,
    [22]  = rv_sys_epoll_pwait,
    [23]  = rv_sys_dup,
    [24]  = rv_sys_dup3,
    [25]  = rv_sys_fcntl,
    [26]  = rv_stub, // inotify requires kernel fs event delivery.
    [27]  = rv_stub,
    [28]  = rv_stub,
    [29]  = rv_sys_ioctl,
    [30]  = rv_sys_ioprio_set,
    [31]  = rv_sys_ioprio_get,
    [32]  = rv_sys_flock,
    [33]  = rv_sys_mknodat,
    [34]  = rv_sys_mkdirat,
    [35]  = rv_sys_unlinkat,
    [36]  = rv_sys_symlinkat,
    [37]  = rv_sys_linkat,
    [38]  = rv_sys_renameat,
    [39]  = rv_sys_umount2,
    [40]  = rv_sys_mount,
    [41]  = rv_stub, // pivot_root is a real-kernel mount namespace operation.
    [43]  = rv_sys_statfs,
    [44]  = rv_sys_fstatfs,
    [45]  = rv_sys_truncate64,
    [46]  = rv_sys_ftruncate64,
    [47]  = rv_sys_fallocate,
    [48]  = rv_sys_faccessat,
    [49]  = rv_sys_chdir,
    [50]  = rv_sys_fchdir,
    [51]  = rv_sys_chroot,
    [52]  = rv_sys_fchmod,
    [53]  = rv_sys_fchmodat,
    [54]  = rv_sys_fchownat,
    [55]  = rv_sys_fchown32,
    [56]  = rv_sys_openat,
    [57]  = rv_sys_close,
    [58]  = rv_stub, // vhangup has no terminal session authority here.
    [59]  = rv_sys_pipe2,
    [60]  = rv_stub, // quota management is host-kernel policy.
    [61]  = rv_sys_getdents64,
    [62]  = rv_sys_lseek,
    [63]  = rv_sys_read,
    [64]  = rv_sys_write,
    [65]  = rv_sys_readv,
    [66]  = rv_sys_writev,
    [67]  = rv_sys_pread,
    [68]  = rv_sys_pwrite,
    [69]  = rv_sys_preadv,
    [70]  = rv_sys_pwritev,
    [71]  = rv_sys_sendfile64,
    [72]  = rv_sys_pselect,
    [73]  = rv_sys_ppoll,
    [74]  = rv_sys_signalfd4,
    [75]  = rv_stub, // vmsplice needs host pipe page donation.
    [76]  = rv_sys_splice,
    [77]  = rv_stub, // tee needs pipe buffer sharing.
    [78]  = rv_sys_readlinkat,
    [79]  = rv_sys_fstatat64,
    [80]  = rv_sys_fstat64,
    [81]  = rv_success_stub,
    [82]  = rv_sys_fsync,
    [83]  = rv_sys_fsync,
    [84]  = rv_success_stub, // sync_file_range is only an advisory flush here.
    [85]  = rv_sys_timerfd_create,
    [86]  = rv_sys_timerfd_settime,
    [87]  = rv_sys_timerfd_gettime,
    [88]  = rv_sys_utimensat,
    [89]  = rv_stub, // process accounting is host-kernel global state.
    [90]  = rv_sys_capget,
    [91]  = rv_sys_capset,
    [92]  = rv_sys_personality,
    [93]  = rv_sys_exit,
    [94]  = rv_sys_exit_group,
    [95]  = rv_sys_waitid,
    [96]  = rv_sys_set_tid_address,
    [97]  = rv_sys_unshare,
    [98]  = rv_sys_futex,
    [99]  = rv_sys_set_robust_list,
    [100] = rv_sys_get_robust_list,
    [101] = rv_sys_nanosleep,
    [102] = rv_sys_getitimer,
    [103] = rv_sys_setitimer,
    [104] = rv_stub, // kexec is a real-kernel boot operation.
    [105] = rv_stub, // kernel module operations are impossible in userspace.
    [106] = rv_stub,
    [107] = rv_sys_timer_create,
    [108] = rv_sys_timer_gettime,
    [109] = rv_sys_timer_getoverrun,
    [110] = rv_sys_timer_settime,
    [111] = rv_sys_timer_delete,
    [112] = rv_sys_clock_settime,
    [113] = rv_sys_clock_gettime,
    [114] = rv_sys_clock_getres,
    [115] = rv_sys_clock_nanosleep,
    [116] = rv_sys_syslog,
    [117] = rv_stub, // ptrace register ABI needs RV64 regset support.
    [118] = rv_sys_sched_getparam,
    [119] = rv_sys_sched_setscheduler,
    [120] = rv_sys_sched_getscheduler,
    [121] = rv_sys_sched_getparam,
    [122] = rv_sys_sched_setaffinity,
    [123] = rv_sys_sched_getaffinity,
    [124] = rv_sys_sched_yield,
    [125] = rv_sys_sched_get_priority_max,
    [126] = rv_sys_sched_get_priority_max,
    [127] = rv_stub, // round-robin interval is not modeled.
    [128] = rv_stub, // restart is handled by syscall return paths, not direct.
    [129] = rv_sys_kill,
    [130] = rv_sys_tkill,
    [131] = rv_sys_tgkill,
    [132] = rv_sys_sigaltstack,
    [133] = rv_sys_rt_sigsuspend,
    [134] = rv_sys_rt_sigaction,
    [135] = rv_sys_rt_sigprocmask,
    [136] = rv_sys_rt_sigpending,
    [137] = rv_sys_rt_sigtimedwait,
    [138] = rv_stub, // queued realtime signal payload delivery incomplete.
    [139] = rv_sys_rt_sigreturn,
    [140] = rv_sys_setpriority,
    [141] = rv_sys_getpriority,
    [142] = rv_sys_reboot,
    [143] = rv_sys_setregid,
    [144] = rv_sys_setgid,
    [145] = rv_sys_setreuid,
    [146] = rv_sys_setuid,
    [147] = rv_sys_setresuid,
    [148] = rv_sys_getresuid,
    [149] = rv_sys_setresgid,
    [150] = rv_sys_getresgid,
    [151] = rv_sys_setfsuid,
    [152] = rv_sys_setfsgid,
    [153] = rv_sys_times,
    [154] = rv_sys_setpgid,
    [155] = rv_sys_getpgid,
    [156] = rv_sys_getsid,
    [157] = rv_sys_setsid,
    [158] = rv_sys_getgroups,
    [159] = rv_sys_setgroups,
    [160] = rv_sys_uname,
    [161] = rv_sys_sethostname,
    [162] = rv_stub, // domainname is not exposed by the emulator.
    [163] = rv_sys_getrlimit64,
    [164] = rv_sys_setrlimit64,
    [166] = rv_sys_umask,
    [165] = rv_sys_getrusage,
    [167] = rv_sys_prctl,
    [168] = rv_sys_getcpu,
    [169] = rv_sys_gettimeofday,
    [170] = rv_sys_settimeofday,
    [171] = rv_stub, // adjtimex is host-kernel clock discipline.
    [172] = rv_sys_getpid,
    [173] = rv_sys_getppid,
    [174] = rv_sys_getuid,
    [175] = rv_sys_geteuid,
    [176] = rv_sys_getgid,
    [177] = rv_sys_getegid,
    [178] = rv_sys_gettid,
    [179] = rv_sys_sysinfo,
    [180 ... 185] = rv_stub, // POSIX message queues not modeled.
    [186 ... 197] = rv_stub, // SysV IPC backend not modeled.
    [198] = rv_sys_socket,
    [199] = rv_sys_socketpair,
    [200] = rv_sys_bind,
    [201] = rv_sys_listen,
    [202] = rv_sys_accept,
    [203] = rv_sys_connect,
    [204] = rv_sys_getsockname,
    [205] = rv_sys_getpeername,
    [206] = rv_sys_sendto,
    [207] = rv_sys_recvfrom,
    [208] = rv_sys_setsockopt,
    [209] = rv_sys_getsockopt,
    [210] = rv_sys_shutdown,
    [211] = rv_sys_sendmsg,
    [212] = rv_sys_recvmsg,
    [213] = rv_success_stub, // readahead is advisory.
    [214] = rv_sys_brk,
    [215] = rv_sys_munmap,
    [216] = rv_sys_mremap,
    [217 ... 219] = rv_stub, // kernel keyrings are not modeled.
    [220] = rv_sys_clone,
    [221] = rv_sys_execve,
    [222] = rv_sys_mmap_riscv64,
    [223] = rv_success_stub, // fadvise is advisory.
    [224] = rv_stub, // swap is host-kernel VM policy.
    [225] = rv_stub,
    [226] = rv_sys_mprotect,
    [227] = rv_sys_msync,
    [228] = rv_sys_mlock,
    [229] = rv_success_stub, // munlock is advisory in this memory model.
    [230] = rv_success_stub,
    [231] = rv_success_stub,
    [232] = rv_stub, // mincore needs resident-page accounting.
    [233] = rv_sys_madvise,
    [234] = rv_stub, // remap_file_pages is obsolete and unsupported.
    [235] = rv_sys_mbind,
    [236 ... 239] = rv_stub, // NUMA policy/page migration not modeled.
    [240] = rv_stub, // queued realtime signal payload delivery incomplete.
    [241] = rv_stub, // perf_event_open needs host perf virtualization.
    [242] = rv_sys_accept4,
    [243] = rv_sys_recvmmsg,
    [258] = rv_sys_riscv_hwprobe,
    [259] = rv_success_stub, // riscv_flush_icache: interpreter has no i-cache.
    [260] = rv_sys_wait4,
    [261] = rv_sys_prlimit64,
    [262 ... 263] = rv_stub, // fanotify needs kernel fs event delivery.
    [264 ... 265] = rv_stub, // file handles are host-kernel persistent ids.
    [266] = rv_stub, // clock_adjtime is host-kernel clock discipline.
    [267] = rv_success_stub, // syncfs is advisory over host fs.
    [268] = rv_stub, // namespaces are not modeled.
    [269] = rv_sys_sendmmsg,
    [270 ... 271] = rv_stub, // process_vm_* needs cross-task ptrace memory API.
    [272] = rv_stub, // kcmp inspects kernel object identity.
    [273] = rv_stub, // module loading impossible in userspace.
    [274] = rv_stub, // sched_attr not modeled.
    [275] = rv_stub,
    [276] = rv_sys_renameat2,
    [277] = rv_sys_seccomp,
    [278] = rv_sys_getrandom,
    [279] = rv_stub, // memfd requires anonymous file object support.
    [280] = rv_stub, // BPF VM/program registry not modeled.
    [281] = rv_stub, // execveat can be added on top of path-at resolution.
    [282] = rv_stub, // userfaultfd requires fault delegation.
    [283] = rv_success_stub,
    [284] = rv_success_stub, // mlock2 is advisory here.
    [285] = rv_sys_copy_file_range,
    [286 ... 287] = rv_stub, // preadv2/pwritev2 not represented by fd layer.
    [288] = rv_sys_mprotect, // pkeys are ignored; keep protection semantics.
    [289 ... 290] = rv_stub, // memory protection keys not modeled.
    [291] = rv_sys_statx,
    [292] = rv_stub, // AIO not modeled.
    [293] = rv_stub, // rseq registration not honored by scheduler yet.
    [294] = rv_stub, // kexec is a real-kernel boot operation.
    [424 ... 434] = rv_stub, // pidfd, io_uring, and mount API require kernel objects.
    [435] = rv_sys_clone3,
    [436] = rv_sys_close_range,
    [437] = rv_sys_openat2,
    [438] = rv_stub, // pidfd_getfd requires pidfd objects.
    [439] = rv_sys_faccessat,
    [440] = rv_stub, // process_madvise needs cross-task memory advice.
    [441] = rv_sys_epoll_pwait2,
    [442 ... 443] = rv_stub, // mount attributes and quota state are host-kernel policy.
    [444 ... 446] = rv_stub, // Landlock needs an LSM ruleset object model.
    [447 ... 448] = rv_stub, // memfd_secret and process_mrelease need kernel VM/task objects.
    [449] = rv_sys_futex_waitv,
    [450 ... 451] = rv_stub, // NUMA policy and cachestat are not modeled.
    [452] = rv_sys_fchmodat2,
    [453] = rv_stub, // shadow stacks are architecture/kernel-managed.
    [454] = rv_sys_futex_wake,
    [455] = rv_sys_futex_wait,
    [456] = rv_sys_futex_requeue,
    [457 ... 461] = rv_stub, // new mount and LSM inspection APIs need kernel-global state.
    [462] = rv_success_stub, // mseal is advisory without VMA seal enforcement here.
    [463 ... 466] = rv_sys_xattr_stub,
    [467 ... 469] = rv_stub, // newer mount/file attribute APIs are not represented yet.
    [470 ... 471] = rv_stub,
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#define NUM_SYSCALLS (sizeof(syscall_table) / sizeof(syscall_table[0]))

void dump_stack(int lines);

void handle_interrupt(int interrupt) {
    struct cpu_state *cpu = &current->cpu;
    if (interrupt == INT_SYSCALL) {
#if GUEST_RISCV64
        unsigned syscall_num = cpu->a7;
#else
        unsigned syscall_num = cpu->eax;
#endif
        if (syscall_num >= NUM_SYSCALLS || syscall_table[syscall_num] == NULL) {
            printk("%d(%s) missing syscall %d\n", current->pid, current->comm, syscall_num);
#if GUEST_RISCV64
            cpu->a0 = (uint64_t) (int64_t) _ENOSYS;
#else
            cpu->eax = _ENOSYS;
#endif
        } else {
#if GUEST_RISCV64
            if (syscall_table[syscall_num] == rv_stub) {
#else
            if (syscall_table[syscall_num] == (syscall_t) syscall_stub) {
#endif
                printk("%d(%s) stub syscall %d\n", current->pid, current->comm, syscall_num);
            }
            STRACE("%d call %-3d ", current->pid, syscall_num);
#if GUEST_RISCV64
            sqword_t result = syscall_table[syscall_num](cpu->a0, cpu->a1, cpu->a2, cpu->a3, cpu->a4, cpu->a5);
            STRACE(" = 0x%llx\n", (unsigned long long) result);
            cpu->a0 = (uint64_t) result;
#else
            int result = syscall_table[syscall_num](cpu->ebx, cpu->ecx, cpu->edx, cpu->esi, cpu->edi, cpu->ebp);
            STRACE(" = 0x%x\n", result);
            cpu->eax = result;
#endif
        }
    } else if (interrupt == INT_GPF) {
        // some page faults, such as stack growing or CoW clones, are handled by mem_ptr
        read_wrlock(&current->mem->lock);
        void *ptr = mem_ptr(current->mem, cpu->segfault_addr, cpu->segfault_was_write ? MEM_WRITE : MEM_READ);
        read_wrunlock(&current->mem->lock);
        if (ptr == NULL) {
#if GUEST_RISCV64
            printk("%d page fault on 0x%llx at 0x%llx\n", current->pid,
                    (unsigned long long) cpu->segfault_addr, (unsigned long long) cpu->eip);
#else
            printk("%d page fault on 0x%x at 0x%x\n", current->pid, cpu->segfault_addr, cpu->eip);
#endif
            struct siginfo_ info = {
                .code = mem_segv_reason(current->mem, cpu->segfault_addr),
                .fault.addr = cpu->segfault_addr,
            };
            dump_stack(8);
            deliver_signal(current, SIGSEGV_, info);
        }
    } else if (interrupt == INT_UNDEFINED) {
#if GUEST_RISCV64
        printk("%d illegal instruction at 0x%llx: ", current->pid, (unsigned long long) cpu->eip);
#else
        printk("%d illegal instruction at 0x%x: ", current->pid, cpu->eip);
#endif
        for (int i = 0; i < 8; i++) {
            uint8_t b;
            if (user_get(cpu->eip + i, b))
                break;
            printk("%02x ", b);
        }
        printk("\n");
        dump_stack(8);
        struct siginfo_ info = {
            .code = SI_KERNEL_,
            .fault.addr = cpu->eip,
        };
        deliver_signal(current, SIGILL_, info);
    } else if (interrupt == INT_BREAKPOINT) {
        lock(&pids_lock);
        send_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = SI_KERNEL_,
        });
        unlock(&pids_lock);
    } else if (interrupt == INT_DEBUG) {
        lock(&pids_lock);
        send_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = TRAP_TRACE_,
        });
        unlock(&pids_lock);
    } else if (interrupt != INT_TIMER) {
        printk("%d unhandled interrupt %d\n", current->pid, interrupt);
        sys_exit(interrupt);
    }

    receive_signals();
    struct tgroup *group = current->group;
    lock(&group->lock);
    while (group->stopped)
        wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
    unlock(&group->lock);
}

void dump_maps(void) {
    extern void proc_maps_dump(struct task *task, struct proc_data *buf);
    struct proc_data buf = {};
    proc_maps_dump(current, &buf);
    // go a line at a time because it can be fucking enormous
    char *orig_data = buf.data;
    while (buf.size > 0) {
        size_t chunk_size = buf.size;
        if (chunk_size > 1024)
            chunk_size = 1024;
        printk("%.*s", chunk_size, buf.data);
        buf.data += chunk_size;
        buf.size -= chunk_size;
    }
    free(orig_data);
}

void dump_mem(addr_t start, uint_t len) {
    const int width = 8;
    for (addr_t addr = start; addr < start + len; addr += sizeof(dword_t)) {
        unsigned from_left = (addr - start) / sizeof(dword_t) % width;
        if (from_left == 0)
            printk("%08x: ", addr);
        dword_t word;
        if (user_get(addr, word))
            break;
        printk("%08x ", word);
        if (from_left == width - 1)
            printk("\n");
    }
}

void dump_stack(int lines) {
#if GUEST_RISCV64
    printk("stack at 0x%llx, ip at 0x%llx\n",
            (unsigned long long) current->cpu.sp,
            (unsigned long long) current->cpu.pc);
    dump_mem(current->cpu.sp, lines * sizeof(dword_t) * 8);
#else
    printk("stack at %x, base at %x, ip at %x\n", current->cpu.esp, current->cpu.ebp, current->cpu.eip);
    dump_mem(current->cpu.esp, lines * sizeof(dword_t) * 8);
#endif
}

// TODO find a home for this
#ifdef LOG_OVERRIDE
int log_override = 0;
#endif
