#include "emu/interrupt.h"
#include "gadgets.h"

.global NAME(fiber_enter)
.type_compat fiber_enter,function
NAME(fiber_enter):
    stp x19, x20, [sp, -0x80]!
    stp x21, x22, [sp, 0x10]
    stp x23, x24, [sp, 0x20]
    stp x25, x26, [sp, 0x30]
    stp x27, x28, [sp, 0x40]
    stp x29, x30, [sp, 0x50]
    mov _cpu, x1
    mov _tlb, x2
    add _ip, x0, 96 /* offsetof(struct fiber_block, code) on arm64 */
    rv_load_hot_regs
    rv_gret

.global NAME(fiber_ret)
NAME(fiber_ret):
    mov _tmp, INT_TIMER
    b NAME(fiber_exit)

.global NAME(fiber_ret_chain)
NAME(fiber_ret_chain):
    rv_gret

.global NAME(fiber_exit)
NAME(fiber_exit):
    rv_save_hot_regs
    ldp x29, x30, [sp, 0x50]
    ldp x27, x28, [sp, 0x40]
    ldp x25, x26, [sp, 0x30]
    ldp x23, x24, [sp, 0x20]
    ldp x21, x22, [sp, 0x10]
    ldp x19, x20, [sp], 0x80
    ret

.rv_gadget interrupt
    ldr _tmp, [_ip]
    b NAME(fiber_exit)

.rv_gadget exit
    mov _tmp, INT_TIMER
    b NAME(fiber_exit)

# vim: ft=gas
