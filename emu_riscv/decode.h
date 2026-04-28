#ifndef EMU_RISCV_DECODE_H
#define EMU_RISCV_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum rv_decode_op {
    RV_OP_ILLEGAL,
    RV_OP_LUI, RV_OP_AUIPC, RV_OP_JAL, RV_OP_JALR,
    RV_OP_BRANCH, RV_OP_LOAD, RV_OP_STORE,
    RV_OP_OP_IMM, RV_OP_OP_IMM_32, RV_OP_OP, RV_OP_OP_32,
    RV_OP_MISC_MEM, RV_OP_SYSTEM,
    RV_OP_LOAD_FP, RV_OP_STORE_FP, RV_OP_OP_FP,
    RV_OP_MADD, RV_OP_MSUB, RV_OP_NMSUB, RV_OP_NMADD,
    RV_OP_AMO,
};

struct rv_insn {
    enum rv_decode_op op;
    uint32_t raw;
    uint32_t expanded;
    uint8_t length;
    uint8_t rd, rs1, rs2, rs3;
    uint8_t funct3, funct7, rm;
    int64_t imm;
    bool compressed;
    bool illegal;
    char mnemonic[16];
    char operands[80];
};

static inline uint32_t rv_bits(uint32_t value, unsigned hi, unsigned lo) {
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1);
}

static inline int64_t rv_sext(uint64_t value, unsigned bits) {
    uint64_t sign = 1ull << (bits - 1);
    return (int64_t) ((value ^ sign) - sign);
}

static inline int32_t rv_i_imm(uint32_t insn) {
    return (int32_t) rv_sext(rv_bits(insn, 31, 20), 12);
}

static inline int32_t rv_s_imm(uint32_t insn) {
    return (int32_t) rv_sext((rv_bits(insn, 31, 25) << 5) | rv_bits(insn, 11, 7), 12);
}

static inline int32_t rv_b_imm(uint32_t insn) {
    uint32_t imm = (rv_bits(insn, 31, 31) << 12) |
        (rv_bits(insn, 7, 7) << 11) |
        (rv_bits(insn, 30, 25) << 5) |
        (rv_bits(insn, 11, 8) << 1);
    return (int32_t) rv_sext(imm, 13);
}

static inline int32_t rv_u_imm(uint32_t insn) {
    return (int32_t) (insn & 0xfffff000u);
}

static inline int32_t rv_j_imm(uint32_t insn) {
    uint32_t imm = (rv_bits(insn, 31, 31) << 20) |
        (rv_bits(insn, 19, 12) << 12) |
        (rv_bits(insn, 20, 20) << 11) |
        (rv_bits(insn, 30, 21) << 1);
    return (int32_t) rv_sext(imm, 21);
}

static inline uint32_t rv_encode_r(unsigned funct7, unsigned rs2, unsigned rs1,
        unsigned funct3, unsigned rd, unsigned opcode) {
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
        (funct3 << 12) | (rd << 7) | opcode;
}

static inline uint32_t rv_encode_i(int32_t imm, unsigned rs1, unsigned funct3,
        unsigned rd, unsigned opcode) {
    return (((uint32_t) imm & 0xfff) << 20) | (rs1 << 15) |
        (funct3 << 12) | (rd << 7) | opcode;
}

static inline uint32_t rv_encode_s(int32_t imm, unsigned rs2, unsigned rs1,
        unsigned funct3, unsigned opcode) {
    uint32_t uimm = (uint32_t) imm & 0xfff;
    return (rv_bits(uimm, 11, 5) << 25) | (rs2 << 20) | (rs1 << 15) |
        (funct3 << 12) | (rv_bits(uimm, 4, 0) << 7) | opcode;
}

