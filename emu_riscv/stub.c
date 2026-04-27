// Phase 0 scaffolding object for the RV64GC port.
//
// This translation unit keeps the cpu_state static assertions in the RV64
// build even before the full JIT is linked into an executable. Its purpose is
// to validate that:
//   1. -Dguest_isa=riscv64 selects this compilation path,
//   2. emu_riscv/cpu.h is internally consistent (static_asserts pass),
//   3. the resulting library archive links cleanly while the port is phased.
//
// Real executable entry points (cpu_run_to_interrupt, cpu_poke, asbestos_*,
// the gadget pool) are introduced in the later phases described in plan.md.

#include "emu_riscv/cpu.h"

// Force the compiler to evaluate the static_asserts in cpu.h by referencing
// the type. The unused-variable attribute keeps -Wunused-* quiet.
static struct cpu_state __attribute__((unused)) ish_riscv_cpu_state_probe;

const int ish_riscv_stub_object_present = 1;
