# iSH x86_32 → RV64GC Port: Work Catalogue

End-to-end plan. Replace the x86_32 guest emulator with an RV64GC guest, ARM64 host only, success = Alpine `apk add` works on both the CLI driver and the iOS app.

---

## Context

iSH is a userspace Linux emulator that JIT-compiles 32-bit x86 to native code via the Asbestos engine. The current build hard-codes:

- **Guest ISA**: 32-bit x86, baked into `emu/decode.h`, `emu/cpu.h`, `asbestos/gen.c`, all `kernel/*.c` register accesses, the vDSO, and the syscall table.
- **Host ISA**: parameterised — `meson.build:54` selects `asbestos/gadgets-{aarch64,x86_64}/`. The aarch64 gadget pool is well-developed.

The port keeps Asbestos's threaded-gadget architecture and the userspace-Linux kernel emulation, replacing only the guest side. Eventually the x86 paths are deleted entirely; during phasing they coexist behind the `guest_isa` meson option.

### Why RV64GC, briefly

- No flags → eliminates the lazy-flags machinery and ~30% of the gadget pool.
- Fixed-length 32/16-bit decode → the decoder is a fraction of `emu/decode.h`'s size.
- Weak memory model matches the ARM64 host → no fences for ordering.
- 32 GPRs map cleanly onto ARM64's 31 host GPRs.
- Alpine `riscv64` is a real, maintained port.

---

## Architectural shape

### Stays (guest-isa-agnostic, reusable)

- `kernel/task.c`, `kernel/memory.c` (data structures need 64-bit widening, not rewrite).
- `kernel/calls.c` *handlers* (`sys_read`, `sys_mmap`, etc.) — only dispatch + arg extraction changes.
- `kernel/fork.c`, `kernel/exit.c`, `kernel/futex.c`, `kernel/poll.c`, `kernel/eventfd.c`, `kernel/random.c`, `kernel/getset.c`, `kernel/ipc.c`, `kernel/group.c`.
- All of `fs/`.
- `emu/tlb.{c,h}` (does not touch cpu fields; pure address translation).
- `emu/interrupt.h` (interrupt enum).
- Asbestos block-cache machinery: `asbestos/asbestos.c`, `asbestos.h`, `frame.h`, jetsam, hash table, direct-link patching.
- `tools/fakefsify.c`, `main.c` (CLI driver).
- `util/`, `platform/`.

### Replaced

- `emu/decode.h` → `emu_riscv/decode.h` (RV64GC decoder).
- `emu/cpu.h` → `emu_riscv/cpu.h` (RV register file, no flags, no segments, no x87).
- `emu/fpu.c`, `emu/vec.c`, `emu/mmx.c`, `emu/float80.c` → `emu_riscv/fp.c` (F/D extension; ARM64 host has matching FP).
- `asbestos/gen.c` → `asbestos_riscv/gen.c` (RV instruction → gadget emission).
- `asbestos/gadgets-aarch64/{entry,memory,control,math,bits,string,misc}.S` → `asbestos_riscv/gadgets-aarch64/{entry,memory,arith,control,mul,atomic,fp}.S`.
- `asbestos/gadgets-aarch64/gadgets.h` → `asbestos_riscv/gadgets-aarch64/gadgets.h` (new register pinning, no flag macros).
- `asbestos/offsets.c` → `asbestos_riscv/offsets.c` (regenerates `cpu-offsets.h` for the new struct).
- `asbestos/helpers.c` → `asbestos_riscv/helpers.c` (helpers for instructions awkward to express as gadgets).
- `vdso/` → `vdso_riscv/` (RV64 ELF with `__vdso_rt_sigreturn`, `__vdso_clock_gettime`).
- `kernel/exec.c` ELF magic + load logic.
- `kernel/calls.c` syscall table (Linux generic syscall numbers — different from i386).
- `kernel/signal.{c,h}` sigframe layout.
- `kernel/tls.c` (delete `set_thread_area`; tp comes from `clone(CLONE_SETTLS)`).
- `kernel/uname.c` arch string.
- `kernel/vdso.c` (loads a different vDSO blob).
- `fs/proc/cpuinfo` ISA string.
- `app/iSH.xcconfig` (`ROOTFS_URL`).
- `app/CurrentRoot.h` apk version metadata.
- `tests/e2e/e2e.bash` `ALPINE_IMAGE` URL, fixtures.
- `tests/manual/`, `tests/e2e/sse2/`, `tests/e2e/fpu/` x86 fixtures.

### Widened (32-bit → 64-bit)

- `addr_t` in `misc.h` (currently `dword_t = uint32_t`) → `uint64_t`. Ripples across every file using guest pointers.
- `kernel/memory.{c,h}` `pgdir`: 2-level 1024×1024 → 3-level Sv39 (9+9+9+12).
- `emu/tlb.{h,c}` hash function input width.
- `fs/fake-db.c` schema if it stores guest addresses (audit needed).

---

## Cross-cutting concerns (apply across phases)

These traps surface in multiple phases; flag them early:

**FP rounding modes.** RV's FCSR has a 3-bit rounding mode. Each FP instruction has a 3-bit `rm` field; `rm = 0b111` ("DYN") means "use FCSR". ARM64 FPCR is a global control. Three options:
- A) Sync FPCR ← FCSR on every FP instruction (high overhead).
- B) Pin FPCR to FCSR's mode; sync only on FCSR writes; recompute on instructions with non-DYN `rm`.
- C) Implement FP via host C helpers using `fesetround`.

