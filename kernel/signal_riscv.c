#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>

#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"

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
    union {
        struct rv_d_fp_state fpregs;
        struct rv_extra_ext_header extdesc;
    };
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
    struct rv_sigcontext mcontext;
};

struct rv_rt_sigframe {
    struct siginfo_ info;
    struct rv_ucontext uc;
    dword_t sigreturn_code[2];
};

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
    copy->altstack = sighand->altstack;
    copy->altstack_size = sighand->altstack_size;
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
    if (!sigset_has(task->pending, sig)) {
        sigset_add(&task->pending, sig);
        struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
        if (sigqueue != NULL) {
            sigqueue->info = info;
            sigqueue->info.sig = sig;
            list_add_tail(&task->queue, &sigqueue->queue);
        }
    }

    if (task != current) {
        cpu_poke(&task->cpu);

        unlock(&sighand->lock);
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
    if (action != 0 || waiting)
        deliver_signal(task, sig, info);
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

static int setup_signal_handler(struct sigqueue *sigqueue) {
    int sig = sigqueue->info.sig;
    struct sigaction_ *action = &current->sighand->action[sig];
    addr_t frame_addr = (current->cpu.sp - sizeof(struct rv_rt_sigframe)) & ~0xfUL;
    struct rv_rt_sigframe frame = {};

    frame.info = sigqueue->info;
    frame.uc.sigmask = current->blocked;
    save_regs(&frame.uc.mcontext.regs);
    memcpy(frame.uc.mcontext.fpregs.f, current->cpu.f, sizeof(frame.uc.mcontext.fpregs.f));
    frame.uc.mcontext.fpregs.fcsr = current->cpu.fcsr;
    frame.sigreturn_code[0] = 0x08b00893; // li a7, __NR_rt_sigreturn
    frame.sigreturn_code[1] = 0x00000073; // ecall
    if (user_put(frame_addr, frame))
        return _EFAULT;

    current->cpu.ra = frame_addr + offsetof(struct rv_rt_sigframe, sigreturn_code);
    current->cpu.pc = action->handler;
    current->cpu.sp = frame_addr;
    current->cpu.a0 = sig;
    current->cpu.a1 = frame_addr + offsetof(struct rv_rt_sigframe, info);
    current->cpu.a2 = frame_addr + offsetof(struct rv_rt_sigframe, uc);

    sigmask_set(current->blocked | action->mask |
        ((action->flags & SA_NODEFER_) ? 0 : sig_mask(sig)));
    return 0;
}

bool try_self_signal(int sig) {
    deliver_signal(current, sig, SIGINFO_NIL);
    return true;
}

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    (void) pgid;
    (void) sig;
    (void) info;
    return 0;
}

void receive_signals(void) {
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
        free(sigqueue);
    }
    unlock(&current->sighand->lock);
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
    memcpy(current->cpu.f, frame.uc.mcontext.fpregs.f, sizeof(current->cpu.f));
    current->cpu.fcsr = frame.uc.mcontext.fpregs.fcsr;
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
        struct stack_t_ old = {
            .stack = current->sighand->altstack,
            .size = current->sighand->altstack_size,
        };
        if (user_put(old_ss_addr, old))
            return _EFAULT;
    }
    if (ss_addr != 0) {
        struct stack_t_ ss;
        if (user_get(ss_addr, ss))
            return _EFAULT;
        current->sighand->altstack = ss.stack;
        current->sighand->altstack_size = ss.size;
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
    if (info_addr != 0 && user_put(info_addr, info))
        return _EFAULT;
    return info.sig;
}

static int kill_task(struct task *task, dword_t sig) {
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid)
        return _EPERM;
    struct siginfo_ info = {
        .code = SI_USER_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    send_signal(task, sig, info);
    return 0;
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
