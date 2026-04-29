#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>

#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "fs/poll.h"

static struct fd_ops signalfd_ops;
static_assert(offsetof(struct sigaction_, mask) == 16, "riscv64 sigaction mask offset");

struct rv_siginfo_ {
    int_t sig;
    int_t sig_errno;
    int_t code;
    int_t __pad0;
    union {
        struct {
            pid_t_ pid;
            uid_t_ uid;
            union sigval_ value;
        } queue;
        struct {
            pid_t_ pid;
            uid_t_ uid;
        } kill;
        struct {
            pid_t_ pid;
            uid_t_ uid;
            int_t status;
            int_t __pad0;
            qword_t utime;
            qword_t stime;
        } child;
        struct {
            addr_t addr;
        } fault;
        struct {
            addr_t addr;
            int_t syscall;
        } sigsys;
        struct {
            int_t timer;
            int_t overrun;
            union sigval_ value;
            int_t _private;
        } timer;
    };
    uint8_t __pad[128 - 16 - 32];
};
static_assert(sizeof(struct rv_siginfo_) == 128, "riscv64 siginfo size");
static_assert(offsetof(struct rv_siginfo_, fault.addr) == 16, "riscv64 siginfo si_addr offset");

struct signalfd_siginfo_ {
    dword_t signo;
    int_t sig_errno;
    int_t code;
    dword_t pid;
    dword_t uid;
    int_t fd;
    dword_t tid;
    dword_t band;
    dword_t overrun;
    dword_t trapno;
    int_t status;
    int_t ssi_int;
    qword_t ptr;
    qword_t utime;
    qword_t stime;
    qword_t addr;
    word_t addr_lsb;
    word_t __pad2;
    int_t syscall;
    qword_t call_addr;
    dword_t arch;
    uint8_t __pad[28];
};
static_assert(sizeof(struct signalfd_siginfo_) == 128, "signalfd_siginfo size");

#define RISCV_SIGNAL_FRAME_MAGIC_END 0
#define RISCV_SIGNAL_FRAME_END_SIZE 0

struct rv_user_regs {
    qword_t pc;
    qword_t ra, sp, gp, tp;
    qword_t t0, t1, t2;
    qword_t s0, s1;
    qword_t a0, a1, a2, a3, a4, a5, a6, a7;
    qword_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    qword_t t3, t4, t5, t6;
};

struct rv_d_fp_state {
    qword_t f[32];
    dword_t fcsr;
};

struct rv_q_fp_state {
    qword_t f[64] __attribute__((aligned(16)));
    dword_t fcsr;
    dword_t reserved[3];
};

union rv_fp_state {
    struct rv_d_fp_state d;
    struct rv_q_fp_state q;
};
static_assert(sizeof(union rv_fp_state) == 528, "riscv64 fp state size");

struct rv_ctx_hdr {
    dword_t magic;
    dword_t size;
};

struct rv_extra_ext_header {
    dword_t padding[129] __attribute__((aligned(16)));
    dword_t reserved;
    struct rv_ctx_hdr hdr;
};

struct rv_sigcontext {
    struct rv_user_regs regs;
    union rv_fp_state fpregs;
};

struct rv_ucontext {
    qword_t flags;
    addr_t link;
    qword_t stack;
    dword_t stack_flags;
    dword_t stack_pad;
    qword_t stack_size;
    sigset_t_ sigmask;
    uint8_t unused[128 - sizeof(sigset_t_)];
    qword_t pad;
    struct rv_sigcontext mcontext;
};

struct rv_rt_sigframe {
    struct rv_siginfo_ info;
    struct rv_ucontext uc;
    dword_t sigreturn_code[2];
};
static_assert(offsetof(struct rv_rt_sigframe, uc) == 128, "riscv64 rt_sigframe ucontext offset");
static_assert(offsetof(struct rv_ucontext, mcontext) == 176, "riscv64 ucontext mcontext offset");

