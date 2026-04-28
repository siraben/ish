#include <limits.h>
#include <math.h>
#include <string.h>

#include "emu_riscv/cpu.h"
#include "emu_riscv/decode.h"
#include "emu_riscv/atomic.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"

#define RV_MAX_INSNS_PER_SLICE 10000

static uint64_t load_reg(struct cpu_state *cpu, unsigned reg) {
    return reg == 0 ? 0 : cpu->x[reg];
}

static void store_reg(struct cpu_state *cpu, unsigned reg, uint64_t value) {
    if (reg != 0)
        cpu->x[reg] = value;
}

static int raise_interrupt(struct cpu_state *cpu, int interrupt) {
    cpu->trapno = interrupt;
    return interrupt;
}

static int segfault(struct cpu_state *cpu, struct tlb *tlb, bool write) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = write;
    return raise_interrupt(cpu, INT_GPF);
}

static bool read_guest(struct cpu_state *cpu, struct tlb *tlb, addr_t addr, void *out, unsigned size) {
    (void) cpu;
    return tlb_read(tlb, addr, out, size);
}

static bool write_guest(struct cpu_state *cpu, struct tlb *tlb, addr_t addr, const void *value, unsigned size) {
    cpu->reservation_valid = false;
    return tlb_write(tlb, addr, value, size);
}

static uint64_t sign_extend_width(uint64_t value, unsigned bits) {
    uint64_t sign = UINT64_C(1) << (bits - 1);
    return (value ^ sign) - sign;
}

static uint64_t div_signed(uint64_t a, uint64_t b) {
    if (b == 0)
        return UINT64_MAX;
    if (a == (uint64_t) INT64_MIN && b == UINT64_MAX)
        return a;
    return (uint64_t) ((int64_t) a / (int64_t) b);
}

static uint64_t rem_signed(uint64_t a, uint64_t b) {
    if (b == 0)
        return a;
    if (a == (uint64_t) INT64_MIN && b == UINT64_MAX)
        return 0;
    return (uint64_t) ((int64_t) a % (int64_t) b);
}

static uint32_t divw_signed(uint32_t a, uint32_t b) {
    if (b == 0)
        return UINT32_MAX;
    if (a == (uint32_t) INT32_MIN && b == UINT32_MAX)
        return a;
    return (uint32_t) ((int32_t) a / (int32_t) b);
}

static uint32_t remw_signed(uint32_t a, uint32_t b) {
    if (b == 0)
        return a;
    if (a == (uint32_t) INT32_MIN && b == UINT32_MAX)
        return 0;
    return (uint32_t) ((int32_t) a % (int32_t) b);
}

