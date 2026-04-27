#ifndef EMU_RISCV_CPU_H
#define EMU_RISCV_CPU_H

#include <stdint.h>
#include "misc.h"
#include "emu/mmu.h"

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

struct cpu_state;
struct tlb;
int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb);
void cpu_poke(struct cpu_state *cpu);

// RV64GC guest CPU state.
//
// Register file: 32 GPRs (x[0] is hardwired zero — writes silently dropped at
// emit time, never stored), 32 FPRs (each 64-bit; F-extension uses the low
// 32 bits and sets NaN-box upper bits). PC is the architectural program
// counter. fcsr is the FP control-and-status register: low 5 bits are the
// accrued exception flags (NV/DZ/OF/UF/NX), bits [7:5] are the rounding mode.
//
// We intentionally mirror a few field names from the x86 cpu_state (eip, mmu,
// poked_ptr, _poked, trapno, segfault_*) so that guest-isa-agnostic plumbing
// in the kernel and JIT can address them uniformly. eip aliases pc here.

struct cpu_state {
    struct mmu *mmu;
    long cycle;

    // x[0] is the architectural zero register. Stored for uniform indexing;
    // gen.c must not emit writes to x[0].
    union {
        uint64_t x[32];
        struct {
            uint64_t x0;        // zero
            uint64_t ra;        // x1, return address
            uint64_t sp;        // x2, stack pointer
            uint64_t gp;        // x3, global pointer
            uint64_t tp;        // x4, thread pointer (TLS base)
            uint64_t t0, t1, t2;
            uint64_t s0, s1;    // s0 == fp
            uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
            uint64_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
            uint64_t t3, t4, t5, t6;
        };
    };

    // Program counter. Aliased as eip for shared kernel/JIT plumbing.
    union {
        uint64_t pc;
        uint64_t eip;
    };

    // Floating-point register file (F + D extensions). Each entry holds a
    // double; single-precision values occupy the low 32 bits with the upper
    // 32 bits NaN-boxed (all ones) per RV spec.
    uint64_t f[32];

    // FP control/status. Layout:
    //   [4:0]  accrued exception flags (NV DZ OF UF NX)
    //   [7:5]  rounding mode (RNE RTZ RDN RUP RMM 5-7=reserved/dynamic)
    uint32_t fcsr;

    // Reservation set tracking for LR/SC (A-extension). reservation_valid is
    // cleared on context switch, mem_changed, and any successful SC.
    uint64_t reservation_addr;
    bool reservation_valid;

    // Single-step / trap flag. Kept as `tf` to match shared dispatch in
    // asbestos.c (cpu->tf ? cpu_single_step : cpu_step_to_interrupt).
    bool tf;

    // Page-fault scratch (read by the kernel signal path).
    addr_t segfault_addr;
    bool segfault_was_write;

    // Last interrupt number raised by the JIT.
    dword_t trapno;

    // Async wake-up flag (kept identical in shape to x86 cpu_state so the
    // shared poke machinery works without changes).
    bool *poked_ptr;
    bool _poked;
};

#define CPU_OFFSET(field) offsetof(struct cpu_state, field)

static_assert(sizeof(((struct cpu_state *)0)->x) == 32 * 8, "RV GPR file size");
static_assert(sizeof(((struct cpu_state *)0)->f) == 32 * 8, "RV FPR file size");
static_assert(CPU_OFFSET(x0) == CPU_OFFSET(x[0]), "x register order");
static_assert(CPU_OFFSET(ra) == CPU_OFFSET(x[1]), "x register order");
static_assert(CPU_OFFSET(sp) == CPU_OFFSET(x[2]), "x register order");
static_assert(CPU_OFFSET(tp) == CPU_OFFSET(x[4]), "x register order");
static_assert(CPU_OFFSET(a0) == CPU_OFFSET(x[10]), "x register order");
static_assert(CPU_OFFSET(a7) == CPU_OFFSET(x[17]), "x register order");
static_assert(sizeof(struct cpu_state) < 0xffff, "cpu struct is too big for vector gadgets");

// Symbolic names for the 32 RV GPRs. Matches the ABI register convention
// (zero ra sp gp tp t0..2 s0/fp s1 a0..7 s2..11 t3..6).
enum rv_reg {
    rv_zero = 0, rv_ra, rv_sp, rv_gp, rv_tp,
    rv_t0, rv_t1, rv_t2,
    rv_s0, rv_s1,
    rv_a0, rv_a1, rv_a2, rv_a3, rv_a4, rv_a5, rv_a6, rv_a7,
    rv_s2, rv_s3, rv_s4, rv_s5, rv_s6, rv_s7, rv_s8, rv_s9, rv_s10, rv_s11,
    rv_t3, rv_t4, rv_t5, rv_t6,
    rv_reg_count = 32,
};

static inline const char *rv_reg_name(enum rv_reg r) {
    static const char *names[32] = {
        "zero","ra","sp","gp","tp","t0","t1","t2",
        "s0","s1","a0","a1","a2","a3","a4","a5",
        "a6","a7","s2","s3","s4","s5","s6","s7",
        "s8","s9","s10","s11","t3","t4","t5","t6",
    };
    if ((unsigned)r >= 32) return "?";
    return names[r];
}

#endif