static bool signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
}

static int signal_action(struct sighand *sighand, int sig) {
    if (signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if (action->handler == SIG_IGN_)
            return 0;
        if (action->handler != SIG_DFL_)
            return 2;
    }

    switch (sig) {
        case SIGURG_: case SIGCONT_: case SIGCHLD_:
        case SIGIO_: case SIGWINCH_:
            return 0;
        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            return 3;
        default:
            return 1;
    }
}

static struct rv_siginfo_ rv_siginfo_from_siginfo(struct siginfo_ info) {
    struct rv_siginfo_ user_info = {
        .sig = info.sig,
        .sig_errno = info.sig_errno,
        .code = info.code,
    };

    switch (info.code) {
    case SI_USER_:
    case SI_TKILL_:
        user_info.kill.pid = info.kill.pid;
        user_info.kill.uid = info.kill.uid;
        break;
    case SI_QUEUE_:
        user_info.queue.pid = info.queue.pid;
        user_info.queue.uid = info.queue.uid;
        user_info.queue.value = info.queue.value;
        break;
    case SI_TIMER_:
        user_info.timer.timer = info.timer.timer;
        user_info.timer.overrun = info.timer.overrun;
        user_info.timer.value = info.timer.value;
        user_info.timer._private = info.timer._private;
        break;
    default:
        break;
    }

    if (info.sig == SIGCHLD_) {
        user_info.child.pid = info.child.pid;
        user_info.child.uid = info.child.uid;
        user_info.child.status = info.child.status;
        user_info.child.utime = info.child.utime;
        user_info.child.stime = info.child.stime;
    } else if (info.sig == SIGILL_ || info.sig == SIGFPE_ ||
            info.sig == SIGSEGV_ || info.sig == SIGBUS_ ||
            info.sig == SIGTRAP_) {
        user_info.fault.addr = info.fault.addr;
    } else if (info.sig == SIGSYS_) {
        user_info.sigsys.addr = info.sigsys.addr;
        user_info.sigsys.syscall = info.sigsys.syscall;
    }

    return user_info;
}

static struct siginfo_ siginfo_from_rv_siginfo(struct rv_siginfo_ user_info) {
    struct siginfo_ info = {
        .sig = user_info.sig,
        .sig_errno = user_info.sig_errno,
        .code = user_info.code,
    };
    switch (user_info.code) {
    case SI_USER_:
    case SI_TKILL_:
        info.kill.pid = user_info.kill.pid;
        info.kill.uid = user_info.kill.uid;
        break;
    case SI_QUEUE_:
        info.queue.pid = user_info.queue.pid;
        info.queue.uid = user_info.queue.uid;
        info.queue.value = user_info.queue.value;
        break;
    case SI_TIMER_:
        info.timer.timer = user_info.timer.timer;
        info.timer.overrun = user_info.timer.overrun;
        info.timer.value = user_info.timer.value;
        info.timer._private = user_info.timer._private;
        break;
    default:
        break;
    }
    return info;
}

static sigset_t_ signalfd_sanitize_mask(sigset_t_ mask) {
    return mask & ~(sig_mask(SIGKILL_) | sig_mask(SIGSTOP_));
}

static bool signalfd_fd_has_signal(struct fd *fd, int sig) {
    bool has_signal;
    lock(&fd->lock);
    has_signal = sigset_has(fd->signalfd.mask, sig);
    unlock(&fd->lock);
    return has_signal;
}

static bool task_has_signalfd_signal(struct task *task, int sig) {
    if (task->files == NULL)
        return false;

    bool found = false;
    lock(&task->files->lock);
    for (fd_t f = 0; (unsigned) f < task->files->size; f++) {
        struct fd *fd = fdtable_get(task->files, f);
        if (fd == NULL || fd->ops != &signalfd_ops)
            continue;
        if (signalfd_fd_has_signal(fd, sig)) {
            found = true;
            break;
        }
    }
    unlock(&task->files->lock);
    return found;
}

