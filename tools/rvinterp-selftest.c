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

static void put_double(struct flat_mmu *flat, addr_t addr, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    for (unsigned i = 0; i < 8; i++)
        flat->mem[addr + i] = (bits >> (i * 8)) & 0xff;
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
    put32(&flat, pc + 28, rv_encode_i(-1, 0, 0, 13, 0x13));    // addi a3,zero,-1
    put32(&flat, pc + 32, rv_encode_i(2, 0, 0, 14, 0x13));     // addi a4,zero,2
    put32(&flat, pc + 36, rv_encode_r(0x01, 14, 13, 1, 15, 0x33)); // mulh a5,a3,a4
    put32(&flat, pc + 40, rv_encode_r(0x01, 14, 13, 2, 16, 0x33)); // mulhsu a6,a3,a4
    put32(&flat, pc + 44, rv_encode_r(0x01, 14, 13, 3, 17, 0x33)); // mulhu a7,a3,a4
    put32(&flat, pc + 48, rv_encode_i(0, 3, 3, 0, 0x07));      // fld ft0,0(gp)
    put32(&flat, pc + 52, rv_encode_i(8, 3, 3, 1, 0x07));      // fld ft1,8(gp)
    put32(&flat, pc + 56, rv_encode_r(0x01, 1, 0, 0, 2, 0x53)); // fadd.d ft2,ft0,ft1
    put32(&flat, pc + 60, rv_encode_s(16, 2, 3, 3, 0x27));     // fsd ft2,16(gp)
    put32(&flat, pc + 64, 0x0000000f);                         // fence
    put32(&flat, pc + 68, 0x0000100f);                         // fence.i
    put32(&flat, pc + 72, rv_encode_i(0x003, 7, 5, 28, 0x73)); // csrrwi t3,fcsr,7
    put32(&flat, pc + 76, rv_encode_i(0x003, 0, 2, 29, 0x73)); // csrrs t4,fcsr,zero
    put32(&flat, pc + 80, rv_encode_i(0, 0, 0, 0, 0x73));      // ecall
    put_double(&flat, 0x380, 1.25);
    put_double(&flat, 0x388, 2.5);

    struct cpu_state cpu = {
        .mmu = &flat.mmu,
        .pc = pc,
        .sp = 0x300,
        .gp = 0x380,
    };
    int interrupt = cpu_run_to_interrupt(&cpu, &tlb);
    assert(interrupt == INT_SYSCALL);
    assert(cpu.a0 == 15);
    assert(cpu.a1 == 0);
    assert(cpu.a2 == 15);
    assert(cpu.a5 == UINT64_MAX);
    assert(cpu.a6 == UINT64_MAX);
    assert(cpu.a7 == 1);
    assert(cpu.x[28] == 0);
    assert(cpu.x[29] == 7);
    assert(cpu.fcsr == 7);
    assert(cpu.pc == pc + 84);
    assert(get64(&flat, 0x300) == 15);
    assert(get_double(&flat, 0x390) == 3.75);

    cpu_poke(&cpu);
    asbestos_free(flat.mmu.asbestos);
    puts("rvinterp selftest passed");
    return 0;
}
