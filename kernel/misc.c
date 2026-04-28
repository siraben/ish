#include <string.h>
#include "kernel/calls.h"

#define PRCTL_SET_PDEATHSIG_ 1
#define PRCTL_GET_PDEATHSIG_ 2
#define PRCTL_SET_KEEPCAPS_ 8
#define PRCTL_SET_NAME_ 15
#define PRCTL_SET_SECCOMP_ 22
#define PRCTL_SET_NO_NEW_PRIVS_ 38
#define PRCTL_GET_NO_NEW_PRIVS_ 39

#define SECCOMP_SET_MODE_STRICT_ 0
#define SECCOMP_SET_MODE_FILTER_ 1
#define SECCOMP_FILTER_FLAG_NEW_LISTENER_ (1 << 3)

static int_t seccomp_set_mode(dword_t mode, dword_t flags) {
    switch (mode) {
        case SECCOMP_SET_MODE_STRICT_: {
            if (flags != 0)
                return _EINVAL;
            return 0;
        }
        case SECCOMP_SET_MODE_FILTER_:
            if (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER_)
                return _EINVAL;
            // iSH cannot enforce seccomp filters. Treat installation as a
            // compatibility no-op so programs that self-sandbox can continue.
            return 0;
        default:
            return _EINVAL;
    }
}

int_t sys_prctl(dword_t option, uint_t arg2, uint_t arg3, uint_t arg4, uint_t arg5) {
    switch (option) {
        case PRCTL_SET_PDEATHSIG_:
            STRACE("prctl(PR_SET_PDEATHSIG, %u)", arg2);
            if (arg2 >= NUM_SIGS)
                return _EINVAL;
            current->parent_death_signal = arg2;
            return 0;
        case PRCTL_GET_PDEATHSIG_:
            STRACE("prctl(PR_GET_PDEATHSIG, %#x)", arg2);
            if (user_put(arg2, current->parent_death_signal))
                return _EFAULT;
            return 0;
        case PRCTL_SET_KEEPCAPS_:
            // stub
            return 0;
        case PRCTL_SET_SECCOMP_:
            STRACE("prctl(PR_SET_SECCOMP, %#x)", arg2);
            return seccomp_set_mode(arg2, 0);
        case PRCTL_SET_NO_NEW_PRIVS_:
            STRACE("prctl(PR_SET_NO_NEW_PRIVS, %u)", arg2);
            if (arg2 != 1 || arg3 != 0 || arg4 != 0 || arg5 != 0)
                return _EINVAL;
            current->no_new_privs = true;
            return 0;
        case PRCTL_GET_NO_NEW_PRIVS_:
            STRACE("prctl(PR_GET_NO_NEW_PRIVS)");
            if (arg2 != 0 || arg3 != 0 || arg4 != 0 || arg5 != 0)
                return _EINVAL;
            return current->no_new_privs ? 1 : 0;
        case PRCTL_SET_NAME_: {
            char name[16];
            if (user_read_string(arg2, name, sizeof(name) - 1))
                return _EFAULT;
            name[sizeof(name) - 1] = '\0';
            STRACE("prctl(PRCTL_SET_NAME, \"%s\")", name);
            strcpy(current->comm, name);
            return 0;
        }
        default:
            STRACE("prctl(%#x)", option);
            return _EINVAL;
    }
}

int_t sys_seccomp(dword_t op, dword_t flags, addr_t UNUSED(args)) {
    STRACE("seccomp(%#x, %#x)", op, flags);
    return seccomp_set_mode(op, flags);
}

int_t sys_arch_prctl(int_t code, addr_t addr) {
    STRACE("arch_prctl(%#x, %#x)", code, addr);
    return _EINVAL;
}

int_t sys_unshare(dword_t flags) {
    STRACE("unshare(%#x)", flags);
    if (flags == 0)
        return 0;
    return _EPERM;
}

int_t sys_getcpu(addr_t cpu_addr, addr_t node_addr, addr_t UNUSED(cache_addr)) {
    dword_t zero = 0;
    if (cpu_addr != 0 && user_put(cpu_addr, zero))
        return _EFAULT;
    if (node_addr != 0 && user_put(node_addr, zero))
        return _EFAULT;
    return 0;
}

#define REBOOT_MAGIC1 0xfee1dead
#define REBOOT_MAGIC2 672274793
#define REBOOT_MAGIC2A 85072278
#define REBOOT_MAGIC2B 369367448
#define REBOOT_MAGIC2C 537993216

#define REBOOT_CMD_CAD_OFF 0
#define REBOOT_CMD_CAD_ON 0x89abcdef

int_t sys_reboot(int_t magic, int_t magic2, int_t cmd) {
    STRACE("reboot(%#x, %d, %d)", magic, magic2, cmd);
    if (!superuser())
        return _EPERM;
    if (magic != (int) REBOOT_MAGIC1 ||
            (magic2 != REBOOT_MAGIC2 &&
             magic2 != REBOOT_MAGIC2A &&
             magic2 != REBOOT_MAGIC2B &&
             magic2 != REBOOT_MAGIC2C))
        return _EINVAL;

    switch (cmd) {
        case REBOOT_CMD_CAD_ON:
        case REBOOT_CMD_CAD_OFF:
            return 0;
        default:
            return _EPERM;
    }
}
