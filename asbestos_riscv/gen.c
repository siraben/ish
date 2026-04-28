#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "asbestos_riscv/gen.h"
#include "debug.h"
#include "emu_riscv/decode.h"
#include "emu_riscv/cpu.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"

static uint64_t rv_gadget_sign_extend(uint64_t value, unsigned bits) {
    unsigned shift = 64 - bits;
    return (uint64_t) ((int64_t) (value << shift) >> shift);
}

int rv_gadget_load_slow(struct cpu_state *cpu, struct tlb *tlb, addr_t addr, unsigned funct3, unsigned rd) {
    uint64_t value = 0;
    unsigned size;
    switch (funct3) {
    case 0: size = 1; break;
    case 1: size = 2; break;
    case 2: size = 4; break;
    case 3: size = 8; break;
    case 4: size = 1; break;
    case 5: size = 2; break;
    case 6: size = 4; break;
    default: return INT_UNDEFINED;
    }
    if (!tlb_read(tlb, addr, &value, size)) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = false;
        return INT_GPF;
    }
    if (funct3 <= 3)
        value = rv_gadget_sign_extend(value, size * CHAR_BIT);
    if (rd != 0)
        cpu->x[rd] = value;
    return INT_NONE;
}

int rv_gadget_store_slow(struct cpu_state *cpu, struct tlb *tlb, addr_t addr, unsigned funct3, uint64_t value) {
    unsigned size;
    switch (funct3) {
    case 0: size = 1; break;
    case 1: size = 2; break;
    case 2: size = 4; break;
    case 3: size = 8; break;
    default: return INT_UNDEFINED;
    }
    if (!tlb_write(tlb, addr, &value, size)) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_GPF;
    }
    return INT_NONE;
}

int rv_gadget_amo(struct cpu_state *cpu, struct tlb *tlb, unsigned funct5,
        unsigned width, unsigned rd, unsigned rs1, unsigned rs2) {
    if (width != 4 && width != 8)
        return INT_UNDEFINED;

    addr_t addr = rs1 == 0 ? 0 : cpu->x[rs1];
    if (PGOFFSET(addr) > PAGE_SIZE - width)
        return INT_UNDEFINED;

    uint64_t loaded = 0;
    if (!tlb_read(tlb, addr, &loaded, width)) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_GPF;
    }
    if (width == 4)
        loaded = rv_gadget_sign_extend((uint32_t) loaded, 32);

    if (funct5 == 0x02) {
        cpu->reservation_addr = addr;
        cpu->reservation_valid = true;
        if (rd != 0)
            cpu->x[rd] = loaded;
        return INT_NONE;
    }

    uint64_t rs2_value = rs2 == 0 ? 0 : cpu->x[rs2];
    uint64_t result = rs2_value;
    bool should_store = true;
    if (funct5 == 0x03) {
        should_store = cpu->reservation_valid && cpu->reservation_addr == addr;
        cpu->reservation_valid = false;
        if (rd != 0)
            cpu->x[rd] = should_store ? 0 : 1;
    } else {
        if (rd != 0)
            cpu->x[rd] = loaded;
        switch (funct5) {
        case 0x00: result = loaded + rs2_value; break;
        case 0x01: result = rs2_value; break;
        case 0x04: result = loaded ^ rs2_value; break;
        case 0x08: result = loaded | rs2_value; break;
        case 0x0c: result = loaded & rs2_value; break;
        case 0x10: result = (int64_t) loaded < (int64_t) rs2_value ? loaded : rs2_value; break;
        case 0x14: result = (int64_t) loaded > (int64_t) rs2_value ? loaded : rs2_value; break;
        case 0x18: result = loaded < rs2_value ? loaded : rs2_value; break;
        case 0x1c: result = loaded > rs2_value ? loaded : rs2_value; break;
        default: return INT_UNDEFINED;
        }
    }

    if (should_store) {
        cpu->reservation_valid = false;
        bool ok;
        if (width == 4) {
            uint32_t narrow = result;
            ok = tlb_write(tlb, addr, &narrow, width);
        } else {
            ok = tlb_write(tlb, addr, &result, width);
        }
        if (!ok) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = true;
            return INT_GPF;
        }
    }
    return INT_NONE;
}

