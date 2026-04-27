#include "emu/interrupt.h"
#include "gadgets.h"

# Hard-coded until asbestos_riscv grows its own generated offsets file.
# Keep in sync with emu_riscv/cpu.h: pc aliases eip after mmu, cycle, x[32].
.equ RV_CPU_pc, 272
.equ RV_FIBER_BLOCK_code, 192
.equ RV_TLB_dirty_page, 8
.equ RV_TLB_entries, 32
.equ RV_TLB_ENTRY_page, 0
.equ RV_TLB_ENTRY_page_if_writable, 8
.equ RV_TLB_ENTRY_data_minus_addr, 16

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

.macro rv_tlb_index_and_page
    lsr x13, x12, 12
    lsr x14, x12, 24
    eor x13, x13, x14
    lsr x14, x12, 36
    eor x13, x13, x14
    and x13, x13, 0x3ff
    add x13, x13, x13, lsl 1
    add x13, _tlb, x13, lsl 3
    add x13, x13, RV_TLB_entries
    lsr x14, x12, 12
    lsl x14, x14, 12
.endm

.macro rv_tlb_read_prep bytes, id
    and x13, x12, 0xfff
    .if \bytes > 1
        tst x12, (\bytes - 1)
        b.ne .Lrv_slow_load_\id
    .endif
    cmp x13, (0x1000 - \bytes)
    b.hi .Lrv_slow_load_\id
    rv_tlb_index_and_page
    ldr x15, [x13, RV_TLB_ENTRY_page]
    cmp x15, x14
    b.ne .Lrv_slow_load_\id
    ldr x15, [x13, RV_TLB_ENTRY_data_minus_addr]
    add x12, x15, x12
.endm

.macro rv_tlb_write_prep bytes, id
    and x13, x12, 0xfff
    .if \bytes > 1
        tst x12, (\bytes - 1)
        b.ne .Lrv_slow_store_\id
    .endif
    cmp x13, (0x1000 - \bytes)
    b.hi .Lrv_slow_store_\id
    rv_tlb_index_and_page
    ldr x15, [x13, RV_TLB_ENTRY_page_if_writable]
    cmp x15, x14
    b.ne .Lrv_slow_store_\id
    str x14, [_tlb, RV_TLB_dirty_page]
    ldr x15, [x13, RV_TLB_ENTRY_data_minus_addr]
    add x12, x15, x12
.endm

.macro rv_load_slow funct3, id
.Lrv_slow_load_\id:
    stp x1, x2, [sp, -0x20]!
    str lr, [sp, 0x10]
    mov x0, _cpu
    mov x1, _tlb
    mov x2, x12
    mov w3, \funct3
    mov w4, w10
    bl NAME(rv_gadget_load_slow)
    ldr lr, [sp, 0x10]
    ldp x1, x2, [sp], 0x20
    cmp w0, INT_NONE
    b.ne NAME(fiber_exit)
    rv_write_pc_and_continue 4
.endm

.macro rv_store_slow funct3, id
.Lrv_slow_store_\id:
    stp x1, x2, [sp, -0x20]!
    str lr, [sp, 0x10]
    mov x0, _cpu
    mov x1, _tlb
    mov x2, x12
    mov w3, \funct3
    mov x4, x6
    bl NAME(rv_gadget_store_slow)
    ldr lr, [sp, 0x10]
    ldp x1, x2, [sp], 0x20
    cmp w0, INT_NONE
    b.ne NAME(fiber_exit)
    rv_write_pc_and_continue 4
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

.rv_gadget branch
    ldr x3, [_ip]      /* funct3 */
    ldr x4, [_ip, 8]  /* rs1 */
    ldr x5, [_ip, 16] /* rs2 */
    ldr x9, [_ip, 24] /* target pc */
    ldr x10, [_ip, 32] /* fallthrough pc */
    rv_load_x x6, x4
    rv_load_x x8, x5
    cmp x3, 0
    b.eq 1f
    cmp x3, 1
    b.eq 2f
    cmp x3, 4
    b.eq 3f
    cmp x3, 5
    b.eq 4f
    cmp x3, 6
    b.eq 5f
    cmp x3, 7
    b.eq 6f
    mov x11, x10
    b 7f