static void signalfd_notify_signal(struct task *task, int sig) {
    if (task->files == NULL)
        return;

    lock(&task->files->lock);
    for (fd_t f = 0; (unsigned) f < task->files->size; f++) {
        struct fd *fd = fdtable_get(task->files, f);
        if (fd == NULL || fd->ops != &signalfd_ops)
            continue;
        if (signalfd_fd_has_signal(fd, sig))
            poll_wakeup(fd, POLL_READ);
    }
    unlock(&task->files->lock);
}

static void sigmask_set(sigset_t_ set) {
    current->blocked = set & ~(sig_mask(SIGKILL_) | sig_mask(SIGSTOP_));
}

struct sighand *sighand_new(void) {
    struct sighand *sighand = malloc(sizeof(*sighand));
    if (sighand == NULL)
        return NULL;
    memset(sighand, 0, sizeof(*sighand));
    sighand->refcount = 1;
    lock_init(&sighand->lock);
    return sighand;
}

struct sighand *sighand_copy(struct sighand *sighand) {
    struct sighand *copy = sighand_new();
    if (copy == NULL)
        return NULL;
    memcpy(copy->action, sighand->action, sizeof(copy->action));
    return copy;
}

void sighand_release(struct sighand *sighand) {
    if (--sighand->refcount == 0)
        free(sighand);
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    if ((sig == SIGKILL_ || sig == SIGSEGV_ || sig == SIGILL_ || sig == SIGBUS_) && task == current)
        do_exit(128 + sig);

    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    bool queued = false;
    if (!sigset_has(task->pending, sig)) {
        sigset_add(&task->pending, sig);
        struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
        if (sigqueue != NULL) {
            sigqueue->info = info;
            sigqueue->info.sig = sig;
            list_add_tail(&task->queue, &sigqueue->queue);
            queued = true;
        }
    }

    if (task != current) {
        cpu_poke(&task->cpu);
        pthread_kill(task->thread, SIGUSR1);

        unlock(&sighand->lock);
        if (queued)
            signalfd_notify_signal(task, sig);
retry:
        lock(&task->waiting_cond_lock);
        if (task->waiting_cond != NULL) {
            bool mine = false;
            if (trylock(task->waiting_lock) == EBUSY) {
                if (pthread_equal(task->waiting_lock->owner, pthread_self()))
                    mine = true;
                if (!mine) {
                    unlock(&task->waiting_cond_lock);
                    goto retry;
                }
            }
            notify(task->waiting_cond);
            if (!mine)
                unlock(task->waiting_lock);
        }
        unlock(&task->waiting_cond_lock);
        return;
    }
    unlock(&sighand->lock);
    if (queued)
        signalfd_notify_signal(task, sig);
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    if (sig == 0)
        return;
    if (task->zombie || task->exiting)
        return;

    lock(&task->sighand->lock);
    bool waiting = sigset_has(task->waiting, sig);
    int action = signal_action(task->sighand, sig);
    unlock(&task->sighand->lock);
    if (action != 0 || waiting || task_has_signalfd_signal(task, sig))
        deliver_signal(task, sig, info);

    if (sig == SIGCONT_ || sig == SIGKILL_) {
        lock(&task->group->lock);
        task->group->stopped = false;
        notify(&task->group->stopped_cond);
        unlock(&task->group->lock);
    }
}