Recommendation: B. Decision needed in Phase 2.

**A-extension correctness.** LR/SC reservation set:
- Cleared on context switch (any task scheduler entry).
- Cleared on cross-page write through TLB.
- Cleared on `mem_changed` invalidations.
- Cleared on signal delivery (including timer interrupts).

LR/SC sequence on ARM64: implement LR via `LDAXR` and SC via `STLXR` for the same address. Spurious failure is ABI-allowed but timer-interrupt-induced unbounded failure is not. Either:
- Inhibit timer interrupts during a 1–16 instruction window after LR (block-end logic must be aware), or
- Restart at LR on signal arrival (more complex).

AMO instructions on ARM64 with LSE: direct map to `LDADD`/`LDSET`/`LDCLR`/`LDEOR`/`SWP` with appropriate `aq`/`rl` ordering. Without LSE: LDAXR/STLXR loop.

**C-extension misalignment.** PC may be 2-byte aligned, not 4-byte. Decoder dispatches on `*pc & 3 == 3` for 32-bit, else 16-bit. Block-end logic that assumes `next_pc = pc + 4` must consult the actual instruction length.

**Signal-during-LR/SC.** See above. Resolve in Phase 2 design; verify with concurrent atomic stress test in Phase 6.

**Sv39 vs Sv48.** Linux RV64 typically uses Sv39 (39-bit VA, 512 GB user). Sv48 is supported but optional. Default to Sv39 — matches musl/glibc layout and keeps the page walk to 3 levels. Decision in Phase 4.

**Signal frame ABI compatibility.** Signal frame layout is part of the kernel ABI; user-space sigreturn trampolines are picky. Match upstream Linux's `<asm/sigcontext.h>` for riscv64 byte-for-byte.

**Atomic memcpy semantics for cross-page operations.** Cross-page LR/SC is impossible in real hardware (PMA enforced). For the emulator: signal SIGBUS on a cross-page atomic, matching real RV behaviour.

---

## Phase 0 — Scaffolding ✓ DONE

**Goal**: Add the new tree alongside the old without disturbing the existing build.

**Deliverables**:
- `meson_options.txt`: `guest_isa` combo, choices `['x86', 'riscv64']`, default `'x86'`.
- `meson.build`: gates `subdir('vdso')`, `subdir('deps')`, kernel block, `subdir('tools')`, tests on `guest_isa == 'x86'`. RV64 build path: `emu_src = ['emu_riscv/stub.c']`.
- `emu_riscv/cpu.h`: real RV64GC `struct cpu_state` — 32×u64 GPR file (with named ABI aliases under a union with `x[32]`), 32×u64 FPR file, `pc` (aliased as `eip`), `fcsr`, A-extension reservation tracking, `tf`/`trapno`/`segfault_*`/`poked_ptr` mirroring x86 cpu_state's shared fields.
- `emu_riscv/decode.h`: empty placeholder.
- `emu_riscv/stub.c`: forces `cpu.h` static_asserts to compile.
- `asbestos_riscv/{gen.h,gen.c,gadgets-aarch64/entry.S}`: skeletons documenting Phase 2 intent. Not yet referenced by `meson.build`.

**Verification**:
- `meson setup build-x86 .` configures (10 targets).
- `meson setup build-rv64 -Dguest_isa=riscv64 .` configures (2 targets).
- `ninja -C build-x86` builds `ish` binary.
- `ninja -C build-rv64` builds `libish_emu.a`.

---

## Phase 1 — RV64GC decoder

**Goal**: Decode any RV64GC instruction into an internal representation suitable for gadget emission. Standalone-testable via a disassembler tool.

**Deliverables**:

`emu_riscv/decode.h`: Macro-driven decoder, in the spirit of `emu/decode.h`'s pattern:

```
DECODE_INSTRUCTION:
  fetch first 16 bits → halfword h0
  if (h0 & 3) != 3:
    decode 16-bit (C extension), expand to 32-bit canonical
  else:
    fetch second 16 bits → halfword h1
    decode 32-bit
```

Steps:
1. **Opcode dispatch**: `opcode[6:0]` → 32 root buckets (`OP_LOAD`, `OP_LOAD_FP`, `OP_MISC_MEM`, `OP_OP_IMM`, `OP_AUIPC`, `OP_OP_IMM_32`, `OP_STORE`, `OP_STORE_FP`, `OP_AMO`, `OP_OP`, `OP_LUI`, `OP_OP_32`, `OP_MADD`, `OP_MSUB`, `OP_NMSUB`, `OP_NMADD`, `OP_OP_FP`, `OP_BRANCH`, `OP_JALR`, `OP_JAL`, `OP_SYSTEM`, ...).
2. **funct3/funct7 sub-dispatch**: per-bucket case analysis.
3. **Immediate decoders**: I-type, S-type, B-type, U-type, J-type, plus the C-extension's compressed immediates (CI/CSS/CIW/CL/CS/CA/CB/CJ formats).
4. **C-extension expansion table**: each compressed encoding maps to its 32-bit canonical equivalent (e.g., `c.add` → `add`). Implementation choice: a switch that emits the expanded form into a 32-bit buffer that re-enters the base decoder. This avoids duplicating semantic dispatch.