int rv_gadget_fload_slow(struct cpu_state *cpu, struct tlb *tlb, addr_t addr, unsigned funct3, unsigned rd) {
    if (funct3 == 2) {
        uint32_t value;
        if (!tlb_read(tlb, addr, &value, sizeof(value))) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return INT_GPF;
        }
        cpu->f[rd] = UINT64_C(0xffffffff00000000) | value;
        return INT_NONE;
    }
    if (funct3 == 3) {
        if (!tlb_read(tlb, addr, &cpu->f[rd], sizeof(uint64_t))) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return INT_GPF;
        }
        return INT_NONE;
    }
    return INT_UNDEFINED;
}

int rv_gadget_fstore_slow(struct cpu_state *cpu, struct tlb *tlb, addr_t addr, unsigned funct3, uint64_t value) {
    if (funct3 == 2) {
        uint32_t narrow = value;
        if (!tlb_write(tlb, addr, &narrow, sizeof(narrow))) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = true;
            return INT_GPF;
        }
        return INT_NONE;
    }
    if (funct3 == 3) {
        if (!tlb_write(tlb, addr, &value, sizeof(value))) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = true;
            return INT_GPF;
        }
        return INT_NONE;
    }
    return INT_UNDEFINED;
}

static bool rv_gadget_read_csr(struct cpu_state *cpu, unsigned csr, uint64_t *value) {
    switch (csr) {
    case 0x001:
        *value = cpu->fcsr & 0x1f;
        return true;
    case 0x002:
        *value = (cpu->fcsr >> 5) & 0x7;
        return true;
    case 0x003:
        *value = cpu->fcsr & 0xff;
        return true;
    case 0xc00:
    case 0xc02:
        *value = (uint64_t) cpu->cycle;
        return true;
    case 0xc01:
        *value = rdtsc();
        return true;
    default:
        return false;
    }
}

static bool rv_gadget_write_csr(struct cpu_state *cpu, unsigned csr, uint64_t value) {
    switch (csr) {
    case 0x001:
        cpu->fcsr = (cpu->fcsr & ~0x1fu) | (value & 0x1f);
        return true;
    case 0x002:
        cpu->fcsr = (cpu->fcsr & ~(0x7u << 5)) | ((value & 0x7) << 5);
        return true;
    case 0x003:
        cpu->fcsr = value & 0xff;
        return true;
    default:
        return false;
    }
}

int rv_gadget_csr(struct cpu_state *cpu, unsigned funct3, unsigned rd, unsigned rs1, unsigned csr) {
    uint64_t old;
    uint64_t csr_arg = funct3 >= 5 ? rs1 : (rs1 == 0 ? 0 : cpu->x[rs1]);
    if (!rv_gadget_read_csr(cpu, csr, &old))
        return INT_UNDEFINED;
    if (rd != 0)
        cpu->x[rd] = old;
    switch (funct3) {
    case 1: case 5:
        if (!rv_gadget_write_csr(cpu, csr, csr_arg))
            return INT_UNDEFINED;
        break;
    case 2: case 6:
        if (csr_arg != 0 && !rv_gadget_write_csr(cpu, csr, old | csr_arg))
            return INT_UNDEFINED;
        break;
    case 3: case 7:
        if (csr_arg != 0 && !rv_gadget_write_csr(cpu, csr, old & ~csr_arg))
            return INT_UNDEFINED;
        break;
    default:
        return INT_UNDEFINED;
    }
    return INT_NONE;
}