static void save_regs(struct rv_user_regs *regs) {
    struct cpu_state *cpu = &current->cpu;
    regs->pc = cpu->pc;
    regs->ra = cpu->ra;
    regs->sp = cpu->sp;
    regs->gp = cpu->gp;
    regs->tp = cpu->tp;
    regs->t0 = cpu->t0;
    regs->t1 = cpu->t1;
    regs->t2 = cpu->t2;
    regs->s0 = cpu->s0;
    regs->s1 = cpu->s1;
    regs->a0 = cpu->a0;
    regs->a1 = cpu->a1;
    regs->a2 = cpu->a2;
    regs->a3 = cpu->a3;
    regs->a4 = cpu->a4;
    regs->a5 = cpu->a5;
    regs->a6 = cpu->a6;
    regs->a7 = cpu->a7;
    regs->s2 = cpu->s2;
    regs->s3 = cpu->s3;
    regs->s4 = cpu->s4;
    regs->s5 = cpu->s5;
    regs->s6 = cpu->s6;
    regs->s7 = cpu->s7;
    regs->s8 = cpu->s8;
    regs->s9 = cpu->s9;
    regs->s10 = cpu->s10;
    regs->s11 = cpu->s11;
    regs->t3 = cpu->t3;
    regs->t4 = cpu->t4;
    regs->t5 = cpu->t5;
    regs->t6 = cpu->t6;
}

static void restore_regs(struct rv_user_regs *regs) {
    struct cpu_state *cpu = &current->cpu;
    cpu->pc = regs->pc;
    cpu->ra = regs->ra;
    cpu->sp = regs->sp;
    cpu->gp = regs->gp;
    cpu->tp = regs->tp;
    cpu->t0 = regs->t0;
    cpu->t1 = regs->t1;
    cpu->t2 = regs->t2;
    cpu->s0 = regs->s0;
    cpu->s1 = regs->s1;
    cpu->a0 = regs->a0;
    cpu->a1 = regs->a1;
    cpu->a2 = regs->a2;
    cpu->a3 = regs->a3;
    cpu->a4 = regs->a4;
    cpu->a5 = regs->a5;
    cpu->a6 = regs->a6;
    cpu->a7 = regs->a7;
    cpu->s2 = regs->s2;
    cpu->s3 = regs->s3;
    cpu->s4 = regs->s4;
    cpu->s5 = regs->s5;
    cpu->s6 = regs->s6;
    cpu->s7 = regs->s7;
    cpu->s8 = regs->s8;
    cpu->s9 = regs->s9;
    cpu->s10 = regs->s10;
    cpu->s11 = regs->s11;
    cpu->t3 = regs->t3;
    cpu->t4 = regs->t4;
    cpu->t5 = regs->t5;
    cpu->t6 = regs->t6;
}

static bool is_on_altstack(addr_t sp) {
    return sp > current->altstack && sp <= current->altstack + current->altstack_size;
}

static void altstack_to_user(struct stack_t_ *user_stack) {
    user_stack->stack = current->altstack;
    user_stack->size = current->altstack_size;
    user_stack->flags = 0;
    if (current->altstack == 0)
        user_stack->flags |= SS_DISABLE_;
    if (is_on_altstack(current->cpu.sp))
        user_stack->flags |= SS_ONSTACK_;
}

static int setup_signal_handler(struct sigqueue *sigqueue) {
    int sig = sigqueue->info.sig;
    struct sigaction_ *action = &current->sighand->action[sig];
    addr_t handler = action->handler;
    qword_t flags = action->flags;
    sigset_t_ mask = action->mask;
    addr_t sp = current->cpu.sp;
    if ((flags & SA_ONSTACK_) && current->altstack != 0 &&
            !is_on_altstack(sp)) {
        sp = current->altstack + current->altstack_size;
    }
    addr_t frame_addr = (sp - sizeof(struct rv_rt_sigframe)) & ~0xfUL;
    struct rv_rt_sigframe frame = {};

    frame.info = rv_siginfo_from_siginfo(sigqueue->info);
    frame.uc.sigmask = current->blocked;
    struct stack_t_ stack;
    altstack_to_user(&stack);
    frame.uc.stack = stack.stack;
    frame.uc.stack_flags = stack.flags;
    frame.uc.stack_size = stack.size;
    save_regs(&frame.uc.mcontext.regs);
    memcpy(frame.uc.mcontext.fpregs.d.f, current->cpu.f, sizeof(frame.uc.mcontext.fpregs.d.f));
    frame.uc.mcontext.fpregs.d.fcsr = current->cpu.fcsr;
    frame.sigreturn_code[0] = 0x08b00893; // li a7, __NR_rt_sigreturn
    frame.sigreturn_code[1] = 0x00000073; // ecall
    if (user_put(frame_addr, frame))
        return _EFAULT;

    current->cpu.ra = frame_addr + offsetof(struct rv_rt_sigframe, sigreturn_code);
    current->cpu.pc = handler;
    current->cpu.sp = frame_addr;
    current->cpu.a0 = sig;
    current->cpu.a1 = frame_addr + offsetof(struct rv_rt_sigframe, info);
    current->cpu.a2 = frame_addr + offsetof(struct rv_rt_sigframe, uc);

    if (flags & SA_RESETHAND_)
        *action = (struct sigaction_) {.handler = SIG_DFL_};
    sigmask_set(current->blocked | mask |
        ((flags & SA_NODEFER_) ? 0 : sig_mask(sig)));
    return 0;
}