1:
    cmp x6, x8
    csel x11, x9, x10, eq
    b 7f
2:
    cmp x6, x8
    csel x11, x9, x10, ne
    b 7f
3:
    cmp x6, x8
    csel x11, x9, x10, lt
    b 7f
4:
    cmp x6, x8
    csel x11, x9, x10, ge
    b 7f
5:
    cmp x6, x8
    csel x11, x9, x10, lo
    b 7f
6:
    cmp x6, x8
    csel x11, x9, x10, hs
7:
    str x11, [_cpu, RV_CPU_pc]
    mov _tmp, INT_NONE
    b NAME(fiber_exit)

.rv_gadget jal
    ldr x3, [_ip]      /* rd */
    ldr x4, [_ip, 8]  /* return pc */
    ldr x5, [_ip, 16] /* target pc */
    rv_store_x x4, x3
    str x5, [_cpu, RV_CPU_pc]
    mov _tmp, INT_NONE
    b NAME(fiber_exit)

.rv_gadget jalr
    ldr x3, [_ip]      /* rd */
    ldr x4, [_ip, 8]  /* rs1 */
    ldr x5, [_ip, 16] /* imm */
    ldr x6, [_ip, 24] /* return pc */
    rv_load_x x8, x4
    add x8, x8, x5
    bic x8, x8, 1
    rv_store_x x6, x3
    str x8, [_cpu, RV_CPU_pc]
    mov _tmp, INT_NONE
    b NAME(fiber_exit)

.rv_gadget lb
    ldr x10, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x4
    rv_tlb_read_prep 1, lb
    ldrsb x6, [x12]
    rv_store_x x6, x10
    rv_write_pc_and_continue 4
    rv_load_slow 0, lb

.rv_gadget lh
    ldr x10, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x4
    rv_tlb_read_prep 2, lh
    ldrsh x6, [x12]
    rv_store_x x6, x10
    rv_write_pc_and_continue 4
    rv_load_slow 1, lh

.rv_gadget lw
    ldr x10, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x4
    rv_tlb_read_prep 4, lw
    ldrsw x6, [x12]
    rv_store_x x6, x10
    rv_write_pc_and_continue 4
    rv_load_slow 2, lw

.rv_gadget ld
    ldr x10, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x4
    rv_tlb_read_prep 8, ld
    ldr x6, [x12]
    rv_store_x x6, x10
    rv_write_pc_and_continue 4
    rv_load_slow 3, ld

.rv_gadget lbu
    ldr x10, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x4
    rv_tlb_read_prep 1, lbu
    ldrb w6, [x12]
    rv_store_x x6, x10
    rv_write_pc_and_continue 4
    rv_load_slow 4, lbu

.rv_gadget lhu
    ldr x10, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x4
    rv_tlb_read_prep 2, lhu
    ldrh w6, [x12]
    rv_store_x x6, x10
    rv_write_pc_and_continue 4
    rv_load_slow 5, lhu

.rv_gadget lwu
    ldr x10, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x4
    rv_tlb_read_prep 4, lwu
    ldr w6, [x12]
    rv_store_x x6, x10
    rv_write_pc_and_continue 4
    rv_load_slow 6, lwu

.rv_gadget sb
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x5
    rv_load_x x6, x4
    rv_tlb_write_prep 1, sb
    strb w6, [x12]
    rv_write_pc_and_continue 4
    rv_store_slow 0, sb

.rv_gadget sh
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x5
    rv_load_x x6, x4
    rv_tlb_write_prep 2, sh
    strh w6, [x12]
    rv_write_pc_and_continue 4
    rv_store_slow 1, sh