static float rv_gadget_freg_s(struct cpu_state *cpu, unsigned reg) {
    uint32_t bits = cpu->f[reg];
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double rv_gadget_freg_d(struct cpu_state *cpu, unsigned reg) {
    double value;
    memcpy(&value, &cpu->f[reg], sizeof(value));
    return value;
}

static void rv_gadget_store_freg_s(struct cpu_state *cpu, unsigned reg, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    cpu->f[reg] = UINT64_C(0xffffffff00000000) | bits;
}

static void rv_gadget_store_freg_d(struct cpu_state *cpu, unsigned reg, double value) {
    memcpy(&cpu->f[reg], &value, sizeof(value));
}

static uint64_t rv_gadget_fp_to_int_s(float value, unsigned kind) {
    switch (kind) {
    case 0: return (uint64_t) (int64_t) (int32_t) value;
    case 1: return (uint64_t) (uint32_t) value;
    case 2: return (uint64_t) (int64_t) value;
    case 3: return (uint64_t) value;
    default: return 0;
    }
}

static uint64_t rv_gadget_fp_to_int_d(double value, unsigned kind) {
    switch (kind) {
    case 0: return (uint64_t) (int64_t) (int32_t) value;
    case 1: return (uint64_t) (uint32_t) value;
    case 2: return (uint64_t) (int64_t) value;
    case 3: return (uint64_t) value;
    default: return 0;
    }
}

static float rv_gadget_int_to_fp_s(uint64_t value, unsigned kind) {
    switch (kind) {
    case 0: return (float) (int32_t) value;
    case 1: return (float) (uint32_t) value;
    case 2: return (float) (int64_t) value;
    case 3: return (float) value;
    default: return 0.0f;
    }
}

static double rv_gadget_int_to_fp_d(uint64_t value, unsigned kind) {
    switch (kind) {
    case 0: return (double) (int32_t) value;
    case 1: return (double) (uint32_t) value;
    case 2: return (double) (int64_t) value;
    case 3: return (double) value;
    default: return 0.0;
    }
}

int rv_gadget_fp(struct cpu_state *cpu, const unsigned long *params) {
    enum rv_decode_op op = (enum rv_decode_op) params[0];
    unsigned funct5 = params[1];
    unsigned fmt = params[2];
    unsigned funct3 = params[3];
    unsigned rd = params[4];
    unsigned rs1 = params[5];
    unsigned rs2 = params[6];
    unsigned rs3 = params[7];
    uint64_t xrs1 = rs1 == 0 ? 0 : cpu->x[rs1];

    if (op == RV_OP_OP_FP) {
        if (fmt == 0) {
            float a = rv_gadget_freg_s(cpu, rs1);
            float b = rv_gadget_freg_s(cpu, rs2);
            switch (funct5) {
            case 0x00: rv_gadget_store_freg_s(cpu, rd, a + b); break;
            case 0x01: rv_gadget_store_freg_s(cpu, rd, a - b); break;
            case 0x02: rv_gadget_store_freg_s(cpu, rd, a * b); break;
            case 0x03: rv_gadget_store_freg_s(cpu, rd, a / b); break;
            case 0x0b: rv_gadget_store_freg_s(cpu, rd, sqrtf(a)); break;
            case 0x04: {
                uint32_t ia = cpu->f[rs1];
                uint32_t ib = cpu->f[rs2];
                uint32_t sign = ib & 0x80000000u;
                if (funct3 == 1)
                    sign ^= 0x80000000u;
                if (funct3 == 2)
                    sign ^= ia & 0x80000000u;
                cpu->f[rd] = UINT64_C(0xffffffff00000000) | ((ia & 0x7fffffffu) | sign);
                break;
            }
            case 0x05:
                if (funct3 == 0)
                    rv_gadget_store_freg_s(cpu, rd, fminf(a, b));
                else if (funct3 == 1)
                    rv_gadget_store_freg_s(cpu, rd, fmaxf(a, b));
                else
                    return INT_UNDEFINED;
                break;
            case 0x08:
                if (rs2 != 1)
                    return INT_UNDEFINED;
                rv_gadget_store_freg_s(cpu, rd, (float) rv_gadget_freg_d(cpu, rs1));
                break;
            case 0x14:
                if (rd != 0) {
                    if (funct3 == 0)
                        cpu->x[rd] = a <= b;
                    else if (funct3 == 1)
                        cpu->x[rd] = a < b;
                    else if (funct3 == 2)
                        cpu->x[rd] = a == b;
                    else
                        return INT_UNDEFINED;
                }
                break;
            case 0x18:
                if (rs2 > 3)
                    return INT_UNDEFINED;
                if (rd != 0)
                    cpu->x[rd] = rv_gadget_fp_to_int_s(a, rs2);
                break;
            case 0x1a:
                if (rs2 > 3)
                    return INT_UNDEFINED;
                rv_gadget_store_freg_s(cpu, rd, rv_gadget_int_to_fp_s(xrs1, rs2));
                break;
            case 0x1c:
                if (funct3 != 0)
                    return INT_UNDEFINED;
                if (rd != 0)
                    cpu->x[rd] = (int32_t) (uint32_t) cpu->f[rs1];
                break;
            case 0x1e:
                if (funct3 != 0)
                    return INT_UNDEFINED;
                cpu->f[rd] = UINT64_C(0xffffffff00000000) | (uint32_t) xrs1;
                break;
            default:
                return INT_UNDEFINED;
            }
            return INT_NONE;
        }
        if (fmt == 1) {
            double a = rv_gadget_freg_d(cpu, rs1);
            double b = rv_gadget_freg_d(cpu, rs2);
            switch (funct5) {
            case 0x00: rv_gadget_store_freg_d(cpu, rd, a + b); break;
            case 0x01: rv_gadget_store_freg_d(cpu, rd, a - b); break;
            case 0x02: rv_gadget_store_freg_d(cpu, rd, a * b); break;
            case 0x03: rv_gadget_store_freg_d(cpu, rd, a / b); break;
            case 0x0b: rv_gadget_store_freg_d(cpu, rd, sqrt(a)); break;
            case 0x04: {
                uint64_t ia = cpu->f[rs1];
                uint64_t ib = cpu->f[rs2];
                uint64_t sign = ib & UINT64_C(0x8000000000000000);
                if (funct3 == 1)
                    sign ^= UINT64_C(0x8000000000000000);
                if (funct3 == 2)
                    sign ^= ia & UINT64_C(0x8000000000000000);
                cpu->f[rd] = (ia & UINT64_C(0x7fffffffffffffff)) | sign;
                break;
            }
            case 0x05:
                if (funct3 == 0)
                    rv_gadget_store_freg_d(cpu, rd, fmin(a, b));
                else if (funct3 == 1)
                    rv_gadget_store_freg_d(cpu, rd, fmax(a, b));
                else
                    return INT_UNDEFINED;
                break;
            case 0x08:
                if (rs2 != 0)
                    return INT_UNDEFINED;
                rv_gadget_store_freg_d(cpu, rd, (double) rv_gadget_freg_s(cpu, rs1));
                break;
            case 0x14:
                if (rd != 0) {
                    if (funct3 == 0)
                        cpu->x[rd] = a <= b;
                    else if (funct3 == 1)
                        cpu->x[rd] = a < b;
                    else if (funct3 == 2)
                        cpu->x[rd] = a == b;
                    else
                        return INT_UNDEFINED;
                }
                break;
            case 0x18:
            case 0x19:
                if (rs2 > 3)
                    return INT_UNDEFINED;
                if (rd != 0)
                    cpu->x[rd] = rv_gadget_fp_to_int_d(a, rs2);
                break;
            case 0x1a:
            case 0x1b:
                if (rs2 > 3)
                    return INT_UNDEFINED;
                rv_gadget_store_freg_d(cpu, rd, rv_gadget_int_to_fp_d(xrs1, rs2));
                break;
            case 0x1c:
                if (funct3 != 0)
                    return INT_UNDEFINED;
                if (rd != 0)
                    cpu->x[rd] = cpu->f[rs1];
                break;
            case 0x1e:
                if (funct3 != 0)
                    return INT_UNDEFINED;
                cpu->f[rd] = xrs1;
                break;
            default:
                return INT_UNDEFINED;
            }
            return INT_NONE;
        }
        return INT_UNDEFINED;
    }

    if (op == RV_OP_MADD || op == RV_OP_MSUB || op == RV_OP_NMSUB || op == RV_OP_NMADD) {
        if (fmt == 0) {
            float a = rv_gadget_freg_s(cpu, rs1);
            float b = rv_gadget_freg_s(cpu, rs2);
            float c = rv_gadget_freg_s(cpu, rs3);
            if (op == RV_OP_MADD)
                rv_gadget_store_freg_s(cpu, rd, a * b + c);
            else if (op == RV_OP_MSUB)
                rv_gadget_store_freg_s(cpu, rd, a * b - c);
            else if (op == RV_OP_NMSUB)
                rv_gadget_store_freg_s(cpu, rd, -(a * b) + c);
            else
                rv_gadget_store_freg_s(cpu, rd, -(a * b) - c);
            return INT_NONE;
        }
        if (fmt == 1) {
            double a = rv_gadget_freg_d(cpu, rs1);
            double b = rv_gadget_freg_d(cpu, rs2);
            double c = rv_gadget_freg_d(cpu, rs3);
            if (op == RV_OP_MADD)
                rv_gadget_store_freg_d(cpu, rd, a * b + c);
            else if (op == RV_OP_MSUB)
                rv_gadget_store_freg_d(cpu, rd, a * b - c);
            else if (op == RV_OP_NMSUB)
                rv_gadget_store_freg_d(cpu, rd, -(a * b) + c);
            else
                rv_gadget_store_freg_d(cpu, rd, -(a * b) - c);
            return INT_NONE;
        }
    }
    return INT_UNDEFINED;
}

enum rv_block_marker {
    RV_BLOCK_INSN = 0x52564900u,
    RV_BLOCK_EXIT = 0x525649ffu,
};

static void gen(struct gen_state *state, unsigned long thing) {
    assert(state->size <= state->capacity);
    if (state->size >= state->capacity) {
        state->capacity *= 2;
        struct fiber_block *bigger = realloc(state->block,
                sizeof(struct fiber_block) + state->capacity * sizeof(unsigned long));
        if (bigger == NULL)
            die("out of memory while generating rv64 block");
        state->block = bigger;
    }
    state->block->code[state->size++] = thing;
}

void gen_start(addr_t addr, struct gen_state *state) {
    state->capacity = FIBER_BLOCK_INITIAL_CAPACITY;
    state->size = 0;
    state->ip = addr;
    state->orig_ip = addr;
    state->orig_ip_extra = 0;
    state->jump_ip[0] = 0;
    state->jump_ip[1] = 0;
    state->block_patch_ip = 0;

    state->block = malloc(sizeof(struct fiber_block) + state->capacity * sizeof(unsigned long));
    if (state->block == NULL)
        die("out of memory while starting rv64 block");
    state->block->addr = addr;
}

void gen_exit(struct gen_state *state) {
    extern void gadget_rv_exit(void);
    gen(state, (unsigned long) gadget_rv_exit);
}

void gen_end(struct gen_state *state) {
    struct fiber_block *block = state->block;
    for (int i = 0; i <= 1; i++) {
    if (state->jump_ip[i] != 0) {
            block->jump_ip[i] = &block->code[state->jump_ip[i]];
            block->old_jump_ip[i] = *block->jump_ip[i];
        } else {
            block->jump_ip[i] = NULL;
            block->old_jump_ip[i] = 0;
        }
    }
    list_init(&block->jumps_from[0]);
    list_init(&block->jumps_from[1]);
    list_init(&block->jumps_from_links[0]);
    list_init(&block->jumps_from_links[1]);
    list_init(&block->chain);
    list_init(&block->page[0]);
    list_init(&block->page[1]);
    list_init(&block->jetsam);
    block->is_jetsam = false;
    if (state->block_patch_ip != 0)
        block->code[state->block_patch_ip] = (unsigned long) block;
    block->end_addr = block->addr == state->ip ? block->addr : state->ip - 1;
}

static bool rv_ends_block(const struct rv_insn *insn) {
    switch (insn->op) {
    case RV_OP_JAL:
    case RV_OP_JALR:
    case RV_OP_BRANCH:
    case RV_OP_ILLEGAL:
        return true;
    case RV_OP_SYSTEM:
        return insn->funct3 == 0;
    default:
        return false;
    }
}

static bool gen_lowered(struct gen_state *state, const struct rv_insn *insn) {
    void (*gadget)(void) = NULL;
    unsigned long fake_ip = state->orig_ip | (1ul << 63);
    switch (insn->op) {
    case RV_OP_LUI: {
        extern void gadget_rv_lui(void);
        gen(state, (unsigned long) gadget_rv_lui);
        gen(state, insn->rd);
        gen(state, (unsigned long) insn->imm);
        gen(state, state->orig_ip + insn->length);
        return true;
    }
    case RV_OP_AUIPC: {
        extern void gadget_rv_auipc(void);
        gen(state, (unsigned long) gadget_rv_auipc);
        gen(state, insn->rd);
        gen(state, (unsigned long) insn->imm);
        gen(state, state->orig_ip);
        gen(state, state->orig_ip + insn->length);
        return true;
    }
    case RV_OP_JAL: {
        extern void gadget_rv_jal(void);
        gen(state, (unsigned long) gadget_rv_jal);
        gen(state, insn->rd);
        gen(state, state->orig_ip + insn->length);
        gen(state, 0);
        state->block_patch_ip = state->size - 1;
        gen(state, fake_ip + insn->length);
        gen(state, fake_ip + insn->imm);
        state->jump_ip[0] = state->size - 2;
        state->jump_ip[1] = state->size - 1;
        return true;
    }
    case RV_OP_JALR: {
        if (insn->rd == 0 && insn->rs1 == 1 && insn->imm == 0) {
            extern void gadget_rv_ret(void);
            gen(state, (unsigned long) gadget_rv_ret);
            return true;
        }
        extern void gadget_rv_jalr(void);
        gen(state, (unsigned long) gadget_rv_jalr);
        gen(state, insn->rd);
        gen(state, insn->rs1);
        gen(state, (unsigned long) insn->imm);
        gen(state, state->orig_ip + insn->length);
        return true;
    }
    case RV_OP_BRANCH: {
        extern void gadget_rv_branch(void);
        gen(state, (unsigned long) gadget_rv_branch);
        gen(state, insn->funct3);
        gen(state, insn->rs1);
        gen(state, insn->rs2);
        gen(state, fake_ip + insn->imm);
        gen(state, fake_ip + insn->length);
        state->jump_ip[0] = state->size - 2;
        state->jump_ip[1] = state->size - 1;
        return true;
    }
    case RV_OP_SYSTEM:
        if (insn->funct3 == 0 && insn->imm == 0) {
            extern void gadget_rv_ecall(void);
            gen(state, (unsigned long) gadget_rv_ecall);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        if (insn->funct3 == 0 && insn->imm == 1) {
            extern void gadget_rv_ebreak(void);
            gen(state, (unsigned long) gadget_rv_ebreak);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        if (insn->funct3 != 0) {
            extern void gadget_rv_csr(void);
            gen(state, (unsigned long) gadget_rv_csr);
            gen(state, insn->funct3);
            gen(state, insn->rd);
            gen(state, insn->rs1);
            gen(state, (unsigned long) rv_bits(insn->expanded, 31, 20));
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        break;
    case RV_OP_LOAD:
        if (insn->rs1 == 2 && (insn->funct3 == 2 || insn->funct3 == 3)) {
            extern void gadget_rv_lw_sp(void);
            extern void gadget_rv_ld_sp(void);
            gen(state, (unsigned long) (insn->funct3 == 2 ? gadget_rv_lw_sp : gadget_rv_ld_sp));
            gen(state, insn->rd);
            gen(state, (unsigned long) insn->imm);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        switch (insn->funct3) {
        case 0: { extern void gadget_rv_lb(void); gadget = gadget_rv_lb; break; }
        case 1: { extern void gadget_rv_lh(void); gadget = gadget_rv_lh; break; }
        case 2: { extern void gadget_rv_lw(void); gadget = gadget_rv_lw; break; }
        case 3: { extern void gadget_rv_ld(void); gadget = gadget_rv_ld; break; }
        case 4: { extern void gadget_rv_lbu(void); gadget = gadget_rv_lbu; break; }
        case 5: { extern void gadget_rv_lhu(void); gadget = gadget_rv_lhu; break; }
        case 6: { extern void gadget_rv_lwu(void); gadget = gadget_rv_lwu; break; }
        default: break;
        }
        if (gadget != NULL)
            goto gen_imm;
        break;
    case RV_OP_STORE:
        if (insn->rs1 == 2 && (insn->funct3 == 2 || insn->funct3 == 3)) {
            extern void gadget_rv_sw_sp(void);
            extern void gadget_rv_sd_sp(void);
            gen(state, (unsigned long) (insn->funct3 == 2 ? gadget_rv_sw_sp : gadget_rv_sd_sp));
            gen(state, insn->rs2);
            gen(state, (unsigned long) insn->imm);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        switch (insn->funct3) {
        case 0: { extern void gadget_rv_sb(void); gadget = gadget_rv_sb; break; }
        case 1: { extern void gadget_rv_sh(void); gadget = gadget_rv_sh; break; }
        case 2: { extern void gadget_rv_sw(void); gadget = gadget_rv_sw; break; }
        case 3: { extern void gadget_rv_sd(void); gadget = gadget_rv_sd; break; }
        default: break;
        }
        if (gadget != NULL) {
            gen(state, (unsigned long) gadget);
            gen(state, insn->rs1);
            gen(state, insn->rs2);
            gen(state, (unsigned long) insn->imm);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        break;
    case RV_OP_AMO:
        if (insn->funct3 == 2 || insn->funct3 == 3) {
            extern void gadget_rv_amo(void);
            gen(state, (unsigned long) gadget_rv_amo);
            gen(state, (unsigned long) rv_bits(insn->expanded, 31, 27));
            gen(state, insn->funct3 == 2 ? 4 : 8);
            gen(state, insn->rd);
            gen(state, insn->rs1);
            gen(state, insn->rs2);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        break;
    case RV_OP_LOAD_FP:
        switch (insn->funct3) {
        case 2: { extern void gadget_rv_flw(void); gadget = gadget_rv_flw; break; }
        case 3: { extern void gadget_rv_fld(void); gadget = gadget_rv_fld; break; }
        default: break;
        }
        if (gadget != NULL)
            goto gen_imm;
        break;
    case RV_OP_STORE_FP:
        switch (insn->funct3) {
        case 2: { extern void gadget_rv_fsw(void); gadget = gadget_rv_fsw; break; }
        case 3: { extern void gadget_rv_fsd(void); gadget = gadget_rv_fsd; break; }
        default: break;
        }
        if (gadget != NULL) {
            gen(state, (unsigned long) gadget);
            gen(state, insn->rs1);
            gen(state, insn->rs2);
            gen(state, (unsigned long) insn->imm);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        break;
    case RV_OP_MISC_MEM:
        if (insn->funct3 == 0 || insn->funct3 == 1) {
            extern void gadget_rv_fence(void);
            gen(state, (unsigned long) gadget_rv_fence);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        break;
    case RV_OP_OP_IMM:
        if (insn->funct3 == 0 && insn->rd == 2 && insn->rs1 == 2) {
            extern void gadget_rv_addi_sp(void);
            gen(state, (unsigned long) gadget_rv_addi_sp);
            gen(state, (unsigned long) insn->imm);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        switch (insn->funct3) {
        case 0: { extern void gadget_rv_addi(void); gadget = gadget_rv_addi; break; }
        case 1: { extern void gadget_rv_slli(void); gadget = gadget_rv_slli; break; }
        case 2: { extern void gadget_rv_slti(void); gadget = gadget_rv_slti; break; }
        case 3: { extern void gadget_rv_sltiu(void); gadget = gadget_rv_sltiu; break; }
        case 4: { extern void gadget_rv_xori(void); gadget = gadget_rv_xori; break; }
        case 5:
            if (rv_bits(insn->expanded, 31, 26) == 0x10) {
                extern void gadget_rv_srai(void); gadget = gadget_rv_srai;
            } else {
                extern void gadget_rv_srli(void); gadget = gadget_rv_srli;
            }
            break;
        case 6: { extern void gadget_rv_ori(void); gadget = gadget_rv_ori; break; }
        case 7: { extern void gadget_rv_andi(void); gadget = gadget_rv_andi; break; }
        default:
            break;
        }
        if (gadget != NULL)
            goto gen_imm;
        break;
    case RV_OP_OP_IMM_32:
        switch (insn->funct3) {
        case 0: { extern void gadget_rv_addiw(void); gadget = gadget_rv_addiw; break; }
        case 1: { extern void gadget_rv_slliw(void); gadget = gadget_rv_slliw; break; }
        case 5:
            if (insn->funct7 == 0x20) {
                extern void gadget_rv_sraiw(void); gadget = gadget_rv_sraiw;
            } else {
                extern void gadget_rv_srliw(void); gadget = gadget_rv_srliw;
            }
            break;
        default:
            break;
        }
        if (gadget != NULL)
            goto gen_imm;
        break;
    case RV_OP_OP:
        if (insn->funct7 == 0x00) {
            switch (insn->funct3) {
            case 0: { extern void gadget_rv_add(void); gadget = gadget_rv_add; break; }
            case 1: { extern void gadget_rv_sll(void); gadget = gadget_rv_sll; break; }
            case 2: { extern void gadget_rv_slt(void); gadget = gadget_rv_slt; break; }
            case 3: { extern void gadget_rv_sltu(void); gadget = gadget_rv_sltu; break; }
            case 4: { extern void gadget_rv_xor(void); gadget = gadget_rv_xor; break; }
            case 5: { extern void gadget_rv_srl(void); gadget = gadget_rv_srl; break; }
            case 6: { extern void gadget_rv_or(void); gadget = gadget_rv_or; break; }
            case 7: { extern void gadget_rv_and(void); gadget = gadget_rv_and; break; }
            default: break;
            }
        } else if (insn->funct7 == 0x20) {
            switch (insn->funct3) {
            case 0: { extern void gadget_rv_sub(void); gadget = gadget_rv_sub; break; }
            case 5: { extern void gadget_rv_sra(void); gadget = gadget_rv_sra; break; }
            default: break;
            }
        } else if (insn->funct7 == 0x01) {
            switch (insn->funct3) {
            case 0: { extern void gadget_rv_mul(void); gadget = gadget_rv_mul; break; }
            case 1: { extern void gadget_rv_mulh(void); gadget = gadget_rv_mulh; break; }
            case 2: { extern void gadget_rv_mulhsu(void); gadget = gadget_rv_mulhsu; break; }
            case 3: { extern void gadget_rv_mulhu(void); gadget = gadget_rv_mulhu; break; }
            case 4: { extern void gadget_rv_div(void); gadget = gadget_rv_div; break; }
            case 5: { extern void gadget_rv_divu(void); gadget = gadget_rv_divu; break; }
            case 6: { extern void gadget_rv_rem(void); gadget = gadget_rv_rem; break; }
            case 7: { extern void gadget_rv_remu(void); gadget = gadget_rv_remu; break; }
            default: break;
            }
        }
        if (gadget != NULL)
            goto gen_reg;
        break;
    case RV_OP_OP_32:
        if (insn->funct7 == 0x00) {
            switch (insn->funct3) {
            case 0: { extern void gadget_rv_addw(void); gadget = gadget_rv_addw; break; }
            case 1: { extern void gadget_rv_sllw(void); gadget = gadget_rv_sllw; break; }
            case 5: { extern void gadget_rv_srlw(void); gadget = gadget_rv_srlw; break; }
            default: break;
            }
        } else if (insn->funct7 == 0x20) {
            switch (insn->funct3) {
            case 0: { extern void gadget_rv_subw(void); gadget = gadget_rv_subw; break; }
            case 5: { extern void gadget_rv_sraw(void); gadget = gadget_rv_sraw; break; }
            default: break;
            }
        } else if (insn->funct7 == 0x01) {
            switch (insn->funct3) {
            case 0: { extern void gadget_rv_mulw(void); gadget = gadget_rv_mulw; break; }
            case 4: { extern void gadget_rv_divw(void); gadget = gadget_rv_divw; break; }
            case 5: { extern void gadget_rv_divuw(void); gadget = gadget_rv_divuw; break; }
            case 6: { extern void gadget_rv_remw(void); gadget = gadget_rv_remw; break; }
            case 7: { extern void gadget_rv_remuw(void); gadget = gadget_rv_remuw; break; }
            default: break;
            }
        }
        if (gadget != NULL)
            goto gen_reg;
        break;
    case RV_OP_OP_FP:
    case RV_OP_MADD:
    case RV_OP_MSUB:
    case RV_OP_NMSUB:
    case RV_OP_NMADD: {
        extern void gadget_rv_fp(void);
        gen(state, (unsigned long) gadget_rv_fp);
        gen(state, insn->op);
        gen(state, (unsigned long) rv_bits(insn->expanded, 31, 27));
        gen(state, (unsigned long) rv_bits(insn->expanded, 26, 25));
        gen(state, insn->funct3);
        gen(state, insn->rd);
        gen(state, insn->rs1);
        gen(state, insn->rs2);
        gen(state, insn->rs3);
        gen(state, state->orig_ip + insn->length);
        return true;
    }
    default:
        break;
    }
    return false;

gen_imm:
    gen(state, (unsigned long) gadget);
    gen(state, insn->rd);
    gen(state, insn->rs1);
    gen(state, (unsigned long) insn->imm);
    gen(state, state->orig_ip + insn->length);
    return true;

gen_reg:
    gen(state, (unsigned long) gadget);
    gen(state, insn->rd);
    gen(state, insn->rs1);
    gen(state, insn->rs2);
    gen(state, state->orig_ip + insn->length);
    return true;
}

int gen_step(struct gen_state *state, struct tlb *tlb) {
    uint8_t bytes[4] = {0};
    state->orig_ip = state->ip;
    state->orig_ip_extra = 0;

    if (!tlb_read(tlb, state->ip, bytes, sizeof(uint16_t))) {
        extern void gadget_rv_interrupt(void);
        gen(state, (unsigned long) gadget_rv_interrupt);
        gen(state, INT_GPF);
        return false;
    }

    uint16_t h = (uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8);
    size_t length = (h & 3) == 3 ? sizeof(uint32_t) : sizeof(uint16_t);
    if (length == sizeof(uint32_t) &&
            !tlb_read(tlb, state->ip + sizeof(uint16_t), bytes + sizeof(uint16_t), sizeof(uint16_t))) {
        extern void gadget_rv_interrupt(void);
        gen(state, (unsigned long) gadget_rv_interrupt);
        gen(state, INT_GPF);
        return false;
    }

    struct rv_insn insn;
    bool decoded = rv_decode(bytes, length, &insn);

    if (!decoded || !gen_lowered(state, &insn)) {
        extern void gadget_rv_exec(void);
        gen(state, (unsigned long) gadget_rv_exec);
        gen(state, state->orig_ip + insn.length);
    }

    state->ip += insn.length;
    bool continues = decoded && !rv_ends_block(&insn);
    if (!continues)
        gen_exit(state);
    return continues;
}
