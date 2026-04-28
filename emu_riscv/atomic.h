#ifndef EMU_RISCV_ATOMIC_H
#define EMU_RISCV_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>

#include "misc.h"

struct cpu_state;
struct tlb;

bool rv_atomic_load(struct cpu_state *cpu, struct tlb *tlb, addr_t addr,
        uint64_t *out, unsigned width, bool write_fault);
bool rv_atomic_store(struct cpu_state *cpu, struct tlb *tlb, addr_t addr,
        uint64_t value, unsigned width);
void rv_reservation_set(struct cpu_state *cpu, addr_t addr, uint64_t value);
bool rv_reservation_matches(struct cpu_state *cpu, addr_t addr, uint64_t value);
void rv_atomic_lock(void);
void rv_atomic_unlock(void);

#endif
