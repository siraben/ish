#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_riscv/cpu.h"
#include "emu_riscv/decode.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"
#include "asbestos/asbestos.h"

int current_pid(void) {
    return 0;
}

_Noreturn void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}

struct flat_mmu {
    struct mmu mmu;
    uint8_t mem[4096];
};

static void *flat_translate(struct mmu *mmu, addr_t addr, int type) {
    (void) type;
    struct flat_mmu *flat = (struct flat_mmu *) mmu;
    if (addr >= sizeof(flat->mem))
        return NULL;
    return &flat->mem[addr];
}

static void put32(struct flat_mmu *flat, addr_t addr, uint32_t value) {
    flat->mem[addr + 0] = value & 0xff;
    flat->mem[addr + 1] = (value >> 8) & 0xff;
    flat->mem[addr + 2] = (value >> 16) & 0xff;
    flat->mem[addr + 3] = (value >> 24) & 0xff;
}

static uint64_t get64(struct flat_mmu *flat, addr_t addr) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value |= (uint64_t) flat->mem[addr + i] << (i * 8);
    return value;
}

static void put64(struct flat_mmu *flat, addr_t addr, uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        flat->mem[addr + i] = (value >> (i * 8)) & 0xff;
}

static void put_double(struct flat_mmu *flat, addr_t addr, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put64(flat, addr, bits);
}