bool try_self_signal(int sig) {
    deliver_signal(current, sig, SIGINFO_NIL);
    return true;
}

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    lock(&pids_lock);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        send_signal(tgroup->leader, sig, info);
    }
    unlock(&pids_lock);
    return 0;
}

void receive_signals(void) {
    lock(&current->group->lock);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    if (current->has_saved_mask) {
        current->blocked = current->saved_mask;
        current->has_saved_mask = false;
    }

    lock(&current->sighand->lock);
    struct sigqueue *sigqueue, *tmp;
    list_for_each_entry_safe(&current->queue, sigqueue, tmp, queue) {
        int sig = sigqueue->info.sig;
        if (sigset_has(current->blocked, sig))
            continue;
        int action = signal_action(current->sighand, sig);
        list_remove(&sigqueue->queue);
        sigset_del(&current->pending, sig);
        if (action == 1) {
            unlock(&current->sighand->lock);
            free(sigqueue);
            do_exit(128 + sig);
        }
        if (action == 2) {
            int err = setup_signal_handler(sigqueue);
            unlock(&current->sighand->lock);
            free(sigqueue);
            if (err < 0)
                do_exit(128 + SIGSEGV_);
            return;
        }
        if (action == 3) {
            lock(&current->group->lock);
            current->group->stopped = true;
            current->group->group_exit_code = sig << 8 | 0x7f;
            unlock(&current->group->lock);
        }
        free(sigqueue);
    }
    unlock(&current->sighand->lock);

    if (!was_stopped) {
        lock(&current->group->lock);
        bool now_stopped = current->group->stopped;
        unlock(&current->group->lock);
        if (now_stopped) {
            lock(&pids_lock);
            notify(&current->parent->group->child_exit);
            send_signal(current->parent, current->group->leader->exit_signal, SIGINFO_NIL);
            unlock(&pids_lock);
        }
    }
}

void sigmask_set_temp(sigset_t_ mask) {
    current->saved_mask = current->blocked;
    current->has_saved_mask = true;
    sigmask_set(mask);
}

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size) {
    if (signum >= NUM_SIGS || !signal_is_blockable(signum))
        return _EINVAL;
    if (sigset_size != sizeof(sigset_t_))
        return _EINVAL;

    struct sigaction_ action;
    struct sigaction_ *action_ptr = NULL;
    if (action_addr != 0) {
        if (user_get(action_addr, action))
            return _EFAULT;
        action_ptr = &action;
    }

    lock(&current->sighand->lock);
    if (oldaction_addr != 0 && user_put(oldaction_addr, current->sighand->action[signum])) {
        unlock(&current->sighand->lock);
        return _EFAULT;
    }
    if (action_ptr != NULL)
        current->sighand->action[signum] = *action_ptr;
    unlock(&current->sighand->lock);
    return 0;
}

dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, sizeof(sigset_t_));
}

