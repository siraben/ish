#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"

static bool signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
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
    (void) info;
    if (sig == SIGKILL_ || sig == SIGSEGV_ || sig == SIGILL_ || sig == SIGBUS_) {
        if (task == current)
            do_exit(128 + sig);
        cpu_poke(&task->cpu);
    }
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    deliver_signal(task, sig, info);
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
    return _ENOSYS;
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
    (void) set_addr;
    (void) info_addr;
    (void) timeout_addr;
    if (set_size != sizeof(sigset_t_))
        return _EINVAL;
    return _EAGAIN;
}

dword_t sys_kill(pid_t_ pid, dword_t sig) {
    (void) pid;
    (void) sig;
    return 0;
}

dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    (void) tgid;
    (void) tid;
    (void) sig;
    return 0;
}

dword_t sys_tkill(pid_t_ tid, dword_t sig) {
    return sys_tgkill(0, tid, sig);
}