static bool read_csr(struct cpu_state *cpu, unsigned csr, uint64_t *value) {
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

static bool write_csr(struct cpu_state *cpu, unsigned csr, uint64_t value) {
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

static float freg_s(struct cpu_state *cpu, unsigned reg) {
    uint32_t bits = cpu->f[reg];
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double freg_d(struct cpu_state *cpu, unsigned reg) {
    double value;
    memcpy(&value, &cpu->f[reg], sizeof(value));
    return value;
}

static void store_freg_s(struct cpu_state *cpu, unsigned reg, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    cpu->f[reg] = UINT64_C(0xffffffff00000000) | bits;
}

static void store_freg_d(struct cpu_state *cpu, unsigned reg, double value) {
    memcpy(&cpu->f[reg], &value, sizeof(value));
}

static uint64_t fclass32(uint32_t bits) {
    uint32_t sign = bits >> 31;
    uint32_t exp = (bits >> 23) & 0xff;
    uint32_t frac = bits & 0x7fffff;
    if (exp == 0xff) {
        if (frac == 0)
            return sign ? (1u << 0) : (1u << 7);
        return (frac & 0x400000) ? (1u << 9) : (1u << 8);
    }
    if (exp == 0) {
        if (frac == 0)
            return sign ? (1u << 3) : (1u << 4);
        return sign ? (1u << 2) : (1u << 5);
    }
    return sign ? (1u << 1) : (1u << 6);
}

static uint64_t fclass64(uint64_t bits) {
    uint64_t sign = bits >> 63;
    uint64_t exp = (bits >> 52) & 0x7ff;
    uint64_t frac = bits & UINT64_C(0x000fffffffffffff);
    if (exp == 0x7ff) {
        if (frac == 0)
            return sign ? (1u << 0) : (1u << 7);
        return (frac & UINT64_C(0x0008000000000000)) ? (1u << 9) : (1u << 8);
    }
    if (exp == 0) {
        if (frac == 0)
            return sign ? (1u << 3) : (1u << 4);
        return sign ? (1u << 2) : (1u << 5);
    }
    return sign ? (1u << 1) : (1u << 6);
}

static uint64_t fp_to_int_s(float value, unsigned kind) {
    switch (kind) {
    case 0: return (uint64_t) (int64_t) (int32_t) value;
    case 1: return (uint64_t) (uint32_t) value;
    case 2: return (uint64_t) (int64_t) value;
    case 3: return (uint64_t) value;
    default: return 0;
    }
}

static uint64_t fp_to_int_d(double value, unsigned kind) {
    switch (kind) {
    case 0: return (uint64_t) (int64_t) (int32_t) value;
    case 1: return (uint64_t) (uint32_t) value;
    case 2: return (uint64_t) (int64_t) value;
    case 3: return (uint64_t) value;
    default: return 0;
    }
}

static float int_to_fp_s(uint64_t value, unsigned kind) {
    switch (kind) {
    case 0: return (float) (int32_t) value;
    case 1: return (float) (uint32_t) value;
    case 2: return (float) (int64_t) value;
    case 3: return (float) value;
    default: return 0.0f;
    }
}

static double int_to_fp_d(uint64_t value, unsigned kind) {
    switch (kind) {
    case 0: return (double) (int32_t) value;
    case 1: return (double) (uint32_t) value;
    case 2: return (double) (int64_t) value;
    case 3: return (double) value;
    default: return 0.0;
    }
}

static bool exec_amo(struct cpu_state *cpu, struct tlb *tlb, struct rv_insn *insn) {
    unsigned funct5 = rv_bits(insn->expanded, 31, 27);
    unsigned width = insn->funct3 == 2 ? 4 : insn->funct3 == 3 ? 8 : 0;
    if (width == 0)
        return false;

    addr_t addr = load_reg(cpu, insn->rs1);
    if (PGOFFSET(addr) > PAGE_SIZE - width)
        return false;

    rv_atomic_lock();

    uint64_t loaded = 0;
    if (!rv_atomic_load(cpu, tlb, addr, &loaded, width, true)) {
        rv_atomic_unlock();
        return false;
    }
    if (width == 4)
        loaded = sign_extend_width((uint32_t) loaded, 32);

    if (funct5 == 0x02) {
        cpu->reservation_addr = addr;
        rv_reservation_set(cpu, addr, loaded);
        cpu->reservation_valid = true;
        store_reg(cpu, insn->rd, loaded);
        rv_atomic_unlock();
        return true;
    }

    uint64_t result = load_reg(cpu, insn->rs2);
    bool should_store = true;
    uint64_t rd_value = loaded;
    bool write_rd = insn->rd != 0;
    if (funct5 == 0x03) {
        should_store = cpu->reservation_valid && cpu->reservation_addr == addr &&
            rv_reservation_matches(cpu, addr, loaded);
        result = load_reg(cpu, insn->rs2);
        cpu->reservation_valid = false;
        rd_value = should_store ? 0 : 1;
    } else {
        uint64_t rs2 = load_reg(cpu, insn->rs2);
        switch (funct5) {
        case 0x00: result = loaded + rs2; break;
        case 0x01: result = rs2; break;
        case 0x04: result = loaded ^ rs2; break;
        case 0x08: result = loaded | rs2; break;
        case 0x0c: result = loaded & rs2; break;
        case 0x10: result = (int64_t) loaded < (int64_t) rs2 ? loaded : rs2; break;
        case 0x14: result = (int64_t) loaded > (int64_t) rs2 ? loaded : rs2; break;
        case 0x18: result = loaded < rs2 ? loaded : rs2; break;
        case 0x1c: result = loaded > rs2 ? loaded : rs2; break;
        default:
            rv_atomic_unlock();
            return false;
        }
    }

    if (should_store) {
        cpu->reservation_valid = false;
        if (!rv_atomic_store(cpu, tlb, addr, result, width)) {
            rv_atomic_unlock();
            return false;
        }
    }
    if (write_rd)
        store_reg(cpu, insn->rd, rd_value);
    rv_atomic_unlock();
    return true;
}

static int exec_one(struct cpu_state *cpu, struct tlb *tlb) {
    uint8_t bytes[4] = {};
    if (!tlb_read(tlb, cpu->pc, bytes, sizeof(uint16_t)))
        return segfault(cpu, tlb, false);

    uint16_t h = (uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8);
    size_t length = (h & 3) == 3 ? sizeof(uint32_t) : sizeof(uint16_t);
    if (length == sizeof(uint32_t) &&
            !tlb_read(tlb, cpu->pc + sizeof(uint16_t), bytes + sizeof(uint16_t), sizeof(uint16_t)))
        return segfault(cpu, tlb, false);

    struct rv_insn insn;
    if (!rv_decode(bytes, length, &insn))
        return raise_interrupt(cpu, INT_UNDEFINED);

    addr_t next_pc = cpu->pc + insn.length;
    uint64_t rs1 = load_reg(cpu, insn.rs1);
    uint64_t rs2 = load_reg(cpu, insn.rs2);
    uint64_t result;

    switch (insn.op) {
    case RV_OP_LUI:
        store_reg(cpu, insn.rd, insn.imm);
        break;
    case RV_OP_AUIPC:
        store_reg(cpu, insn.rd, cpu->pc + insn.imm);
        break;
    case RV_OP_JAL:
        store_reg(cpu, insn.rd, next_pc);
        next_pc = cpu->pc + insn.imm;
        break;
    case RV_OP_JALR:
        store_reg(cpu, insn.rd, next_pc);
        next_pc = (rs1 + insn.imm) & ~UINT64_C(1);
        break;
    case RV_OP_BRANCH: {
        bool take = false;
        switch (insn.funct3) {
        case 0: take = rs1 == rs2; break;
        case 1: take = rs1 != rs2; break;
        case 4: take = (int64_t) rs1 < (int64_t) rs2; break;
        case 5: take = (int64_t) rs1 >= (int64_t) rs2; break;
        case 6: take = rs1 < rs2; break;
        case 7: take = rs1 >= rs2; break;
        default: return raise_interrupt(cpu, INT_UNDEFINED);
        }
        if (take)
            next_pc = cpu->pc + insn.imm;
        break;
    }
    case RV_OP_LOAD: {
        addr_t addr = rs1 + insn.imm;
        uint64_t value = 0;
        unsigned size;
        switch (insn.funct3) {
        case 0: size = 1; break;
        case 1: size = 2; break;
        case 2: size = 4; break;
        case 3: size = 8; break;
        case 4: size = 1; break;
        case 5: size = 2; break;
        case 6: size = 4; break;
        default: return raise_interrupt(cpu, INT_UNDEFINED);
        }
        if (!read_guest(cpu, tlb, addr, &value, size))
            return segfault(cpu, tlb, false);
        if (insn.funct3 <= 3)
            value = sign_extend_width(value, size * CHAR_BIT);
        store_reg(cpu, insn.rd, value);
        break;
    }
    case RV_OP_STORE: {
        addr_t addr = rs1 + insn.imm;
        unsigned size;
        switch (insn.funct3) {
        case 0: size = 1; break;
        case 1: size = 2; break;
        case 2: size = 4; break;
        case 3: size = 8; break;
        default: return raise_interrupt(cpu, INT_UNDEFINED);
        }
        if (!write_guest(cpu, tlb, addr, &rs2, size))
            return segfault(cpu, tlb, true);
        break;
    }
    case RV_OP_LOAD_FP: {
        addr_t addr = rs1 + insn.imm;
        if (insn.funct3 == 2) {
            uint32_t value;
            if (!read_guest(cpu, tlb, addr, &value, sizeof(value)))
                return segfault(cpu, tlb, false);
            cpu->f[insn.rd] = UINT64_C(0xffffffff00000000) | value;
        } else if (insn.funct3 == 3) {
            if (!read_guest(cpu, tlb, addr, &cpu->f[insn.rd], sizeof(uint64_t)))
                return segfault(cpu, tlb, false);
        } else {
            return raise_interrupt(cpu, INT_UNDEFINED);
        }
        break;
    }
    case RV_OP_STORE_FP: {
        addr_t addr = rs1 + insn.imm;
        if (insn.funct3 == 2) {
            uint32_t value = cpu->f[insn.rs2];
            if (!write_guest(cpu, tlb, addr, &value, sizeof(value)))
                return segfault(cpu, tlb, true);
        } else if (insn.funct3 == 3) {
            if (!write_guest(cpu, tlb, addr, &cpu->f[insn.rs2], sizeof(uint64_t)))
                return segfault(cpu, tlb, true);
        } else {
            return raise_interrupt(cpu, INT_UNDEFINED);
        }
        break;
    }
    case RV_OP_OP_IMM:
        switch (insn.funct3) {
        case 0: result = rs1 + insn.imm; break;
        case 1: result = rs1 << rv_bits(insn.expanded, 25, 20); break;
        case 2: result = (int64_t) rs1 < insn.imm; break;
        case 3: result = rs1 < (uint64_t) insn.imm; break;
        case 4: result = rs1 ^ insn.imm; break;
        case 5:
            if (rv_bits(insn.expanded, 31, 26) == 0x10)
                result = (uint64_t) ((int64_t) rs1 >> rv_bits(insn.expanded, 25, 20));
            else
                result = rs1 >> rv_bits(insn.expanded, 25, 20);
            break;
        case 6: result = rs1 | insn.imm; break;
        case 7: result = rs1 & insn.imm; break;
        default: return raise_interrupt(cpu, INT_UNDEFINED);
        }
        store_reg(cpu, insn.rd, result);
        break;
    case RV_OP_OP_IMM_32:
        switch (insn.funct3) {
        case 0: result = (int32_t) ((uint32_t) rs1 + (uint32_t) insn.imm); break;
        case 1: result = (int32_t) ((uint32_t) rs1 << rv_bits(insn.expanded, 24, 20)); break;
        case 5:
            if (insn.funct7 == 0x20)
                result = (int32_t) rs1 >> rv_bits(insn.expanded, 24, 20);
            else
                result = (int32_t) ((uint32_t) rs1 >> rv_bits(insn.expanded, 24, 20));
            break;
        default: return raise_interrupt(cpu, INT_UNDEFINED);
        }
        store_reg(cpu, insn.rd, result);
        break;
    case RV_OP_OP:
        if (insn.funct7 == 0x01) {
            switch (insn.funct3) {
            case 0: result = rs1 * rs2; break;
            case 1: result = (uint64_t) (((__int128) (int64_t) rs1 * (int64_t) rs2) >> 64); break;
            case 2: result = (uint64_t) (((__int128) (int64_t) rs1 * (uint64_t) rs2) >> 64); break;
            case 3: result = (uint64_t) (((unsigned __int128) rs1 * rs2) >> 64); break;
            case 4: result = div_signed(rs1, rs2); break;
            case 5: result = rs2 == 0 ? UINT64_MAX : rs1 / rs2; break;
            case 6: result = rem_signed(rs1, rs2); break;
            case 7: result = rs2 == 0 ? rs1 : rs1 % rs2; break;
            default: return raise_interrupt(cpu, INT_UNDEFINED);
            }
        } else {
            switch (insn.funct3) {
            case 0: result = insn.funct7 == 0x20 ? rs1 - rs2 : rs1 + rs2; break;
            case 1: result = rs1 << (rs2 & 0x3f); break;
            case 2: result = (int64_t) rs1 < (int64_t) rs2; break;
            case 3: result = rs1 < rs2; break;
            case 4: result = rs1 ^ rs2; break;
            case 5: result = insn.funct7 == 0x20 ? (uint64_t) ((int64_t) rs1 >> (rs2 & 0x3f)) : rs1 >> (rs2 & 0x3f); break;
            case 6: result = rs1 | rs2; break;
            case 7: result = rs1 & rs2; break;
            default: return raise_interrupt(cpu, INT_UNDEFINED);
            }
        }
        store_reg(cpu, insn.rd, result);
        break;
    case RV_OP_OP_32:
        if (insn.funct7 == 0x01) {
            uint32_t a = rs1;
            uint32_t b = rs2;
            switch (insn.funct3) {
            case 0: result = (int32_t) (a * b); break;
            case 4: result = (int32_t) divw_signed(a, b); break;
            case 5: result = (int32_t) (b == 0 ? UINT32_MAX : a / b); break;
            case 6: result = (int32_t) remw_signed(a, b); break;
            case 7: result = (int32_t) (b == 0 ? a : a % b); break;
            default: return raise_interrupt(cpu, INT_UNDEFINED);
            }
        } else {
            switch (insn.funct3) {
            case 0: result = (int32_t) (insn.funct7 == 0x20 ? (uint32_t) rs1 - (uint32_t) rs2 : (uint32_t) rs1 + (uint32_t) rs2); break;
            case 1: result = (int32_t) ((uint32_t) rs1 << (rs2 & 0x1f)); break;
            case 5: result = insn.funct7 == 0x20 ? (int32_t) rs1 >> (rs2 & 0x1f) : (int32_t) ((uint32_t) rs1 >> (rs2 & 0x1f)); break;
            default: return raise_interrupt(cpu, INT_UNDEFINED);
            }
        }
        store_reg(cpu, insn.rd, result);
        break;
    case RV_OP_MISC_MEM:
        break;
    case RV_OP_SYSTEM:
        if (insn.funct3 == 0) {
            cpu->pc = next_pc;
            if (insn.imm == 0)
                return raise_interrupt(cpu, INT_SYSCALL);
            if (insn.imm == 1)
                return raise_interrupt(cpu, INT_BREAKPOINT);
            return raise_interrupt(cpu, INT_UNDEFINED);
        } else {
            unsigned csr = rv_bits(insn.expanded, 31, 20);
            uint64_t old;
            uint64_t csr_arg = insn.funct3 >= 5 ? insn.rs1 : rs1;
            if (!read_csr(cpu, csr, &old))
                return raise_interrupt(cpu, INT_UNDEFINED);
            store_reg(cpu, insn.rd, old);
            switch (insn.funct3) {
            case 1: case 5:
                if (!write_csr(cpu, csr, csr_arg))
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            case 2: case 6:
                if (csr_arg != 0 && !write_csr(cpu, csr, old | csr_arg))
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            case 3: case 7:
                if (csr_arg != 0 && !write_csr(cpu, csr, old & ~csr_arg))
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            default:
                return raise_interrupt(cpu, INT_UNDEFINED);
            }
        }
        break;
    case RV_OP_AMO:
        if (!exec_amo(cpu, tlb, &insn))
            return segfault(cpu, tlb, true);
        break;
    case RV_OP_OP_FP: {
        unsigned funct5 = rv_bits(insn.expanded, 31, 27);
        unsigned fmt = rv_bits(insn.expanded, 26, 25);
        if (fmt == 0) {
            float a = freg_s(cpu, insn.rs1);
            float b = freg_s(cpu, insn.rs2);
            switch (funct5) {
            case 0x00: store_freg_s(cpu, insn.rd, a + b); break;
            case 0x01: store_freg_s(cpu, insn.rd, a - b); break;
            case 0x02: store_freg_s(cpu, insn.rd, a * b); break;
            case 0x03: store_freg_s(cpu, insn.rd, a / b); break;
            case 0x0b: store_freg_s(cpu, insn.rd, sqrtf(a)); break;
            case 0x04: {
                uint32_t ia = cpu->f[insn.rs1];
                uint32_t ib = cpu->f[insn.rs2];
                uint32_t sign = ib & 0x80000000u;
                if (insn.funct3 == 1)
                    sign ^= 0x80000000u;
                if (insn.funct3 == 2)
                    sign ^= ia & 0x80000000u;
                cpu->f[insn.rd] = UINT64_C(0xffffffff00000000) | ((ia & 0x7fffffffu) | sign);
                break;
            }
            case 0x05:
                if (insn.funct3 == 0)
                    store_freg_s(cpu, insn.rd, fminf(a, b));
                else if (insn.funct3 == 1)
                    store_freg_s(cpu, insn.rd, fmaxf(a, b));
                else
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            case 0x08:
                if (insn.rs2 != 1)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                store_freg_s(cpu, insn.rd, (float) freg_d(cpu, insn.rs1));
                break;
            case 0x14:
                if (insn.funct3 == 0)
                    store_reg(cpu, insn.rd, a <= b);
                else if (insn.funct3 == 1)
                    store_reg(cpu, insn.rd, a < b);
                else if (insn.funct3 == 2)
                    store_reg(cpu, insn.rd, a == b);
                else
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            case 0x18:
                if (insn.rs2 > 3)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                store_reg(cpu, insn.rd, fp_to_int_s(a, insn.rs2));
                break;
            case 0x1a:
                if (insn.rs2 > 3)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                store_freg_s(cpu, insn.rd, int_to_fp_s(rs1, insn.rs2));
                break;
            case 0x1c:
                if (insn.funct3 == 0) {
                    store_reg(cpu, insn.rd, (int32_t) (uint32_t) cpu->f[insn.rs1]);
                } else if (insn.funct3 == 1) {
                    uint64_t raw = cpu->f[insn.rs1];
                    uint64_t boxed = raw >> 32;
                    store_reg(cpu, insn.rd, boxed == UINT32_MAX ? fclass32(raw) : (1u << 9));
                } else {
                    return raise_interrupt(cpu, INT_UNDEFINED);
                }
                break;
            case 0x1e:
                if (insn.funct3 != 0)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                cpu->f[insn.rd] = UINT64_C(0xffffffff00000000) | (uint32_t) rs1;
                break;
            default:
                return raise_interrupt(cpu, INT_UNDEFINED);
            }
        } else if (fmt == 1) {
            double a = freg_d(cpu, insn.rs1);
            double b = freg_d(cpu, insn.rs2);
            switch (funct5) {
            case 0x00: store_freg_d(cpu, insn.rd, a + b); break;
            case 0x01: store_freg_d(cpu, insn.rd, a - b); break;
            case 0x02: store_freg_d(cpu, insn.rd, a * b); break;
            case 0x03: store_freg_d(cpu, insn.rd, a / b); break;
            case 0x0b: store_freg_d(cpu, insn.rd, sqrt(a)); break;
            case 0x04: {
                uint64_t ia = cpu->f[insn.rs1];
                uint64_t ib = cpu->f[insn.rs2];
                uint64_t sign = ib & UINT64_C(0x8000000000000000);
                if (insn.funct3 == 1)
                    sign ^= UINT64_C(0x8000000000000000);
                if (insn.funct3 == 2)
                    sign ^= ia & UINT64_C(0x8000000000000000);
                cpu->f[insn.rd] = (ia & UINT64_C(0x7fffffffffffffff)) | sign;
                break;
            }
            case 0x05:
                if (insn.funct3 == 0)
                    store_freg_d(cpu, insn.rd, fmin(a, b));
                else if (insn.funct3 == 1)
                    store_freg_d(cpu, insn.rd, fmax(a, b));
                else
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            case 0x08:
                if (insn.rs2 != 0)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                store_freg_d(cpu, insn.rd, (double) freg_s(cpu, insn.rs1));
                break;
            case 0x14:
                if (insn.funct3 == 0)
                    store_reg(cpu, insn.rd, a <= b);
                else if (insn.funct3 == 1)
                    store_reg(cpu, insn.rd, a < b);
                else if (insn.funct3 == 2)
                    store_reg(cpu, insn.rd, a == b);
                else
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            case 0x18:
            case 0x19:
                if (insn.rs2 > 3)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                store_reg(cpu, insn.rd, fp_to_int_d(a, insn.rs2));
                break;
            case 0x1a:
            case 0x1b:
                if (insn.rs2 > 3)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                store_freg_d(cpu, insn.rd, int_to_fp_d(rs1, insn.rs2));
                break;
            case 0x1c:
                if (insn.funct3 == 0)
                    store_reg(cpu, insn.rd, cpu->f[insn.rs1]);
                else if (insn.funct3 == 1)
                    store_reg(cpu, insn.rd, fclass64(cpu->f[insn.rs1]));
                else
                    return raise_interrupt(cpu, INT_UNDEFINED);
                break;
            case 0x1e:
                if (insn.funct3 != 0)
                    return raise_interrupt(cpu, INT_UNDEFINED);
                cpu->f[insn.rd] = rs1;
                break;
            default:
                return raise_interrupt(cpu, INT_UNDEFINED);
            }
        } else {
            return raise_interrupt(cpu, INT_UNDEFINED);
        }
        break;
    }
    case RV_OP_MADD:
    case RV_OP_MSUB:
    case RV_OP_NMSUB:
    case RV_OP_NMADD: {
        unsigned fmt = rv_bits(insn.expanded, 26, 25);
        if (fmt == 0) {
            float a = freg_s(cpu, insn.rs1);
            float b = freg_s(cpu, insn.rs2);
            float c = freg_s(cpu, insn.rs3);
            if (insn.op == RV_OP_MADD)
                store_freg_s(cpu, insn.rd, a * b + c);
            else if (insn.op == RV_OP_MSUB)
                store_freg_s(cpu, insn.rd, a * b - c);
            else if (insn.op == RV_OP_NMSUB)
                store_freg_s(cpu, insn.rd, -(a * b) + c);
            else
                store_freg_s(cpu, insn.rd, -(a * b) - c);
        } else if (fmt == 1) {
            double a = freg_d(cpu, insn.rs1);
            double b = freg_d(cpu, insn.rs2);
            double c = freg_d(cpu, insn.rs3);
            if (insn.op == RV_OP_MADD)
                store_freg_d(cpu, insn.rd, a * b + c);
            else if (insn.op == RV_OP_MSUB)
                store_freg_d(cpu, insn.rd, a * b - c);
            else if (insn.op == RV_OP_NMSUB)
                store_freg_d(cpu, insn.rd, -(a * b) + c);
            else
                store_freg_d(cpu, insn.rd, -(a * b) - c);
        } else {
            return raise_interrupt(cpu, INT_UNDEFINED);
        }
        break;
    }
    default:
        return raise_interrupt(cpu, INT_UNDEFINED);
    }

    cpu->pc = next_pc;
    cpu->x[0] = 0;
    cpu->cycle++;
    return INT_NONE;
}

#if ENGINE_ASBESTOS
int rv_exec_one_gadget(struct cpu_state *cpu, struct tlb *tlb) {
    return exec_one(cpu, tlb);
}
#else
int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->poked_ptr = &cpu->_poked;
    cpu->_poked = false;
    for (unsigned i = 0; i < RV_MAX_INSNS_PER_SLICE; i++) {
        if (cpu->_poked)
            return raise_interrupt(cpu, INT_TIMER);
        int interrupt = exec_one(cpu, tlb);
        if (interrupt != INT_NONE)
            return interrupt;
        if (cpu->tf)
            return raise_interrupt(cpu, INT_DEBUG);
    }
    return raise_interrupt(cpu, INT_TIMER);
}

void cpu_poke(struct cpu_state *cpu) {
    if (cpu->poked_ptr != NULL)
        *cpu->poked_ptr = true;
}
#endif