dword_t sys_rt_sigreturn(void) {
    struct rv_rt_sigframe frame;
    if (user_get(current->cpu.sp, frame))
        do_exit(128 + SIGSEGV_);
    current->blocked = frame.uc.sigmask & ~(sig_mask(SIGKILL_) | sig_mask(SIGSTOP_));
    restore_regs(&frame.uc.mcontext.regs);
    memcpy(current->cpu.f, frame.uc.mcontext.fpregs.d.f, sizeof(current->cpu.f));
    current->cpu.fcsr = frame.uc.mcontext.fpregs.d.fcsr;
    return current->cpu.a0;
}

dword_t sys_sigreturn(void) {
    return _ENOSYS;
}

dword_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, dword_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;
    if (oldset_addr != 0 && user_put(oldset_addr, current->blocked))
        return _EFAULT;
    if (set_addr == 0)
        return 0;

    sigset_t_ set;
    if (user_get(set_addr, set))
        return _EFAULT;
    switch (how) {
    case SIG_BLOCK_:
        sigmask_set(current->blocked | set);
        return 0;
    case SIG_UNBLOCK_:
        sigmask_set(current->blocked & ~set);
        return 0;
    case SIG_SETMASK_:
        sigmask_set(set);
        return 0;
    default:
        return _EINVAL;
    }
}

int_t sys_rt_sigpending(addr_t set_addr) {
    sigset_t_ pending = current->pending & current->blocked;
    if (user_put(set_addr, pending))
        return _EFAULT;
    return 0;
}

dword_t sys_sigaltstack(addr_t ss_addr, addr_t old_ss_addr) {
    if (old_ss_addr != 0) {
        struct stack_t_ old_ss;
        altstack_to_user(&old_ss);
        if (user_put(old_ss_addr, old_ss))
            return _EFAULT;
    }
    if (ss_addr != 0) {
        if (is_on_altstack(current->cpu.sp))
            return _EPERM;
        struct stack_t_ ss;
        if (user_get(ss_addr, ss))
            return _EFAULT;
        if (ss.flags & SS_DISABLE_) {
            current->altstack = 0;
            current->altstack_size = 0;
        } else {
            if (ss.size < MINSIGSTKSZ_)
                return _ENOMEM;
            current->altstack = ss.stack;
            current->altstack_size = ss.size;
        }
    }
    return 0;
}

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    sigmask_set_temp(mask);
    return _EINTR;
}

int_t sys_pause(void) {
    return _EINTR;
}

int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    if (set_size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ set;
    if (user_get(set_addr, set))
        return _EFAULT;
    struct timespec timeout;
    if (timeout_addr != 0) {
        struct timespec_ timeout_;
        if (user_get(timeout_addr, timeout_))
            return _EFAULT;
        if (timeout_.sec < 0 || timeout_.nsec < 0 || timeout_.nsec >= 1000000000)
            return _EINVAL;
        timeout.tv_sec = timeout_.sec;
        timeout.tv_nsec = timeout_.nsec;
    }

    lock(&current->sighand->lock);
    assert(current->waiting == 0);
    current->waiting = set;

    struct sigqueue *sigqueue;
    int err = 0;
    while (true) {
        list_for_each_entry(&current->queue, sigqueue, queue) {
            if (sigset_has(set, sigqueue->info.sig)) {
                list_remove(&sigqueue->queue);
                sigset_del(&current->pending, sigqueue->info.sig);
                goto found;
            }
        }
        err = wait_for(&current->pause, &current->sighand->lock, timeout_addr == 0 ? NULL : &timeout);
        if (err != 0)
            break;
    }
    current->waiting = 0;
    unlock(&current->sighand->lock);
    if (err == _ETIMEDOUT)
        return _EAGAIN;
    return _EINTR;

found:
    current->waiting = 0;
    struct siginfo_ info = sigqueue->info;
    free(sigqueue);
    unlock(&current->sighand->lock);
    if (info_addr != 0) {
        struct rv_siginfo_ user_info = rv_siginfo_from_siginfo(info);
        if (user_put(info_addr, user_info))
            return _EFAULT;
    }
    return info.sig;
}

