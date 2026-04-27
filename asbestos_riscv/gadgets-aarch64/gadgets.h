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

# Hot RV registers pinned in host registers.
rv_ra .req x20
rv_sp .req x21
rv_gp .req x22
rv_tp .req x23
rv_t0 .req x24
rv_t1 .req x25
rv_t2 .req x26
rv_s0 .req x27
rv_a0 .req x8
rv_a1 .req x9
rv_a2 .req x10
rv_a3 .req x11
rv_a4 .req x12
rv_a5 .req x13
rv_a6 .req x14
rv_a7 .req x15

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
.endm

.macro rv_save_hot_regs
.endm

# vim: ft=gas