static inline uint32_t rv_encode_b(int32_t imm, unsigned rs2, unsigned rs1,
        unsigned funct3, unsigned opcode) {
    uint32_t uimm = (uint32_t) imm & 0x1fff;
    return (rv_bits(uimm, 12, 12) << 31) | (rv_bits(uimm, 10, 5) << 25) |
        (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
        (rv_bits(uimm, 4, 1) << 8) | (rv_bits(uimm, 11, 11) << 7) | opcode;
}

static inline uint32_t rv_encode_u(int32_t imm, unsigned rd, unsigned opcode) {
    return ((uint32_t) imm & 0xfffff000u) | (rd << 7) | opcode;
}

static inline uint32_t rv_encode_j(int32_t imm, unsigned rd, unsigned opcode) {
    uint32_t uimm = (uint32_t) imm & 0x1fffff;
    return (rv_bits(uimm, 20, 20) << 31) | (rv_bits(uimm, 10, 1) << 21) |
        (rv_bits(uimm, 11, 11) << 20) | (rv_bits(uimm, 19, 12) << 12) |
        (rd << 7) | opcode;
}

static inline const char *rv_xreg_name(unsigned reg) {
    static const char *names[32] = {
        "zero","ra","sp","gp","tp","t0","t1","t2",
        "s0","s1","a0","a1","a2","a3","a4","a5",
        "a6","a7","s2","s3","s4","s5","s6","s7",
        "s8","s9","s10","s11","t3","t4","t5","t6",
    };
    return reg < 32 ? names[reg] : "?";
}

static inline const char *rv_freg_name(unsigned reg) {
    static const char *names[32] = {
        "ft0","ft1","ft2","ft3","ft4","ft5","ft6","ft7",
        "fs0","fs1","fa0","fa1","fa2","fa3","fa4","fa5",
        "fa6","fa7","fs2","fs3","fs4","fs5","fs6","fs7",
        "fs8","fs9","fs10","fs11","ft8","ft9","ft10","ft11",
    };
    return reg < 32 ? names[reg] : "?";
}

static inline const char *rv_csr_name(unsigned csr) {
    switch (csr) {
    case 0x001: return "fflags";
    case 0x002: return "frm";
    case 0x003: return "fcsr";
    case 0xc00: return "cycle";
    case 0xc01: return "time";
    case 0xc02: return "instret";
    case 0xc80: return "cycleh";
    case 0xc81: return "timeh";
    case 0xc82: return "instreth";
    default: return NULL;
    }
}

static inline void rv_set_illegal(struct rv_insn *out, uint32_t raw, uint8_t length) {
    memset(out, 0, sizeof(*out));
    out->op = RV_OP_ILLEGAL;
    out->raw = raw;
    out->expanded = raw;
    out->length = length;
    out->illegal = true;
    snprintf(out->mnemonic, sizeof(out->mnemonic), ".%u", length == 2 ? 2u : 4u);
    snprintf(out->operands, sizeof(out->operands), "0x%0*x", length == 2 ? 4 : 8, raw);
}

static inline int32_t rv_cj_imm(uint16_t h) {
    uint32_t imm = (rv_bits(h, 12, 12) << 11) |
        (rv_bits(h, 8, 8) << 10) |
        (rv_bits(h, 10, 9) << 8) |
        (rv_bits(h, 6, 6) << 7) |
        (rv_bits(h, 7, 7) << 6) |
        (rv_bits(h, 2, 2) << 5) |
        (rv_bits(h, 11, 11) << 4) |
        (rv_bits(h, 5, 3) << 1);
    return (int32_t) rv_sext(imm, 12);
}

static inline int32_t rv_cb_imm(uint16_t h) {
    uint32_t imm = (rv_bits(h, 12, 12) << 8) |
        (rv_bits(h, 6, 5) << 6) |
        (rv_bits(h, 2, 2) << 5) |
        (rv_bits(h, 11, 10) << 3) |
        (rv_bits(h, 4, 3) << 1);
    return (int32_t) rv_sext(imm, 9);
}

static inline int32_t rv_ci_imm(uint16_t h) {
    return (int32_t) rv_sext((rv_bits(h, 12, 12) << 5) | rv_bits(h, 6, 2), 6);
}

static inline bool rv_expand_compressed(uint16_t h, uint32_t *expanded) {
    unsigned quadrant = rv_bits(h, 1, 0);
    unsigned funct3 = rv_bits(h, 15, 13);
    unsigned rd = rv_bits(h, 11, 7);
    unsigned rs2 = rv_bits(h, 6, 2);
    unsigned crd = 8 + rv_bits(h, 4, 2);
    unsigned crs1 = 8 + rv_bits(h, 9, 7);
    unsigned crs2 = 8 + rv_bits(h, 4, 2);

    if (quadrant == 0) {
        switch (funct3) {
        case 0: { // c.addi4spn
            uint32_t imm = (rv_bits(h, 10, 7) << 6) | (rv_bits(h, 12, 11) << 4) |
                (rv_bits(h, 5, 5) << 3) | (rv_bits(h, 6, 6) << 2);
            if (imm == 0) return false;
            *expanded = rv_encode_i((int32_t) imm, 2, 0, crd, 0x13);
            return true;
        }
        case 1: { // c.fld
            uint32_t imm = (rv_bits(h, 6, 5) << 6) | (rv_bits(h, 12, 10) << 3);
            *expanded = rv_encode_i((int32_t) imm, crs1, 3, crd, 0x07);
            return true;
        }
        case 2: { // c.lw
            uint32_t imm = (rv_bits(h, 5, 5) << 6) | (rv_bits(h, 12, 10) << 3) |
                (rv_bits(h, 6, 6) << 2);
            *expanded = rv_encode_i((int32_t) imm, crs1, 2, crd, 0x03);
            return true;
        }
        case 3: { // c.ld
            uint32_t imm = (rv_bits(h, 6, 5) << 6) | (rv_bits(h, 12, 10) << 3);
            *expanded = rv_encode_i((int32_t) imm, crs1, 3, crd, 0x03);
            return true;
        }
        case 5: { // c.fsd
            uint32_t imm = (rv_bits(h, 6, 5) << 6) | (rv_bits(h, 12, 10) << 3);
            *expanded = rv_encode_s((int32_t) imm, crs2, crs1, 3, 0x27);
            return true;
        }
        case 6: { // c.sw
            uint32_t imm = (rv_bits(h, 5, 5) << 6) | (rv_bits(h, 12, 10) << 3) |
                (rv_bits(h, 6, 6) << 2);
            *expanded = rv_encode_s((int32_t) imm, crs2, crs1, 2, 0x23);
            return true;
        }
        case 7: { // c.sd
            uint32_t imm = (rv_bits(h, 6, 5) << 6) | (rv_bits(h, 12, 10) << 3);
            *expanded = rv_encode_s((int32_t) imm, crs2, crs1, 3, 0x23);
            return true;
        }
        default:
            return false;
        }
    }

    if (quadrant == 1) {
        switch (funct3) {
        case 0: // c.nop/c.addi
            *expanded = rv_encode_i(rv_ci_imm(h), rd, 0, rd, 0x13);
            return true;
        case 1: // c.addiw
            if (rd == 0) return false;
            *expanded = rv_encode_i(rv_ci_imm(h), rd, 0, rd, 0x1b);
            return true;
        case 2: // c.li
            *expanded = rv_encode_i(rv_ci_imm(h), 0, 0, rd, 0x13);
            return true;
        case 3: { // c.addi16sp/c.lui
            int32_t imm;
            if (rd == 2) {
                imm = (int32_t) rv_sext((rv_bits(h, 12, 12) << 9) |
                    (rv_bits(h, 4, 3) << 7) | (rv_bits(h, 5, 5) << 6) |
                    (rv_bits(h, 2, 2) << 5) | (rv_bits(h, 6, 6) << 4), 10);
                if (imm == 0) return false;
                *expanded = rv_encode_i(imm, 2, 0, 2, 0x13);
                return true;
            }
            if (rd == 0) return false;
            imm = (int32_t) rv_sext((rv_bits(h, 12, 12) << 17) | (rv_bits(h, 6, 2) << 12), 18);
            if (imm == 0) return false;
            *expanded = rv_encode_u(imm, rd, 0x37);
            return true;
        }
        case 4: {
            unsigned subop = rv_bits(h, 11, 10);
            if (subop == 0) {
                uint32_t shamt = (rv_bits(h, 12, 12) << 5) | rv_bits(h, 6, 2);
                *expanded = rv_encode_i((int32_t) shamt, crs1, 5, crs1, 0x13);
                return true;
            }
            if (subop == 1) {
                uint32_t shamt = (rv_bits(h, 12, 12) << 5) | rv_bits(h, 6, 2);
                *expanded = rv_encode_i((int32_t) (0x400 | shamt), crs1, 5, crs1, 0x13);
                return true;
            }
            if (subop == 2) {
                *expanded = rv_encode_i(rv_ci_imm(h), crs1, 7, crs1, 0x13);
                return true;
            }
            switch ((rv_bits(h, 12, 12) << 2) | rv_bits(h, 6, 5)) {
            case 0: *expanded = rv_encode_r(0x20, crs2, crs1, 0, crs1, 0x33); return true; // sub
            case 1: *expanded = rv_encode_r(0x00, crs2, crs1, 4, crs1, 0x33); return true; // xor
            case 2: *expanded = rv_encode_r(0x00, crs2, crs1, 6, crs1, 0x33); return true; // or
            case 3: *expanded = rv_encode_r(0x00, crs2, crs1, 7, crs1, 0x33); return true; // and
            case 4: *expanded = rv_encode_r(0x20, crs2, crs1, 0, crs1, 0x3b); return true; // subw
            case 5: *expanded = rv_encode_r(0x00, crs2, crs1, 0, crs1, 0x3b); return true; // addw
            default: return false;
            }
        }
        case 5: // c.j
            *expanded = rv_encode_j(rv_cj_imm(h), 0, 0x6f);
            return true;
        case 6: // c.beqz
            *expanded = rv_encode_b(rv_cb_imm(h), 0, crs1, 0, 0x63);
            return true;
        case 7: // c.bnez
            *expanded = rv_encode_b(rv_cb_imm(h), 0, crs1, 1, 0x63);
            return true;
        default:
            return false;
        }
    }

    if (quadrant == 2) {
        switch (funct3) {
        case 0: { // c.slli
            uint32_t shamt = (rv_bits(h, 12, 12) << 5) | rv_bits(h, 6, 2);
            *expanded = rv_encode_i((int32_t) shamt, rd, 1, rd, 0x13);
            return rd != 0;
        }
        case 1: { // c.fldsp
            uint32_t imm = (rv_bits(h, 4, 2) << 6) | (rv_bits(h, 12, 12) << 5) |
                (rv_bits(h, 6, 5) << 3);
            *expanded = rv_encode_i((int32_t) imm, 2, 3, rd, 0x07);
            return rd != 0;
        }
        case 2: { // c.lwsp
            uint32_t imm = (rv_bits(h, 3, 2) << 6) | (rv_bits(h, 12, 12) << 5) |
                (rv_bits(h, 6, 4) << 2);
            *expanded = rv_encode_i((int32_t) imm, 2, 2, rd, 0x03);
            return rd != 0;
        }
        case 3: { // c.ldsp
            uint32_t imm = (rv_bits(h, 4, 2) << 6) | (rv_bits(h, 12, 12) << 5) |
                (rv_bits(h, 6, 5) << 3);
            *expanded = rv_encode_i((int32_t) imm, 2, 3, rd, 0x03);
            return rd != 0;
        }
        case 4:
            if (rv_bits(h, 12, 12) == 0) {
                if (rs2 == 0) {
                    *expanded = rv_encode_i(0, rd, 0, 0, 0x67); // c.jr
                    return rd != 0;
                }
                *expanded = rv_encode_r(0, rs2, 0, 0, rd, 0x33); // c.mv
                return rd != 0;
            }
            if (rs2 == 0) {
                *expanded = rd == 0 ? rv_encode_i(1, 0, 0, 0, 0x73) : rv_encode_i(0, rd, 0, 1, 0x67);
                return true; // c.ebreak/c.jalr
            }
            *expanded = rv_encode_r(0, rs2, rd, 0, rd, 0x33); // c.add
            return rd != 0;
        case 5: { // c.fsdsp
            uint32_t imm = (rv_bits(h, 9, 7) << 6) | (rv_bits(h, 12, 10) << 3);
            *expanded = rv_encode_s((int32_t) imm, rs2, 2, 3, 0x27);
            return true;
        }
        case 6: { // c.swsp
            uint32_t imm = (rv_bits(h, 8, 7) << 6) | (rv_bits(h, 12, 9) << 2);
            *expanded = rv_encode_s((int32_t) imm, rs2, 2, 2, 0x23);
            return true;
        }
        case 7: { // c.sdsp
            uint32_t imm = (rv_bits(h, 9, 7) << 6) | (rv_bits(h, 12, 10) << 3);
            *expanded = rv_encode_s((int32_t) imm, rs2, 2, 3, 0x23);
            return true;
        }
        default:
            return false;
        }
    }

    return false;
}

static inline void rv_format_reg_imm(struct rv_insn *out, bool fp_rd, bool fp_rs1) {
    const char *rd = fp_rd ? rv_freg_name(out->rd) : rv_xreg_name(out->rd);
    const char *rs1 = fp_rs1 ? rv_freg_name(out->rs1) : rv_xreg_name(out->rs1);
    snprintf(out->operands, sizeof(out->operands), "%s,%s,%lld", rd, rs1, (long long) out->imm);
}

static inline void rv_format_reg_reg(struct rv_insn *out, bool fp_rd, bool fp_rs1, bool fp_rs2) {
    const char *rd = fp_rd ? rv_freg_name(out->rd) : rv_xreg_name(out->rd);
    const char *rs1 = fp_rs1 ? rv_freg_name(out->rs1) : rv_xreg_name(out->rs1);
    const char *rs2 = fp_rs2 ? rv_freg_name(out->rs2) : rv_xreg_name(out->rs2);
    snprintf(out->operands, sizeof(out->operands), "%s,%s,%s", rd, rs1, rs2);
}

static inline bool rv_decode_32(uint32_t insn, struct rv_insn *out) {
    memset(out, 0, sizeof(*out));
    out->raw = insn;
    out->expanded = insn;
    out->length = 4;
    out->rd = rv_bits(insn, 11, 7);
    out->rs1 = rv_bits(insn, 19, 15);
    out->rs2 = rv_bits(insn, 24, 20);
    out->rs3 = rv_bits(insn, 31, 27);
    out->funct3 = rv_bits(insn, 14, 12);
    out->funct7 = rv_bits(insn, 31, 25);
    out->rm = out->funct3;

    switch (rv_bits(insn, 6, 0)) {
    case 0x03: {
        static const char *names[8] = {"lb","lh","lw","ld","lbu","lhu","lwu",NULL};
        if (out->funct3 >= 7 || names[out->funct3] == NULL) break;
        out->op = RV_OP_LOAD;
        out->imm = rv_i_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", names[out->funct3]);
        snprintf(out->operands, sizeof(out->operands), "%s,%lld(%s)",
            rv_xreg_name(out->rd), (long long) out->imm, rv_xreg_name(out->rs1));
        return true;
    }
    case 0x07: {
        const char *name = out->funct3 == 2 ? "flw" : out->funct3 == 3 ? "fld" : NULL;
        if (name == NULL) break;
        out->op = RV_OP_LOAD_FP;
        out->imm = rv_i_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", name);
        snprintf(out->operands, sizeof(out->operands), "%s,%lld(%s)",
            rv_freg_name(out->rd), (long long) out->imm, rv_xreg_name(out->rs1));
        return true;
    }
    case 0x0f:
        out->op = RV_OP_MISC_MEM;
        if (out->funct3 == 0) {
            snprintf(out->mnemonic, sizeof(out->mnemonic), "fence");
            snprintf(out->operands, sizeof(out->operands), "0x%x,0x%x", rv_bits(insn, 27, 24), rv_bits(insn, 23, 20));
            return true;
        }
        if (out->funct3 == 1) {
            snprintf(out->mnemonic, sizeof(out->mnemonic), "fence.i");
            return true;
        }
        break;
    case 0x13: {
        static const char *names[8] = {"addi","slli","slti","sltiu","xori","srli","ori","andi"};
        out->op = RV_OP_OP_IMM;
        out->imm = rv_i_imm(insn);
        unsigned funct6 = rv_bits(insn, 31, 26);
        if (out->funct3 == 1 && funct6 != 0) break;
        if (out->funct3 == 5) {
            if (funct6 == 0x10) snprintf(out->mnemonic, sizeof(out->mnemonic), "srai");
            else if (funct6 == 0x00) snprintf(out->mnemonic, sizeof(out->mnemonic), "srli");
            else break;
            out->imm = rv_bits(insn, 25, 20);
        } else {
            snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", names[out->funct3]);
            if (out->funct3 == 1) out->imm = rv_bits(insn, 25, 20);
        }
        rv_format_reg_imm(out, false, false);
        return true;
    }
    case 0x17:
        out->op = RV_OP_AUIPC;
        out->imm = rv_u_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "auipc");
        snprintf(out->operands, sizeof(out->operands), "%s,0x%llx", rv_xreg_name(out->rd), (unsigned long long) out->imm);
        return true;
    case 0x1b:
        out->op = RV_OP_OP_IMM_32;
        out->imm = rv_i_imm(insn);
        if (out->funct3 == 0) snprintf(out->mnemonic, sizeof(out->mnemonic), "addiw");
        else if (out->funct3 == 1 && out->funct7 == 0) {
            snprintf(out->mnemonic, sizeof(out->mnemonic), "slliw");
            out->imm = rv_bits(insn, 24, 20);
        } else if (out->funct3 == 5 && out->funct7 == 0) {
            snprintf(out->mnemonic, sizeof(out->mnemonic), "srliw");
            out->imm = rv_bits(insn, 24, 20);
        } else if (out->funct3 == 5 && out->funct7 == 0x20) {
            snprintf(out->mnemonic, sizeof(out->mnemonic), "sraiw");
            out->imm = rv_bits(insn, 24, 20);
        } else break;
        rv_format_reg_imm(out, false, false);
        return true;
    case 0x23: {
        static const char *names[4] = {"sb","sh","sw","sd"};
        if (out->funct3 >= 4) break;
        out->op = RV_OP_STORE;
        out->imm = rv_s_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", names[out->funct3]);
        snprintf(out->operands, sizeof(out->operands), "%s,%lld(%s)",
            rv_xreg_name(out->rs2), (long long) out->imm, rv_xreg_name(out->rs1));
        return true;
    }
    case 0x27: {
        const char *name = out->funct3 == 2 ? "fsw" : out->funct3 == 3 ? "fsd" : NULL;
        if (name == NULL) break;
        out->op = RV_OP_STORE_FP;
        out->imm = rv_s_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", name);
        snprintf(out->operands, sizeof(out->operands), "%s,%lld(%s)",
            rv_freg_name(out->rs2), (long long) out->imm, rv_xreg_name(out->rs1));
        return true;
    }
    case 0x2f: {
        static const char *amo[32] = {
            [0x00] = "amoadd", [0x01] = "amoswap", [0x02] = "lr", [0x03] = "sc",
            [0x04] = "amoxor", [0x08] = "amoor", [0x0c] = "amoand",
            [0x10] = "amomin", [0x14] = "amomax", [0x18] = "amominu", [0x1c] = "amomaxu",
        };
        unsigned funct5 = rv_bits(insn, 31, 27);
        const char *base = funct5 < 32 ? amo[funct5] : NULL;
        const char *suffix = out->funct3 == 2 ? ".w" : out->funct3 == 3 ? ".d" : NULL;
        if (base == NULL || suffix == NULL) break;
        out->op = RV_OP_AMO;
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s%s", base, suffix);
        if (funct5 == 0x02)
            snprintf(out->operands, sizeof(out->operands), "%s,(%s)", rv_xreg_name(out->rd), rv_xreg_name(out->rs1));
        else
            rv_format_reg_reg(out, false, false, false);
        return true;
    }
    case 0x33: {
        const char *name = NULL;
        out->op = RV_OP_OP;
        if (out->funct7 == 0x00) {
            static const char *names[8] = {"add","sll","slt","sltu","xor","srl","or","and"};
            name = names[out->funct3];
        } else if (out->funct7 == 0x20) {
            if (out->funct3 == 0) name = "sub";
            else if (out->funct3 == 5) name = "sra";
        } else if (out->funct7 == 0x01) {
            static const char *names[8] = {"mul","mulh","mulhsu","mulhu","div","divu","rem","remu"};
            name = names[out->funct3];
        }
        if (name == NULL) break;
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", name);
        rv_format_reg_reg(out, false, false, false);
        return true;
    }
    case 0x37:
        out->op = RV_OP_LUI;
        out->imm = rv_u_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "lui");
        snprintf(out->operands, sizeof(out->operands), "%s,0x%llx", rv_xreg_name(out->rd), (unsigned long long) out->imm);
        return true;
    case 0x3b: {
        const char *name = NULL;
        out->op = RV_OP_OP_32;
        if (out->funct7 == 0x00) {
            static const char *names[8] = {"addw","sllw",NULL,NULL,NULL,"srlw",NULL,NULL};
            name = names[out->funct3];
        } else if (out->funct7 == 0x20) {
            if (out->funct3 == 0) name = "subw";
            else if (out->funct3 == 5) name = "sraw";
        } else if (out->funct7 == 0x01) {
            static const char *names[8] = {"mulw",NULL,NULL,NULL,"divw","divuw","remw","remuw"};
            name = names[out->funct3];
        }
        if (name == NULL) break;
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", name);
        rv_format_reg_reg(out, false, false, false);
        return true;
    }
    case 0x43: case 0x47: case 0x4b: case 0x4f: {
        static const char *names[4] = {"fmadd","fmsub","fnmsub","fnmadd"};
        static const enum rv_decode_op ops[4] = {RV_OP_MADD, RV_OP_MSUB, RV_OP_NMSUB, RV_OP_NMADD};
        unsigned idx = (rv_bits(insn, 6, 0) - 0x43) / 4;
        const char *fmt = rv_bits(insn, 26, 25) == 0 ? ".s" : rv_bits(insn, 26, 25) == 1 ? ".d" : NULL;
        if (fmt == NULL) break;
        out->op = ops[idx];
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s%s", names[idx], fmt);
        snprintf(out->operands, sizeof(out->operands), "%s,%s,%s,%s",
            rv_freg_name(out->rd), rv_freg_name(out->rs1), rv_freg_name(out->rs2), rv_freg_name(out->rs3));
        return true;
    }
    case 0x53: {
        static const char *fmt_suffix[4] = {".s",".d",NULL,NULL};
        const char *fmt = fmt_suffix[rv_bits(insn, 26, 25)];
        const char *name = NULL;
        unsigned funct5 = rv_bits(insn, 31, 27);
        out->op = RV_OP_OP_FP;
        if (fmt == NULL) break;
        switch (funct5) {
        case 0x00: name = "fadd"; break;
        case 0x01: name = "fsub"; break;
        case 0x02: name = "fmul"; break;
        case 0x03: name = "fdiv"; break;
        case 0x0b: name = "fsqrt"; break;
        case 0x04:
            name = out->funct3 == 0 ? "fsgnj" : out->funct3 == 1 ? "fsgnjn" : out->funct3 == 2 ? "fsgnjx" : NULL;
            break;
        case 0x05:
            name = out->funct3 == 0 ? "fmin" : out->funct3 == 1 ? "fmax" : NULL;
            break;
        case 0x08: name = "fcvt"; break;
        case 0x14:
            name = out->funct3 == 0 ? "fle" : out->funct3 == 1 ? "flt" : out->funct3 == 2 ? "feq" : NULL;
            break;
        case 0x18: name = "fcvt.w"; break;
        case 0x19: name = "fcvt.wu"; break;
        case 0x1a: name = "fcvt"; break;
        case 0x1b: name = "fcvt"; break;
        case 0x1c:
            name = out->funct3 == 0 ? "fmv.x" : out->funct3 == 1 ? "fclass" : NULL;
            break;
        case 0x1e:
            name = out->funct3 == 0 ? "fmv" : NULL;
            break;
        default:
            break;
        }
        if (name == NULL) break;
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s%s", name, fmt);
        if (funct5 == 0x08 || funct5 == 0x14 || funct5 == 0x18 || funct5 == 0x19 || funct5 == 0x1c)
            rv_format_reg_reg(out, false, true, true);
        else
            rv_format_reg_reg(out, true, true, true);
        return true;
    }
    case 0x63: {
        static const char *names[8] = {"beq","bne",NULL,NULL,"blt","bge","bltu","bgeu"};
        if (names[out->funct3] == NULL) break;
        out->op = RV_OP_BRANCH;
        out->imm = rv_b_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", names[out->funct3]);
        snprintf(out->operands, sizeof(out->operands), "%s,%s,%+lld",
            rv_xreg_name(out->rs1), rv_xreg_name(out->rs2), (long long) out->imm);
        return true;
    }
    case 0x67:
        if (out->funct3 != 0) break;
        out->op = RV_OP_JALR;
        out->imm = rv_i_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "jalr");
        snprintf(out->operands, sizeof(out->operands), "%s,%lld(%s)",
            rv_xreg_name(out->rd), (long long) out->imm, rv_xreg_name(out->rs1));
        return true;
    case 0x6f:
        out->op = RV_OP_JAL;
        out->imm = rv_j_imm(insn);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "jal");
        snprintf(out->operands, sizeof(out->operands), "%s,%+lld", rv_xreg_name(out->rd), (long long) out->imm);
        return true;
    case 0x73: {
        out->op = RV_OP_SYSTEM;
        out->imm = rv_i_imm(insn);
        if (out->funct3 == 0) {
            if (out->imm == 0) snprintf(out->mnemonic, sizeof(out->mnemonic), "ecall");
            else if (out->imm == 1) snprintf(out->mnemonic, sizeof(out->mnemonic), "ebreak");
            else break;
            return true;
        }
        static const char *names[8] = {NULL,"csrrw","csrrs","csrrc",NULL,"csrrwi","csrrsi","csrrci"};
        const char *name = names[out->funct3];
        if (name == NULL) break;
        unsigned csr = rv_bits(insn, 31, 20);
        const char *csr_name = rv_csr_name(csr);
        snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", name);
        if (out->funct3 >= 5)
            if (csr_name)
                snprintf(out->operands, sizeof(out->operands), "%s,%s,%u",
                    rv_xreg_name(out->rd), csr_name, out->rs1);
            else
                snprintf(out->operands, sizeof(out->operands), "%s,0x%x,%u",
                    rv_xreg_name(out->rd), csr, out->rs1);
        else if (csr_name)
            snprintf(out->operands, sizeof(out->operands), "%s,%s,%s",
                rv_xreg_name(out->rd), csr_name, rv_xreg_name(out->rs1));
        else
            snprintf(out->operands, sizeof(out->operands), "%s,0x%x,%s",
                rv_xreg_name(out->rd), csr, rv_xreg_name(out->rs1));
        return true;
    }
    default:
        break;
    }

    rv_set_illegal(out, insn, 4);
    return false;
}

static inline bool rv_decode(const uint8_t *code, size_t code_size, struct rv_insn *out) {
    if (code_size < 2) {
        rv_set_illegal(out, 0, 0);
        return false;
    }

    uint16_t h = (uint16_t) code[0] | ((uint16_t) code[1] << 8);
    if ((h & 3) != 3) {
        uint32_t expanded;
        if (!rv_expand_compressed(h, &expanded)) {
            rv_set_illegal(out, h, 2);
            out->compressed = true;
            return false;
        }
        bool ok = rv_decode_32(expanded, out);
        out->raw = h;
        out->expanded = expanded;
        out->length = 2;
        out->compressed = true;
        return ok;
    }

    if (code_size < 4) {
        rv_set_illegal(out, h, 2);
        return false;
    }
    uint32_t insn = (uint32_t) h | ((uint32_t) code[2] << 16) | ((uint32_t) code[3] << 24);
    return rv_decode_32(insn, out);
}

#endif
