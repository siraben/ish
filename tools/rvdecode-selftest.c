#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "emu_riscv/decode.h"

static void decode32(uint32_t raw, struct rv_insn *insn) {
    uint8_t bytes[4] = {
        raw & 0xff,
        (raw >> 8) & 0xff,
        (raw >> 16) & 0xff,
        (raw >> 24) & 0xff,
    };
    assert(rv_decode(bytes, sizeof(bytes), insn));
}

static void decode16(uint16_t raw, struct rv_insn *insn) {
    uint8_t bytes[2] = {
        raw & 0xff,
        (raw >> 8) & 0xff,
    };
    assert(rv_decode(bytes, sizeof(bytes), insn));
}

static void expect32(uint32_t raw, const char *mnemonic, const char *operands) {
    struct rv_insn insn;
    decode32(raw, &insn);
    assert(!insn.compressed);
    assert(insn.length == 4);
    assert(strcmp(insn.mnemonic, mnemonic) == 0);
    assert(strcmp(insn.operands, operands) == 0);
}

static void expect16(uint16_t raw, const char *mnemonic, const char *operands, uint32_t expanded) {
    struct rv_insn insn;
    decode16(raw, &insn);
    assert(insn.compressed);
    assert(insn.length == 2);
    assert(insn.expanded == expanded);
    assert(strcmp(insn.mnemonic, mnemonic) == 0);
    assert(strcmp(insn.operands, operands) == 0);
}

int main(void) {
    expect32(0x00100513, "addi", "a0,zero,1");
    expect32(0x00b50633, "add", "a2,a0,a1");
    expect32(0x00000073, "ecall", "");
    expect32(0x00c5b023, "sd", "a2,0(a1)");
    expect32(0x0005b503, "ld", "a0,0(a1)");
    expect32(0x02b50533, "mul", "a0,a0,a1");
    expect32(0x1005352f, "lr.d", "a0,(a0)");
    expect32(0x00302573, "csrrs", "a0,fcsr,zero");
    expect32(rv_encode_r(0x01, 1, 0, 0, 2, 0x53), "fadd.d", "ft2,ft0,ft1");
    expect32(rv_encode_r(0x00, 1, 0, 0, 2, 0x53), "fadd.s", "ft2,ft0,ft1");

    expect16(0x4505, "addi", "a0,zero,1", 0x00100513);
    expect16(0x060d, "addi", "a2,a2,3", 0x00360613);
    expect16(0x8082, "jalr", "zero,0(ra)", 0x00008067);

    uint8_t illegal[2] = {0, 0};
    struct rv_insn insn;
    assert(!rv_decode(illegal, sizeof(illegal), &insn));
    assert(insn.illegal);

    puts("rvdecode selftest passed");
    return 0;
}
