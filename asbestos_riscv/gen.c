#include <assert.h>
#include <stdlib.h>

#include "asbestos_riscv/gen.h"
#include "debug.h"
#include "emu_riscv/decode.h"
#include "emu/interrupt.h"

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
    case RV_OP_SYSTEM:
    case RV_OP_ILLEGAL:
        return true;
    default:
        return false;
    }
}

static bool gen_lowered(struct gen_state *state, const struct rv_insn *insn) {
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
    case RV_OP_OP_IMM:
        if (insn->funct3 == 0) {
            extern void gadget_rv_addi(void);
            gen(state, (unsigned long) gadget_rv_addi);
            gen(state, insn->rd);
            gen(state, insn->rs1);
            gen(state, (unsigned long) insn->imm);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        break;
    case RV_OP_OP:
        if (insn->funct7 == 0x00 && insn->funct3 == 0) {
            extern void gadget_rv_add(void);
            gen(state, (unsigned long) gadget_rv_add);
            gen(state, insn->rd);
            gen(state, insn->rs1);
            gen(state, insn->rs2);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        if (insn->funct7 == 0x20 && insn->funct3 == 0) {
            extern void gadget_rv_sub(void);
            gen(state, (unsigned long) gadget_rv_sub);
            gen(state, insn->rd);
            gen(state, insn->rs1);
            gen(state, insn->rs2);
            gen(state, state->orig_ip + insn->length);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
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