static bool signalfd_dequeue_signal(struct fd *fd, struct siginfo_ *info) {
    sigset_t_ mask;
    lock(&fd->lock);
    mask = fd->signalfd.mask;
    unlock(&fd->lock);

    struct sigqueue *sigqueue, *tmp;
    list_for_each_entry_safe(&current->queue, sigqueue, tmp, queue) {
        if (!sigset_has(mask, sigqueue->info.sig))
            continue;

        list_remove(&sigqueue->queue);
        sigset_del(&current->pending, sigqueue->info.sig);
        *info = sigqueue->info;
        free(sigqueue);
        return true;
    }
    return false;
}

static struct signalfd_siginfo_ signalfd_siginfo_from_siginfo(struct siginfo_ info) {
    struct signalfd_siginfo_ ssi = {
        .signo = info.sig,
        .sig_errno = info.sig_errno,
        .code = info.code,
    };

    switch (info.code) {
    case SI_USER_:
    case SI_TKILL_:
        ssi.pid = info.kill.pid;
        ssi.uid = info.kill.uid;
        break;
    case SI_TIMER_:
        ssi.overrun = info.timer.overrun;
        ssi.ssi_int = info.timer.value.sv_int;
        ssi.ptr = info.timer.value.sv_ptr;
        break;
    default:
        break;
    }

    if (info.sig == SIGCHLD_) {
        ssi.pid = info.child.pid;
        ssi.uid = info.child.uid;
        ssi.status = info.child.status;
        ssi.utime = info.child.utime;
        ssi.stime = info.child.stime;
    } else if (info.sig == SIGILL_ || info.sig == SIGFPE_ ||
            info.sig == SIGSEGV_ || info.sig == SIGBUS_ ||
            info.sig == SIGTRAP_) {
        ssi.addr = info.fault.addr;
    } else if (info.sig == SIGSYS_) {
        ssi.addr = info.sigsys.addr;
        ssi.syscall = info.sigsys.syscall;
    }

    return ssi;
}

static ssize_t signalfd_read(struct fd *fd, void *buf, size_t bufsize) {
    size_t max_infos = bufsize / sizeof(struct signalfd_siginfo_);
    if (max_infos == 0)
        return _EINVAL;

    struct signalfd_siginfo_ *out = buf;
    size_t count = 0;

    lock(&current->sighand->lock);
    while (count == 0) {
        struct siginfo_ info;
        while (count < max_infos && signalfd_dequeue_signal(fd, &info))
            out[count++] = signalfd_siginfo_from_siginfo(info);
        if (count != 0)
            break;

        if (fd->flags & O_NONBLOCK_) {
            unlock(&current->sighand->lock);
            return _EAGAIN;
        }

        int err = wait_for(&current->pause, &current->sighand->lock, NULL);
        if (err < 0) {
            unlock(&current->sighand->lock);
            return err;
        }
    }
    unlock(&current->sighand->lock);
    return count * sizeof(struct signalfd_siginfo_);
}

static int signalfd_poll(struct fd *fd) {
    int res = 0;
    lock(&current->sighand->lock);
    sigset_t_ mask;
    lock(&fd->lock);
    mask = fd->signalfd.mask;
    unlock(&fd->lock);
    if (current->pending & mask)
        res |= POLL_READ;
    unlock(&current->sighand->lock);
    return res;
}

