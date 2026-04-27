#include "emu/interrupt.h"
#include "gadgets.h"

# Hard-coded until asbestos_riscv grows its own generated offsets file.
# Keep in sync with emu_riscv/cpu.h: pc aliases eip after mmu, cycle, x[32].
.equ RV_CPU_pc, 272
.equ RV_FIBER_BLOCK_code, 192

.macro rv_write_pc_and_continue param_count
    ldr x3, [_ip, (\param_count - 1) * 8]
    str x3, [_cpu, RV_CPU_pc]
    rv_gret \param_count
.endm

.macro rv_load_x dst, reg
    cbz \reg, 1f
    add x7, _cpu, 16
    ldr \dst, [x7, \reg, lsl 3]
    b 2f
1:
    mov \dst, xzr
2:
.endm

.macro rv_store_x src, reg
    cbz \reg, 1f
    add x7, _cpu, 16
    str \src, [x7, \reg, lsl 3]
1:
.endm

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

.rv_gadget lui
    ldr x3, [_ip]      /* rd */
    ldr x4, [_ip, 8]  /* imm */
    rv_store_x x4, x3
    rv_write_pc_and_continue 3

.rv_gadget auipc
    ldr x3, [_ip]      /* rd */
    ldr x4, [_ip, 8]  /* imm */
    ldr x5, [_ip, 16] /* pc */
    add x4, x4, x5
    rv_store_x x4, x3
    rv_write_pc_and_continue 4

.rv_gadget addi
    ldr x3, [_ip]      /* rd */
    ldr x4, [_ip, 8]  /* rs1 */
    ldr x5, [_ip, 16] /* imm */
    rv_load_x x6, x4
    add x6, x6, x5
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget add
    ldr x3, [_ip]      /* rd */
    ldr x4, [_ip, 8]  /* rs1 */
    ldr x5, [_ip, 16] /* rs2 */
    rv_load_x x6, x4
    rv_load_x x8, x5
    add x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sub
    ldr x3, [_ip]      /* rd */
    ldr x4, [_ip, 8]  /* rs1 */
    ldr x5, [_ip, 16] /* rs2 */
    rv_load_x x6, x4
    rv_load_x x8, x5
    sub x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

# vim: ft=gas
