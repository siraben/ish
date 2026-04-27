#include <assert.h>
#include <limits.h>
#include <stdlib.h>

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
    block->jump_ip[0] = NULL;
    block->jump_ip[1] = NULL;
    block->old_jump_ip[0] = 0;
    block->old_jump_ip[1] = 0;
    list_init(&block->jumps_from[0]);
    list_init(&block->jumps_from[1]);
    list_init(&block->jumps_from_links[0]);
    list_init(&block->jumps_from_links[1]);
    list_init(&block->chain);
    list_init(&block->page[0]);
    list_init(&block->page[1]);
    list_init(&block->jetsam);
    block->is_jetsam = false;
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
        gen(state, state->orig_ip + insn->imm);
        return true;
    }
    case RV_OP_JALR: {
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
        gen(state, state->orig_ip + insn->imm);
        gen(state, state->orig_ip + insn->length);
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
        break;
    case RV_OP_LOAD:
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