ISA coverage priority order:
1. RV64I base (~50 insns).
2. C extension (~30 compressed forms).
3. M extension (mul/div, 13 insns).
4. A extension (LR/SC, AMO ops, 22 insns).
5. F extension (single-precision FP, 26 insns).
6. D extension (double-precision FP, 26 insns).
7. Zicsr (CSRRW/CSRRS/CSRRC + immediate variants, 6 insns) — only the user-visible subset (`fflags`, `frm`, `fcsr`, `cycle`, `time`, `instret`).
8. Zifencei (`fence.i`, 1 insn).

Excluded for now: V (vector), B (bit-manip), Zfh (half-precision), H (hypervisor), supervisor-mode CSRs.

`tools/rvdecode.c`: standalone disassembler harness. Reads a static RV64 ELF, walks `.text`, prints one line per instruction. Used to diff against `riscv64-linux-gnu-objdump -d`.

`asbestos_riscv/gen.c`: gen_step() reads from the guest TLB, dispatches via decode.h macros, emits gadget pointers + immediates into `state->block->code[]`.

**Dependencies**:
- Phase 0 done.
- A `riscv64-linux-gnu-gcc` cross-toolchain on the build machine for producing test fixtures.

**Verification**:
- Cross-compile `static-musl-hello.c` → `hello.elf`.
- Run `tools/rvdecode hello.elf > ish.dump`.
- Run `riscv64-linux-gnu-objdump -d hello.elf > objdump.dump`.
- Diff is empty (after normalising address formatting).
- Same on `busybox.elf` (a much larger test binary).
- Same on `libc.so.6.elf` (covers FP-heavy paths like `printf`).

---

## Phase 2 — Gadget pool (ARM64 host)

**Goal**: For each RV64GC instruction emitted by gen.c, provide an ARM64 gadget that executes it on the host.

**Deliverables**:

`asbestos_riscv/gadgets-aarch64/gadgets.h`:
- Register pinning map. ARM64 has 31 GPRs. Reserved: x18 (platform), x29 (FP), x30 (LR), x16/x17 (intra-procedure), `_cpu`, `_tlb`, `_ip`, `_tmp`, `_addr`. Leaves ~22 for guest GPR pinning. Pin (subject to revision):
  - x1 (ra), x2 (sp), x3 (gp), x4 (tp), x5–x7 (t0–t2), x8 (s0/fp), x9 (s1), x10–x17 (a0–a7), x28–x31 (t3–t6).
  - s2–s11 (callee-saved RV regs, x18–x27) live in `cpu_state.x[]`; `gen.c` emits explicit load/store gadgets for these.
  - x0 (zero) is never materialised; `gen.c` emits the constant 0 directly when used as a source.
- `load_regs` / `save_regs` macros banking 22 RV regs into ARM64 regs at fiber entry/exit.
- No flag macros (none needed).
- `gret` macro identical in shape to x86 version (tail-call to next gadget pointer).

`asbestos_riscv/gadgets-aarch64/entry.S`:
- `fiber_enter(block, cpu, tlb)`: stack-save callee-saved ARM64 regs, set `_ip`/`_cpu`/`_tlb`, call `load_regs`, `gret` to first gadget.
- `fiber_ret_chain` / `fiber_ret` / `fiber_exit`: identical shape to x86 version.
- `interrupt`, `exit` gadgets.

`asbestos_riscv/gadgets-aarch64/arith.S`:
- `add`, `sub`, `and`, `or`, `xor`: direct ARM64 mapping.
- `addi`, `andi`, `ori`, `xori`: with embedded 12-bit immediate, sign-extended.
- `addw`, `subw`, `addiw`: 32-bit sign-extending operations on a 64-bit register file.
- `slt`, `sltu`, `slti`, `sltiu`: ARM64 `cmp` + `cset`.
- `sll`, `srl`, `sra` + immediate variants: register count (low 6 bits) for 64-bit; low 5 bits for word.
- `sllw`, `srlw`, `sraw` + immediate variants.
- `lui`: emit constant.
- `auipc`: PC + sign-extended 20-bit immediate, emitted at gen time.

`asbestos_riscv/gadgets-aarch64/memory.S`:
- `lb`, `lh`, `lw`, `ld` (signed loads), `lbu`, `lhu`, `lwu` (unsigned loads).
- `sb`, `sh`, `sw`, `sd` (stores).
- TLB probe inline: ~3 ARM64 instructions to check page hit, single host load/store on hit.
- Cross-page slow path: reuse `__tlb_read_cross_page` / `__tlb_write_cross_page`. Pattern matches x86 `crosspage_load`/`crosspage_store`.
- Misalignment: RV permits unaligned access (with potential SIGBUS); ARM64 also permits it on most loads/stores. No special handling needed.

