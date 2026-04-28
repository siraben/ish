#include <pthread.h>

#include "emu_riscv/atomic.h"
#include "emu_riscv/cpu.h"
#include "emu/tlb.h"

static pthread_mutex_t rv_atomic_mutex = PTHREAD_MUTEX_INITIALIZER;

struct rv_reservation {
    struct cpu_state *cpu;
    addr_t addr;
    uint64_t value;
};

static struct rv_reservation rv_reservations[256];

void rv_atomic_lock(void) {
    pthread_mutex_lock(&rv_atomic_mutex);
}

void rv_atomic_unlock(void) {
    pthread_mutex_unlock(&rv_atomic_mutex);
}

bool rv_atomic_load(struct cpu_state *cpu, struct tlb *tlb, addr_t addr,
        uint64_t *out, unsigned width, bool write_fault) {
    *out = 0;
    if (tlb_read(tlb, addr, out, width))
        return true;
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = write_fault;
    return false;
}

bool rv_atomic_store(struct cpu_state *cpu, struct tlb *tlb, addr_t addr,
        uint64_t value, unsigned width) {
    bool ok;
    if (width == 4) {
        uint32_t narrow = value;
        ok = tlb_write(tlb, addr, &narrow, width);
    } else {
        ok = tlb_write(tlb, addr, &value, width);
    }
    if (!ok) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return false;
    }
    return true;
}

void rv_reservation_set(struct cpu_state *cpu, addr_t addr, uint64_t value) {
    size_t slot = ((uintptr_t) cpu >> 4) % (sizeof(rv_reservations) / sizeof(rv_reservations[0]));
    rv_reservations[slot] = (struct rv_reservation) {
        .cpu = cpu,
        .addr = addr,
        .value = value,
    };
}

bool rv_reservation_matches(struct cpu_state *cpu, addr_t addr, uint64_t value) {
    size_t slot = ((uintptr_t) cpu >> 4) % (sizeof(rv_reservations) / sizeof(rv_reservations[0]));
    struct rv_reservation *reservation = &rv_reservations[slot];
    return reservation->cpu == cpu && reservation->addr == addr &&
        reservation->value == value;
}