.rv_gadget sw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x5
    rv_load_x x6, x4
    rv_tlb_write_prep 4, sw
    str w6, [x12]
    rv_write_pc_and_continue 4
    rv_store_slow 2, sw

.rv_gadget sd
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x12, x3
    add x12, x12, x5
    rv_load_x x6, x4
    rv_tlb_write_prep 8, sd
    str x6, [x12]
    rv_write_pc_and_continue 4
    rv_store_slow 3, sd

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

.rv_gadget andi
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    and x6, x6, x5
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget ori
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    orr x6, x6, x5
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget xori
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    eor x6, x6, x5
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget slti
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    cmp x6, x5
    cset x6, lt
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sltiu
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    cmp x6, x5
    cset x6, lo
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget slli
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    lsl x6, x6, x5
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget srli
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    lsr x6, x6, x5
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget srai
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    asr x6, x6, x5
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget addiw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    add w6, w6, w5
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget slliw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    lsl w6, w6, w5
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget srliw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    lsr w6, w6, w5
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sraiw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    asr w6, w6, w5
    sxtw x6, w6
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

.rv_gadget mul
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    mul x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget div
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cbz x8, 1f
    mov x9, 1
    lsl x9, x9, 63
    cmp x6, x9
    b.ne 2f
    cmn x8, 1
    b.eq 3f
2:
    sdiv x6, x6, x8
    b 3f
1:
    mov x6, -1
3:
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget divu
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cbz x8, 1f
    udiv x6, x6, x8
    b 2f
1:
    mov x6, -1
2:
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget rem
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    mov x9, x6
    cbz x8, 3f
    mov x10, 1
    lsl x10, x10, 63
    cmp x6, x10
    b.ne 1f
    cmn x8, 1
    b.eq 2f
1:
    sdiv x10, x6, x8
    msub x6, x10, x8, x6
    b 3f
2:
    mov x6, xzr
    b 3f
3:
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget remu
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cbz x8, 1f
    udiv x9, x6, x8
    msub x6, x9, x8, x6
1:
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget and
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    and x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget or
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    orr x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget xor
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    eor x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget slt
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cmp x6, x8
    cset x6, lt
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sltu
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cmp x6, x8
    cset x6, lo
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sll
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    lsl x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget srl
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    lsr x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sra
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    asr x6, x6, x8
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget addw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    add w6, w6, w8
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget mulw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    mul w6, w6, w8
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget divw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cbz w8, 1f
    mov w9, 0x80000000
    cmp w6, w9
    b.ne 2f
    cmn w8, 1
    b.eq 3f
2:
    sdiv w6, w6, w8
    b 4f
1:
    mov x6, -1
    b 5f
3:
    mov w6, w9
4:
    sxtw x6, w6
5:
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget divuw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cbz w8, 1f
    udiv w6, w6, w8
    sxtw x6, w6
    b 2f
1:
    mov x6, -1
2:
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget remw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cbz w8, 3f
    mov w9, 0x80000000
    cmp w6, w9
    b.ne 1f
    cmn w8, 1
    b.eq 2f
1:
    sdiv w9, w6, w8
    msub w6, w9, w8, w6
    b 3f
2:
    mov w6, wzr
3:
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget remuw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    cbz w8, 1f
    udiv w9, w6, w8
    msub w6, w9, w8, w6
1:
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget subw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    sub w6, w6, w8
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sllw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    lsl w6, w6, w8
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget srlw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    lsr w6, w6, w8
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

.rv_gadget sraw
    ldr x3, [_ip]
    ldr x4, [_ip, 8]
    ldr x5, [_ip, 16]
    rv_load_x x6, x4
    rv_load_x x8, x5
    asr w6, w6, w8
    sxtw x6, w6
    rv_store_x x6, x3
    rv_write_pc_and_continue 4

# vim: ft=gas