int_t sys_signalfd4(fd_t f, addr_t mask_addr, dword_t mask_size, int_t flags) {
    STRACE("signalfd4(%d, %#x, %u, %#x)", f, mask_addr, mask_size, flags);
    if (flags & ~(O_CLOEXEC_ | O_NONBLOCK_))
        return _EINVAL;
    if (mask_size != sizeof(sigset_t_))
        return _EINVAL;

    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    mask = signalfd_sanitize_mask(mask);

    if (f == -1) {
        struct fd *fd = adhoc_fd_create(&signalfd_ops);
        if (fd == NULL)
            return _ENOMEM;
        fd->signalfd.mask = mask;
        return f_install(fd, flags);
    }

    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &signalfd_ops)
        return _EINVAL;

    lock(&fd->lock);
    fd->signalfd.mask = mask;
    unlock(&fd->lock);
    lock(&current->sighand->lock);
    bool readable = current->pending & mask;
    unlock(&current->sighand->lock);
    if (readable)
        poll_wakeup(fd, POLL_READ);
    return f;
}

int_t sys_signalfd(fd_t f, addr_t mask_addr, dword_t mask_size) {
    return sys_signalfd4(f, mask_addr, mask_size, 0);
}

static struct fd_ops signalfd_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
};

static int kill_task_info(struct task *task, dword_t sig, struct siginfo_ info) {
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid)
        return _EPERM;
    send_signal(task, sig, info);
    return 0;
}

static int kill_task(struct task *task, dword_t sig) {
    struct siginfo_ info = {
        .code = SI_USER_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    return kill_task_info(task, sig, info);
}

static int kill_group(pid_t_ pgid, dword_t sig) {
    struct pid *pid = pid_get(pgid);
    if (pid == NULL)
        return _ESRCH;
    struct tgroup *tgroup;
    int err = _EPERM;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        int kill_err = kill_task(tgroup->leader, sig);
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int kill_everything(dword_t sig) {
    int err = _EPERM;
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task == NULL || task == current || !task_is_leader(task))
            continue;
        int kill_err = kill_task(task, sig);
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int do_kill(pid_t_ pid, dword_t sig, pid_t_ tgid) {
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (pid == 0)
        pid = -current->group->pgid;

    int err;
    lock(&pids_lock);
    if (pid == -1) {
        err = kill_everything(sig);
    } else if (pid < 0) {
        err = kill_group(-pid, sig);
    } else {
        struct task *task = pid_get_task(pid);
        if (task == NULL) {
            unlock(&pids_lock);
            return _ESRCH;
        }
        if (tgid != 0 && task->tgid != tgid) {
            unlock(&pids_lock);
            return _ESRCH;
        }
        err = kill_task(task, sig);
    }
    unlock(&pids_lock);
    return err;
}

dword_t sys_kill(pid_t_ pid, dword_t sig) {
    return do_kill(pid, sig, 0);
}

dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    if (tid <= 0 || tgid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, tgid);
}

dword_t sys_tkill(pid_t_ tid, dword_t sig) {
    if (tid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, 0);
}

static int do_rt_sigqueueinfo(pid_t_ tid, pid_t_ tgid, dword_t sig, struct siginfo_ info) {
    if (sig >= NUM_SIGS || tid <= 0)
        return _EINVAL;
    if (info.sig != 0 && info.sig != (int_t) sig)
        return _EINVAL;
    info.sig = sig;

    lock(&pids_lock);
    struct task *task = pid_get_task(tid);
    if (task == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    if (tgid != 0 && task->tgid != tgid) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    int err = kill_task_info(task, sig, info);
    unlock(&pids_lock);
    return err;
}

dword_t sys_rt_sigqueueinfo(pid_t_ pid, dword_t sig, addr_t info_addr) {
    struct rv_siginfo_ user_info;
    if (user_get(info_addr, user_info))
        return _EFAULT;
    return do_rt_sigqueueinfo(pid, 0, sig, siginfo_from_rv_siginfo(user_info));
}

dword_t sys_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid, dword_t sig, addr_t info_addr) {
    struct rv_siginfo_ user_info;
    if (user_get(info_addr, user_info))
        return _EFAULT;
    return do_rt_sigqueueinfo(tid, tgid, sig, siginfo_from_rv_siginfo(user_info));
}
