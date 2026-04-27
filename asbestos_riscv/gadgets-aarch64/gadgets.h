#include "../gadgets-generic.h"

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
    ldr rv_ra, [_cpu, 24]   /* cpu.x[1] */
    ldr rv_sp, [_cpu, 32]   /* cpu.x[2] */
    ldr rv_gp, [_cpu, 40]   /* cpu.x[3] */
    ldr rv_tp, [_cpu, 48]   /* cpu.x[4] */
    ldr rv_t0, [_cpu, 56]   /* cpu.x[5] */
    ldr rv_t1, [_cpu, 64]   /* cpu.x[6] */
    ldr rv_t2, [_cpu, 72]   /* cpu.x[7] */
    ldr rv_s0, [_cpu, 80]   /* cpu.x[8] */
    ldr rv_a0, [_cpu, 96]   /* cpu.x[10] */
    ldr rv_a1, [_cpu, 104]  /* cpu.x[11] */
    ldr rv_a2, [_cpu, 112]  /* cpu.x[12] */
    ldr rv_a3, [_cpu, 120]  /* cpu.x[13] */
    ldr rv_a4, [_cpu, 128]  /* cpu.x[14] */
    ldr rv_a5, [_cpu, 136]  /* cpu.x[15] */
    ldr rv_a6, [_cpu, 144]  /* cpu.x[16] */
    ldr rv_a7, [_cpu, 152]  /* cpu.x[17] */
.endm

.macro rv_save_hot_regs
    str xzr, [_cpu, 16]     /* cpu.x[0] */
    str rv_ra, [_cpu, 24]
    str rv_sp, [_cpu, 32]
    str rv_gp, [_cpu, 40]
    str rv_tp, [_cpu, 48]
    str rv_t0, [_cpu, 56]
    str rv_t1, [_cpu, 64]
    str rv_t2, [_cpu, 72]
    str rv_s0, [_cpu, 80]
    str rv_a0, [_cpu, 96]
    str rv_a1, [_cpu, 104]
    str rv_a2, [_cpu, 112]
    str rv_a3, [_cpu, 120]
    str rv_a4, [_cpu, 128]
    str rv_a5, [_cpu, 136]
    str rv_a6, [_cpu, 144]
    str rv_a7, [_cpu, 152]
.endm

# vim: ft=gas