static double get_double(struct flat_mmu *flat, addr_t addr) {
    uint64_t bits = get64(flat, addr);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

int main(void) {
    static struct mmu_ops ops = {
        .translate = flat_translate,
    };
    struct flat_mmu flat = {
        .mmu = {
            .ops = &ops,
            .changes = 1,
        },
    };
    flat.mmu.asbestos = asbestos_new(&flat.mmu);
    struct tlb tlb = {};
    tlb_refresh(&tlb, &flat.mmu);

    addr_t pc = 0x100;
    put32(&flat, pc + 0, rv_encode_i(0, 0, 0, 10, 0x13));      // addi a0,zero,0
    put32(&flat, pc + 4, rv_encode_i(5, 0, 0, 11, 0x13));      // addi a1,zero,5
    put32(&flat, pc + 8, rv_encode_r(0, 11, 10, 0, 10, 0x33)); // add a0,a0,a1
    put32(&flat, pc + 12, rv_encode_i(-1, 11, 0, 11, 0x13));   // addi a1,a1,-1
    put32(&flat, pc + 16, rv_encode_b(-8, 0, 11, 1, 0x63));    // bne a1,zero,loop
    put32(&flat, pc + 20, rv_encode_s(0, 10, 2, 3, 0x23));     // sd a0,0(sp)
    put32(&flat, pc + 24, rv_encode_i(0, 2, 3, 12, 0x03));     // ld a2,0(sp)
    put32(&flat, pc + 28, rv_encode_i(5, 0, 0, 11, 0x13));     // addi a1,zero,5
    put32(&flat, pc + 32, rv_encode_r(0x00, 11, 2, 3, 30, 0x2f)); // amoadd.d t5,a1,(sp)
    put32(&flat, pc + 36, rv_encode_i(-1, 0, 0, 13, 0x13));    // addi a3,zero,-1
    put32(&flat, pc + 40, rv_encode_i(2, 0, 0, 14, 0x13));     // addi a4,zero,2
    put32(&flat, pc + 44, rv_encode_r(0x01, 14, 13, 1, 15, 0x33)); // mulh a5,a3,a4
    put32(&flat, pc + 48, rv_encode_r(0x01, 14, 13, 2, 16, 0x33)); // mulhsu a6,a3,a4
    put32(&flat, pc + 52, rv_encode_r(0x01, 14, 13, 3, 17, 0x33)); // mulhu a7,a3,a4
    put32(&flat, pc + 56, rv_encode_i(0, 3, 3, 0, 0x07));      // fld ft0,0(gp)
    put32(&flat, pc + 60, rv_encode_i(8, 3, 3, 1, 0x07));      // fld ft1,8(gp)
    put32(&flat, pc + 64, rv_encode_r(0x01, 1, 0, 0, 2, 0x53)); // fadd.d ft2,ft0,ft1
    put32(&flat, pc + 68, rv_encode_s(16, 2, 3, 3, 0x27));     // fsd ft2,16(gp)
    put32(&flat, pc + 72, rv_encode_r(0x20, 1, 2, 0, 3, 0x53)); // fcvt.s.d ft3,ft2
    put32(&flat, pc + 76, rv_encode_r(0x21, 0, 3, 0, 4, 0x53)); // fcvt.d.s ft4,ft3
    put32(&flat, pc + 80, rv_encode_s(24, 4, 3, 3, 0x27));     // fsd ft4,24(gp)
    put32(&flat, pc + 84, 0x0000000f);                         // fence
    put32(&flat, pc + 88, 0x0000100f);                         // fence.i
    put32(&flat, pc + 92, rv_encode_i(0x003, 7, 5, 28, 0x73)); // csrrwi t3,fcsr,7
    put32(&flat, pc + 96, rv_encode_i(0x003, 0, 2, 29, 0x73)); // csrrs t4,fcsr,zero
    put32(&flat, pc + 100, rv_encode_r(0x71, 0, 0, 1, 18, 0x53)); // fclass.d s2,ft0
    put32(&flat, pc + 104, rv_encode_i(32, 3, 3, 5, 0x07));     // fld ft5,32(gp)
    put32(&flat, pc + 108, rv_encode_r(0x71, 0, 5, 1, 19, 0x53)); // fclass.d s3,ft5
    put32(&flat, pc + 112, rv_encode_i(40, 3, 3, 6, 0x07));     // fld ft6,40(gp)
    put32(&flat, pc + 116, rv_encode_r(0x71, 0, 6, 1, 20, 0x53)); // fclass.d s4,ft6
    put32(&flat, pc + 120, rv_encode_i(48, 3, 2, 7, 0x07));     // flw ft7,48(gp)
    put32(&flat, pc + 124, rv_encode_r(0x70, 0, 7, 1, 21, 0x53)); // fclass.s s5,ft7
    put32(&flat, pc + 128, rv_encode_r(0x70, 0, 8, 1, 22, 0x53)); // fclass.s s6,fs0
    put32(&flat, pc + 132, rv_encode_i(0, 0, 0, 0, 0x73));      // ecall
    put_double(&flat, 0x380, 1.25);
    put_double(&flat, 0x388, 2.5);
    put64(&flat, 0x3a0, UINT64_C(0x7ff0000000000000));
    put64(&flat, 0x3a8, UINT64_C(0x7ff8000000000000));
    put32(&flat, 0x3b0, 0x80000000u);

    struct cpu_state cpu = {
        .mmu = &flat.mmu,
        .pc = pc,
        .sp = 0x300,
        .gp = 0x380,
        .f[8] = 0x7f800000u,
    };
    int interrupt = cpu_run_to_interrupt(&cpu, &tlb);
    if (interrupt != INT_SYSCALL) {
        struct rv_insn insn = {};
        rv_decode(&flat.mem[cpu.pc], sizeof(flat.mem) - cpu.pc, &insn);
        fprintf(stderr, "unexpected interrupt %d at pc %#llx: %s %s raw=%#x expanded=%#x funct3=%u funct7=%u\n",
                interrupt, (unsigned long long) cpu.pc, insn.mnemonic, insn.operands,
                insn.raw, insn.expanded, insn.funct3, insn.funct7);
    }
    assert(interrupt == INT_SYSCALL);
    assert(cpu.a0 == 15);
    assert(cpu.a1 == 5);
    assert(cpu.a2 == 15);
    assert(cpu.a5 == UINT64_MAX);
    assert(cpu.a6 == UINT64_MAX);
    assert(cpu.a7 == 1);
    assert(cpu.x[28] == 0);
    assert(cpu.x[29] == 7);
    assert(cpu.x[30] == 15);
    assert(cpu.s2 == (1u << 6));
    assert(cpu.s3 == (1u << 7));
    assert(cpu.s4 == (1u << 9));
    assert(cpu.s5 == (1u << 3));
    assert(cpu.s6 == (1u << 9));
    assert(cpu.fcsr == 7);
    assert(cpu.pc == pc + 136);
    assert(get64(&flat, 0x300) == 20);
    assert(get_double(&flat, 0x390) == 3.75);
    assert(get_double(&flat, 0x398) == 3.75);

    cpu_poke(&cpu);
    asbestos_free(flat.mmu.asbestos);
    puts("rvinterp selftest passed");
    return 0;
}
