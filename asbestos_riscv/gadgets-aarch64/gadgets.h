#if __APPLE__
#define NAME(x) _##x
#else
#define NAME(x) x
#endif

.macro .type_compat type:vararg
#if !__APPLE__
    .type \type
#endif
.endm

/*
 * RV64GC -> AArch64 gadget ABI.
 *
 * This file is the RV64 equivalent of asbestos/gadgets-aarch64/gadgets.h, but
 * without i386 flags, segments, or 32-bit register assumptions. The current
 * production RV64 path is interpreter-backed; these definitions pin the ABI
 * that the gadget pool is being filled into.
 */

# register assignments
_tmp .req x0
_cpu .req x1
_tlb .req x2
_addr .req x3
_ip .req x28

# Hot RV registers pinned in callee-saved host registers. x8-x15 remain
# scratch registers in the existing RV64 gadget ABI, so the a-registers stay
# memory-backed until the scratch convention is widened.
rv_ra .req x19
rv_sp .req x20
rv_gp .req x21
rv_tp .req x22
rv_t0 .req x23
rv_t1 .req x24
rv_t2 .req x25
rv_s0 .req x26
rv_s1 .req x27

.macro .rv_gadget name
    .global NAME(gadget_rv_\()\name)
    .align 4
    NAME(gadget_rv_\()\name) :
.endm

.macro rv_gret pop=0
    ldr x16, [_ip, \pop*8]!
    add _ip, _ip, 8
    br x16
.endm

.macro rv_load_hot_regs
    ldp rv_ra, rv_sp, [_cpu, 24]
    ldp rv_gp, rv_tp, [_cpu, 40]
    ldp rv_t0, rv_t1, [_cpu, 56]
    ldp rv_t2, rv_s0, [_cpu, 72]
    ldr rv_s1, [_cpu, 88]
.endm

.macro rv_save_hot_regs
    stp rv_ra, rv_sp, [_cpu, 24]
    stp rv_gp, rv_tp, [_cpu, 40]
    stp rv_t0, rv_t1, [_cpu, 56]
    stp rv_t2, rv_s0, [_cpu, 72]
    str rv_s1, [_cpu, 88]
.endm

# vim: ft=gas
