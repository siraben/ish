// RV64GC instruction → host-gadget emitter.
//
// Phase 0 placeholder. The emitter has the same public surface as the x86
// version (asbestos/gen.h) so guest-isa-agnostic callers in asbestos.c can
// remain unchanged once we promote it to the shared path.
//
// Real implementation lands in Phase 2 alongside the gadget pool.

#ifndef EMU_RISCV_GEN_H
#define EMU_RISCV_GEN_H

#include "asbestos/asbestos.h"
#include "emu/tlb.h"

struct gen_state {
    addr_t ip;
    addr_t orig_ip;
    unsigned long orig_ip_extra;
    struct fiber_block *block;
    unsigned size;
    unsigned capacity;
    unsigned jump_ip[2];
    unsigned block_patch_ip;
};

void gen_start(addr_t addr, struct gen_state *state);
void gen_exit(struct gen_state *state);
void gen_end(struct gen_state *state);

int gen_step(struct gen_state *state, struct tlb *tlb);

#endif