`asbestos_riscv/gadgets-aarch64/control.S`:
- `beq`, `bne`, `blt`, `bge`, `bltu`, `bgeu`: emit ARM64 `cmp` + `b.cond`. Direct-link the not-taken path; the taken path goes through the dispatcher initially, then gets patched (reuse `asbestos.c`'s direct-link logic).
- `jal`: unconditional direct branch + write of return address; direct-link the target.
- `jalr`: indirect branch. Implement a 1- or 2-entry inline cache at the call site for hot returns. Maintain a return-cache (mirror `fiber_frame.ret_cache`) keyed on the link register's saved IP.

`asbestos_riscv/gadgets-aarch64/mul.S`:
- `mul`, `mulh`, `mulhsu`, `mulhu`: ARM64 `MUL`, `SMULH`, `UMULH`.
- `mulw`: `MUL` then `SXTW`.
- `div`, `divu`, `rem`, `remu`: ARM64 `SDIV`/`UDIV` + `MSUB` for remainder. Handle div-by-zero (RV returns -1 / dividend) and signed overflow (RV returns dividend / 0) — these differ from ARM64 trapping behaviour, so wrap with checks.
- `divw`, `divuw`, `remw`, `remuw`: 32-bit variants.

`asbestos_riscv/gadgets-aarch64/atomic.S`:
- `lr.w`, `lr.d`: emit ARM64 `LDAXR` + record `cpu->reservation_addr` and `cpu->reservation_valid = true`.
- `sc.w`, `sc.d`: check reservation; if valid + matching address, `STLXR`; else fail. Clear reservation on success or failure.
- AMO ops (`amoswap`, `amoadd`, `amoand`, `amoor`, `amoxor`, `amomin`, `amomax`, `amominu`, `amomaxu`) for `.w` and `.d` widths: with LSE, direct ARM64 atomics. Without LSE: `LDAXR`/`STLXR` loop.
- All AMO and SC paths: clear `cpu->reservation_valid`.
- Hooks in: `tlb` cross-page write (clear reservation), task scheduler (clear on context switch), `mem_changed` (clear).

`asbestos_riscv/gadgets-aarch64/fp.S`:
- F-extension single-precision: `fadd.s`, `fsub.s`, `fmul.s`, `fdiv.s`, `fsqrt.s`, `fmadd.s`/`fmsub.s`/`fnmadd.s`/`fnmsub.s`.
- D-extension double-precision: same operations with `.d`.
- Conversions: `fcvt.{w,wu,l,lu}.{s,d}`, `fcvt.{s,d}.{w,wu,l,lu}`, `fcvt.s.d`, `fcvt.d.s`.
- Sign-injection: `fsgnj`, `fsgnjn`, `fsgnjx` (S/D).
- Min/max: `fmin`, `fmax` (S/D). NaN handling per RV spec.
- Comparison: `feq`, `flt`, `fle` (S/D).
- Move: `fmv.x.w`, `fmv.w.x`, `fmv.x.d`, `fmv.d.x`.
- FP load/store: `flw`, `fld`, `fsw`, `fsd`.
- NaN-boxing for single-precision: writes to F regs of a single-precision result must NaN-box (set upper 32 bits to all 1s); reads of single-precision interpret non-NaN-boxed values as canonical NaN.
- Rounding mode: implement strategy B from cross-cutting concerns. Maintain FPCR ← FCSR.RM at FCSR write time. For instructions with non-DYN `rm`, set FPCR temporarily.
- Exception flags: ARM64 FPSR exception bits → FCSR accrued flags after each FP op.

`asbestos_riscv/gadgets-aarch64/csr.S`:
- `csrrw`, `csrrs`, `csrrc`, `csrrwi`, `csrrsi`, `csrrci` for the user-visible CSRs only.
- `fflags`, `frm`, `fcsr`: read/write low bits of `cpu->fcsr`.
- `cycle`, `time`, `instret`: read host monotonic clock or instruction counter (best-effort).
- All other CSRs: raise illegal-instruction.

`asbestos_riscv/gadgets-aarch64/system.S`:
- `ecall`: emit `interrupt INT_SYSCALL` gadget (numeric value matches Phase 3 dispatch).
- `ebreak`: emit `interrupt INT_BREAKPOINT`.
- `fence`, `fence.i`: emit a memory barrier gadget. On ARM64, `DMB ISH` for `fence`, `DMB ISH; ISB` for `fence.i`.
- `wfi`, `mret`, `sret`: not in U-mode binaries; raise illegal-instruction.

`asbestos_riscv/asbestos.c`: Decision point. Either:
- (a) Share the existing `asbestos/asbestos.c` by making `cpu.h` dispatch on `GUEST_RISCV64` (small invasive change to `emu/cpu.h`'s top to `#if`-include the right struct).
- (b) Maintain a near-copy in `asbestos_riscv/`.

Recommendation: (a). The shared file references only `cpu->mmu`, `cpu->poked_ptr`, `cpu->_poked`, `cpu->eip` (aliased), `cpu->tf`, `cpu->trapno`, `cpu->mmu->changes` — all present on the RV cpu_state.

`asbestos_riscv/helpers.c`: C helpers for instructions awkward to express as gadgets. Likely candidates: division by zero shaping, FP rounding mode setup, `cycle`/`time` CSR reads.

`asbestos_riscv/offsets.c`: regenerate `cpu-offsets.h` with the new struct's offsets — `CPU_x` (array base), `CPU_pc`, `CPU_fcsr`, `CPU_reservation_addr`, `CPU_reservation_valid`, `CPU_segfault_*`, `CPU_poked_ptr`, plus `LOCAL_*` from `fiber_frame` (unchanged), `FIBER_BLOCK_*`, `TLB_*`. Reuses `tools/staticdefine.sh`.

`meson.build`: extend the `guest_isa == 'riscv64'` branch:
```
emu_src = ['emu/tlb.c', 'emu_riscv/fp.c', ...,
           'asbestos_riscv/asbestos.c' (or shared),
           'asbestos_riscv/gen.c', 'asbestos_riscv/helpers.c',
           'asbestos_riscv/gadgets-aarch64/{entry,arith,memory,control,
                                            mul,atomic,fp,csr,system}.S',
           offsets_riscv]
```

**Dependencies**: Phase 1 done.

**Verification**:
- Stub kernel that just calls `cpu_run_to_interrupt` in a loop and exit-syscalls after N instructions.
- Hand-assembled RV64 binary that does pure arithmetic + ecall — runs to completion.
- Static C `puts("hi")` cross-compiled — exercises arith, memory, control, ecall.
- Static C atomic-counter program (one thread incrementing a shared counter via AMO) — exercises atomics.
- Static C FP smoke test (`printf("%f", 1.5 * 2.0)`) — exercises F/D and rounding mode.
- All before any kernel ABI changes — uses a stub syscall handler that just calls `write(1, …)` and `exit()`.

---

## Phase 3 — Kernel ABI (RV64 user-space)

**Goal**: Make the iSH kernel speak the RV64 Linux user ABI. This is run in parallel with Phase 2 once the cpu_state shape is settled.

**Deliverables**:

`kernel/exec.c`:
- `read_header` validations: `ELFCLASS64`, `ELF_LITTLEENDIAN`, machine = `EM_RISCV` (243).
- Program-header parsing: 64-bit `Elf64_Phdr` (64-bit `p_offset`, `p_vaddr`, etc.).
- Default load addresses (musl convention):
  - Static binary: as specified in PHDR (typically 0x10000 base).
  - Dynamic interpreter (`/lib/ld-musl-riscv64.so.1`): bias 0x40000000.
  - Stack top: 0x4000000000 (within Sv39's 512 GB user range), grows down.
- Auxv: 64-bit entries.
  - `AT_HWCAP`: bit-string for I/M/A/F/D/C extensions (use the `'I' | 'M' | 'A' | 'F' | 'D' | 'C'` bit pattern Linux uses).
  - `AT_PLATFORM`: `"riscv64"`.
  - `AT_BASE`, `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY`, `AT_SYSINFO_EHDR`, `AT_RANDOM`, `AT_SECURE`, `AT_PAGESZ`, `AT_FLAGS`, `AT_CLKTCK`, `AT_UID`/`EUID`/`GID`/`EGID`, `AT_EXECFN`.
  - Drop x86-specific entries (`AT_SYSINFO`, `AT_HWCAP2` if not applicable).
- Argv/envp/auxv layout: same shape as i386 but 64-bit pointers.

`kernel/calls.c`:
- Replace `syscall_table[]` with the Linux `<asm-generic/unistd.h>` numbering used by riscv64. Highlights:
  - `__NR_read = 63`, `__NR_write = 64`, `__NR_close = 57`, `__NR_openat = 56` (no `open`!), `__NR_mmap = 222`, `__NR_munmap = 215`, `__NR_brk = 214`, `__NR_clone = 220`, `__NR_execve = 221`, `__NR_exit = 93`, `__NR_exit_group = 94`, `__NR_rt_sigreturn = 139`, `__NR_futex = 98`, `__NR_clock_gettime = 113`.
- Drop syscalls not present in the generic table: `creat`, `open`, `mkdir` (use `*at` variants).
- Add new syscalls if not already present: `clone3`, `statx`, `getrandom`, `pidfd_*`.
- Dispatch: `INT_SYSCALL` handler reads `cpu->x[17]` (a7) for nr, `cpu->x[10..15]` (a0..a5) for args, writes `cpu->x[10]` for return. Move from `int 0x80`-style handling to `ecall`.
- Audit each handler for places that read/write `cpu->eax/ebx/...` directly (rare; most handlers take args by value).

`kernel/signal.{c,h}`:
- New `struct sigcontext` matching upstream `<asm/sigcontext.h>` for riscv64:
  ```
  struct sigcontext {
      __riscv_d_ext_state sc_fpregs;  // optional, conditional on __SC_HAS_FPREGS
      unsigned long sc_regs[32];      // x[0..31]
      unsigned long sc_pc;
      // ... padding to match upstream layout exactly
  };
  ```
  Match upstream byte-for-byte; user trampolines are picky.
- New `struct rt_sigframe`: `siginfo`, `ucontext`, with the new `sigcontext`.
- Drop the embedded x86 retcode (`mov $nr; int 0x80`); replace with calling vDSO's `__vdso_rt_sigreturn`.
- Drop non-RT signal frame entirely (RV64 only does RT signals — `rt_sigaction` is the only path).
- Signal stack alignment: 16 bytes (RV ABI).

`kernel/tls.c`:
- Delete `set_thread_area` syscall handler (no equivalent on RV64).
- TLS base lives in `cpu->tp` (which is `cpu->x[4]`).
- `clone(CLONE_SETTLS, ..., newtls, ...)` writes `child->cpu->tp = newtls`. Hook in `kernel/fork.c`.

`kernel/uname.c`:
- `uts->arch` = `"riscv64"`.
- `uts->release` ≥ `"5.10"` (musl probes for at least this).

`kernel/vdso.c`:
- Loads new `vdso_riscv/libvdso.so.elf` blob (built in Phase 5).

`fs/proc/cpuinfo` (in `fs/proc/root.c` or similar):
```
processor : 0
hart      : 0
isa       : rv64imafdc
mmu       : sv39
```

`fs/proc/version`: update generic kernel version string if hard-coded to mention x86.

`emu/interrupt.h`: ensure `INT_SYSCALL` numeric value works with `ecall`'s gadget emission. May add `INT_BREAKPOINT` if not present.

**Dependencies**:
- Phase 0 done.
- Phase 2 not strictly required, but the `cpu->x[]` field layout must be settled.

**Verification**:
- Combined with Phase 2 output: a hand-rolled static musl `write(1, "hi\n", 3); exit(0)` — prints "hi" through the iSH CLI driver.
- Static busybox echo runs: `./ish -f rv64-fakefs /bin/busybox echo hello`.

---

## Phase 4 — Memory model widening

**Goal**: Make guest pointers 64-bit. Replace the 2-level pgdir with a 3-level Sv39 walker.

**Deliverables**:

`misc.h`:
- `addr_t` becomes `uint64_t`. Audit callers — most should be transparent.
- Print format strings: `%x` → `%lx` for guest addresses. Many call sites; search-and-replace then audit.

`kernel/memory.{c,h}`:
- Replace `struct pgdir` with a 3-level walker. Sv39: VPN[2] (9 bits), VPN[1] (9 bits), VPN[0] (9 bits), offset (12 bits). 39-bit VA = 512 GB.
- Top level allocated eagerly: 512 entries × 8 bytes = 4 KB.
- Mid and leaf levels allocated lazily on first map.
- `pt_map`, `pt_unmap`, `pt_lookup`: rewrite for 3-level walk.
- mmap region picker: hand out from `0x3fff_ff_f000` downward (top of user Sv39).
- Stack region: top of user VA; grows down.

`emu/tlb.{h,c}`:
- `addr_t` widening through.
- `TLB_INDEX` hash: extend to mix more bits, e.g. `((addr >> 12) ^ (addr >> 24) ^ (addr >> 36)) & (TLB_SIZE - 1)`.
- `TLB_PAGE`: `(addr & ~0xfffull)`.

`fs/`: audit any guest-address persistence (e.g., `fs/fake-db.c`'s schema). Likely none, but verify.

`linux/`: the Linux-host kernel mode shim — leave for later (or drop if not targeted for RV64 in initial port).

**Dependencies**: Phases 0–3 done. (Strictly: Phase 3's exec.c uses 64-bit auxv already, so this is the canonicalising pass.)

**Verification**:
- Stress-test binary that mmaps 1 GB anonymous, touches every page sequentially, then munmaps. Exercises 3-level walker + TLB + cross-page logic.
- Stress-test that mmaps two regions far apart in VA (e.g. low + high half of Sv39) — exercises sparse upper levels.
- Re-run all Phase 2 and Phase 3 verification binaries; nothing should regress.

---

## Phase 5 — vDSO + dynamic linker bring-up

**Goal**: Run dynamically-linked RV64 binaries.

**Deliverables**:

`vdso_riscv/`:
- `vdso.S`: assembly trampolines.
  - `__vdso_rt_sigreturn`: `li a7, 139; ecall`.
  - `__vdso_clock_gettime`: initially same as a syscall path (`li a7, 113; ecall; ret`); optimise later.
  - `__vdso_gettimeofday`: similar.
  - `__vdso_getcpu`: similar.
- `vdso.lds`: linker script setting load address and symbol exports.
- `vdso.c`: Optional fast paths reading shared kernel time pages.
- `meson.build`: cross-compile to riscv64 ELF using `riscv64-linux-gnu-gcc` or clang `-target riscv64-linux`.
- Output: `libvdso.so.elf` blob, linked into `kernel/vdso.c`.

`kernel/vdso.c`: load the new blob, set auxv `AT_SYSINFO_EHDR` to its load address.

Dynamic linker bring-up:
- Provide `/lib/ld-musl-riscv64.so.1` in the test fakefs (from Alpine riscv64 musl package).
- Run dynamically-linked hello-world. Expect to fight: `getauxval`, TLS setup (`__set_thread_area`-equivalent on RV is just writing tp at clone time), library search paths, relocation processing.
- Each failure is a missing syscall, wrong auxv, or wrong stack layout. Iterate.

**Dependencies**: Phases 0–4 done. RISC-V cross-toolchain installed.

**Verification**:
- Dynamically-linked `hello.c` runs.
- `ldd hello` works (queries the dynamic linker, which itself exercises lots of syscalls).
- A C program calling `pthread_create` runs — exercises clone, futex, TLS.

---

## Phase 6 — Alpine bring-up

**Goal**: A real Alpine riscv64 rootfs runs busybox sh; `apk update` and `apk add` work.

**Deliverables**:

Rootfs:
- `tests/e2e/e2e.bash`: `ALPINE_IMAGE` URL → `https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/riscv64/alpine-minirootfs-3.21.x-riscv64.tar.gz` (use first Alpine version with first-class riscv64; check current state).
- `app/iSH.xcconfig`: `ROOTFS_URL` → riscv64 prebuilt apk-installable rootfs (likely needs to be built and hosted, since the iOS app expects a specific tarball format).
- `app/CurrentRoot.h`: bump `CURRENT_APK_VERSION_STRING` and `CURRENT_APK_VERSION` to the riscv64 Alpine release.

Long-tail debugging:
- Each Alpine binary exercises a different cluster of syscalls. Expected weak spots:
  - `futex`: WAIT/WAKE/REQUEUE/CMP_REQUEUE semantics. Must interact correctly with A-extension atomics.
  - `clone3`: newer flavour, may not be implemented.
  - `epoll`: `epoll_pwait2`, level vs edge triggered.
  - `clock_nanosleep`: spurious wakes, `TIMER_ABSTIME`.
  - `prlimit64`: per-process resource limits.
  - `getrandom`: with/without GRND_NONBLOCK.
  - `statx`: newer than legacy `stat`/`fstat`.
  - `pidfd_*`, `io_uring_*`: probably not needed for apk; stub with ENOSYS.
  - Signal masking edge cases around `rt_sigprocmask`, `rt_sigsuspend`, signal queueing.
  - `mmap` with `MAP_FIXED` at high addresses (validates Phase 4).
- RV64-specific: ensure A-extension atomics in glibc/musl don't generate spurious LR/SC failures across guest TLB invalidations or timer interrupts.

`tests/e2e/`:
- Replace x86 fixtures (`sse2/`, `fpu/`) with riscv64 equivalents or delete.
- New e2e tests: `apk_add` test that fetches and installs a small package.

**Dependencies**: Phases 0–5 done.

**Verification**:
- `./build-rv64/ish -f alpine-rv64 /bin/busybox sh -c "echo hello"`.
- `./build-rv64/ish -f alpine-rv64 /bin/busybox httpd` (servers exercise networking, fork, signals).
- `./build-rv64/ish -f alpine-rv64 /sbin/apk update`.
- `./build-rv64/ish -f alpine-rv64 /sbin/apk add hello && /usr/bin/hello` prints "Hello, world!".
- iOS app: same scenarios run on a real iPhone/iPad.

---

## Phase 7 — Cleanup

**Goal**: RV64GC is the only guest. x86 paths deleted.

**Deliverables**:
- Delete `emu/decode.h`, `emu/cpu.h`, `emu/fpu.c`, `emu/vec.c`, `emu/mmx.c`, `emu/float80.c`, `emu/float80-test.c`.
- Keep `emu/tlb.c`, `emu/tlb.h`, `emu/mmu.h`, `emu/interrupt.h` (guest-isa-agnostic).
- Delete `asbestos/asbestos.c`, `asbestos/gen.c`, `asbestos/helpers.c`, `asbestos/offsets.c`, `asbestos/gadgets-aarch64/`, `asbestos/gadgets-x86_64/`, `asbestos/gadgets-generic.h`, `asbestos/gen.h`, `asbestos/asbestos.h`, `asbestos/frame.h`.
- Delete `vdso/`.
- Rename `emu_riscv/` → `emu/` (or merge contents into the surviving `emu/`).
- Rename `asbestos_riscv/` → `asbestos/`.
- Rename `vdso_riscv/` → `vdso/`.
- Drop `gadgets-x86_64/` (we agreed ARM64 host only).
- `meson_options.txt`: drop `guest_isa` option.
- `meson.build`: drop the `if guest_isa == 'x86' …` gating; collapse to the riscv64 path.
- Drop x86 test fixtures: `tests/manual/*.{c,asm}` (any x86), `tests/e2e/sse2/`, `tests/e2e/fpu/`, `tests/e2e/instruction_audit/` (x86-specific).
- Delete `tools/ptraceomatic.c`, `tools/unicornomatic.c`, `tools/vdso-transplant*.c` (x86-specific debugging tools, no RV equivalent today).
- `README.md`: update to describe the RV64GC guest. Mention that this is a fork of the original x86 iSH.
- `README_*.md`: same in translations, or drop translations until contributors update them.

CI:
- `.github/workflows/ci.yml`: replace x86 cross-toolchain with `gcc-riscv64-linux-gnu` on Ubuntu.
- Remove the `engine=linux`/`engine=unicorn` jobs unless they've been ported.

**Dependencies**: Phase 6 stable.

**Verification**:
- All previous phase verification still passes after the rename.
- CI green.

---

## Inventory: files modified / created / deleted

### Created (new tree)
```
emu_riscv/
  cpu.h          [Phase 0 ✓]
  decode.h       [Phase 1]
  fp.c           [Phase 2]
  stub.c         [Phase 0 ✓; deleted in Phase 2]

asbestos_riscv/
  asbestos.h         [Phase 2; possibly shared]
  frame.h            [Phase 2; possibly shared]
  gen.h              [Phase 0 ✓ skeleton; Phase 1 fleshed out]
  gen.c              [Phase 0 ✓ skeleton; Phase 1+2 fleshed out]
  helpers.c          [Phase 2]
  offsets.c          [Phase 2]
  gadgets-generic.h  [Phase 2]
  gadgets-aarch64/
    gadgets.h        [Phase 2]
    entry.S          [Phase 0 ✓ skeleton; Phase 2 implemented]
    arith.S          [Phase 2]
    memory.S         [Phase 2]
    control.S        [Phase 2]
    mul.S            [Phase 2]
    atomic.S         [Phase 2]
    fp.S             [Phase 2]
    csr.S            [Phase 2]
    system.S         [Phase 2]

vdso_riscv/
  vdso.S, vdso.c, vdso.lds, meson.build, check-cc.sh  [Phase 5]

tools/
  rvdecode.c     [Phase 1]
```

### Modified

| File | Phases |
|---|---|
| `meson.build` | 0 ✓, 2, 5, 7 |
| `meson_options.txt` | 0 ✓, 7 (drop) |
| `misc.h` | 4 (`addr_t`) |
| `emu/tlb.{c,h}` | 4 (64-bit) |
| `kernel/exec.c` | 3 |
| `kernel/calls.c` | 3 (full table swap) |
| `kernel/signal.{c,h}` | 3 (sigframe rewrite) |
| `kernel/tls.c` | 3 (delete set_thread_area handler) |
| `kernel/uname.c` | 3 |
| `kernel/vdso.c` | 5 |
| `kernel/memory.{c,h}` | 4 (Sv39 walker) |
| `kernel/fork.c` | 3 (CLONE_SETTLS hook for tp) |
| `fs/proc/root.c` | 3 (cpuinfo) |
| `app/iSH.xcconfig` | 6 (ROOTFS_URL) |
| `app/CurrentRoot.{h,m}` | 6 (apk version) |
| `tests/e2e/e2e.bash` | 6 (ALPINE_IMAGE) |
| `.github/workflows/ci.yml` | 7 (RV cross-toolchain) |
| `README.md` | 7 |

### Deleted (Phase 7)

```
emu/cpu.h, emu/decode.h, emu/fpu.c, emu/vec.c, emu/mmx.c,
emu/float80.c, emu/float80-test.c
asbestos/  (entire directory)
vdso/      (entire directory; replaced by vdso_riscv/ → vdso/)
tests/manual/*.{c,asm} (x86 fixtures)
tests/e2e/sse2/, tests/e2e/fpu/, tests/e2e/instruction_audit/
tools/ptraceomatic.c, tools/unicornomatic.c, tools/vdso-transplant*.c,
tools/undefined-flags.c, tools/ptutil.c
```

---

## Dependency graph

```
Phase 0 ✓ — Scaffolding
  └─ Phase 1 — Decoder
       └─ Phase 2 — Gadget pool         ┐
       └─ Phase 3 — Kernel ABI          ┼── must converge before Phase 5
                                        │   (both touch cpu_state)
Phase 4 — Memory widening (parallel with 1–3 once cpu.h is stable;
          most disruptive when serialised after 3)
                                        │
       Phase 5 — vDSO + dyn linker  ────┘
            └─ Phase 6 — Alpine bring-up
                  └─ Phase 7 — Cleanup
```

Phase 1 and Phase 3 can run in parallel with Phase 2 once the cpu_state shape is settled. Phase 4 can run in parallel after Phase 3 sets the auxv layout. Phases 5–7 are strictly sequential.

---

## Open questions / decisions to revisit

- **FPU rounding strategy**: pick A/B/C in Phase 2 design.
- **Sv39 vs Sv48**: default Sv39; revisit if a real workload demands it.
- **Asbestos.c sharing**: at Phase 2, decide between (a) `#if`-include in `emu/cpu.h` to redirect to `emu_riscv/cpu.h`, or (b) full fork in `asbestos_riscv/asbestos.c`. Recommendation: (a).
- **Linux-host kernel mode (`linux/`)**: is a RV64 build of `engine=linux` in scope? If not, delete `linux/emu_*.c` paths in Phase 7.
- **Alpine packaging**: who hosts the `ROOTFS_URL` for the iOS app?  Need a build pipeline or sponsor.
- **CI host**: do we run RV64 binaries under qemu-user in CI for end-to-end verification, or only build-test?
- **Naming**: keep `iSH` branding, or fork-rename to `iSH-rv` / `RISH`? Affects bundle ID, app store identity.

---

## What's done so far

Phase 0 is committed-ready in `~/Git/ish-riscv` on branch `riscv-port`:

- `meson_options.txt`: `guest_isa` option added.
- `meson.build`: x86 vs riscv64 dispatch.
- `emu_riscv/cpu.h`: real RV64 cpu_state.
- `emu_riscv/{decode.h,stub.c}`: placeholders.
- `asbestos_riscv/{gen.h,gen.c,gadgets-aarch64/entry.S}`: Phase 2 skeletons.
- Both `meson setup build-x86` and `meson setup build-rv64 -Dguest_isa=riscv64` configure cleanly; both `ninja` builds succeed.

No commit has been created yet.

Additional progress in this worktree:

- `emu_riscv/decode.h`: implemented a standalone RV64 decoder with C-extension expansion and coverage for RV64I/M/A/F/D, Zicsr, and Zifencei decode classes.
- `tools/rvdecode.c`: added an ELF64 RISC-V disassembler harness.
- `tools/rvdecode-selftest.c`: added native decoder regression tests, including compressed and FP decode cases.
- `emu_riscv/interp.c`: added a functional RV64 interpreter behind `cpu_run_to_interrupt`/`cpu_poke`, covering integer/control/load-store, M-extension, basic A-extension LR/SC/AMO, CSR, FP load-store, and basic F/D arithmetic.
- `tools/rvinterp-selftest.c`: added an interpreter self-test that executes a guest loop, memory load/store, `ecall`, and double-precision FP through the public CPU API.
- `tools/rvrun.c`: added a flat-memory RV64 ELF runner that can execute static test ELFs using Linux generic `write`, `exit`, `exit_group`, and `brk` syscall numbers.
- `misc.h`, `emu/mmu.h`, `emu/tlb.{c,h}`: RV64 builds now use 64-bit guest addresses/pages and a wider TLB hash while preserving the x86 build.
- `kernel/calls.{c,h}`: split the syscall ABI enough for RV64 `ecall` dispatch to read `a7`/`a0`-`a5` and use Linux generic syscall numbers for the initial table.
- RV64 `ish` integration: `meson.build` now produces `build-rv64/ish`; the RV64 kernel path compiles with a minimal RV64 signal implementation, RV64 ELF header parsing/stack setup, RV64 `uname`/`cpuinfo` identity, and no-op interpreter-mode Asbestos cache hooks.
- Smoke verification: a hand-built static RV64 ELF mounted under a realfs root runs via `./build-rv64/ish -r <root> /bin/hello`, writes through guest syscall 64, and exits through guest syscall 93 with status 0.
