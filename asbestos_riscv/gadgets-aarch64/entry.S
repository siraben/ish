#include "emu/interrupt.h"
#include "gadgets.h"

# Hard-coded until asbestos_riscv grows its own generated offsets file.
# Keep in sync with emu_riscv/cpu.h: pc aliases eip after mmu, cycle, x[32].
.equ RV_CPU_pc, 272
.equ RV_FIBER_BLOCK_code, 192

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
    add _ip, x0, RV_FIBER_BLOCK_code
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
    mov _tmp, INT_NONE
    b NAME(fiber_exit)

.rv_gadget exec
    stp x1, x2, [sp, -0x30]!
    stp x3, x4, [sp, 0x10]
    str lr, [sp, 0x20]
    mov x0, _cpu
    mov x1, _tlb
    bl NAME(rv_exec_one_gadget)
    ldr lr, [sp, 0x20]
    ldp x3, x4, [sp, 0x10]
    ldp x1, x2, [sp], 0x30
    cmp w0, INT_NONE
    b.ne NAME(fiber_exit)
    ldr x3, [_ip]
    ldr x4, [_cpu, RV_CPU_pc]
    cmp x3, x4
    b.ne NAME(fiber_exit)
    rv_gret 1

# vim: ft=gas
