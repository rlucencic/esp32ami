/* RV32IM instructions for the translating core (jit_rv.c): encoders that
 * append 32-bit words to a buffer, labels and the fixups of branches to
 * them. The board runs the words; the host build runs them through its
 * own interpreter (host/rvsim.c), so the collection checks the very code
 * the board runs.
 */
#ifndef RV_H
#define RV_H

#include <stdbool.h>
#include <stdint.h>

enum {
    X0 = 0, RA = 1, SP = 2, GP = 3, TP = 4, T0 = 5, T1 = 6, T2 = 7,
    S0 = 8, S1 = 9, A0 = 10, A1 = 11, A2 = 12, A3 = 13, A4 = 14, A5 = 15, A6 = 16, A7 = 17,
    S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23, S8 = 24, S9 = 25, S10 = 26, S11 = 27,
    T3 = 28, T4 = 29, T5 = 30, T6 = 31,
};

#define RV_LABELS   512
#define RV_FIXUPS   1024

typedef struct {
    uint32_t *start, *p, *end;
    bool full;                              /* ran out of room: the block is cut */
    int32_t label[RV_LABELS];               /* word offset, -1 unplaced */
    int nlabels;
    struct { int32_t at; int16_t label; uint8_t kind; } fixup[RV_FIXUPS];
    int nfixups;
} rv_t;

static inline void rv_word(rv_t *r, uint32_t w)
{
    if (r->p < r->end) {
        *r->p++ = w;
    } else {
        r->full = true;
    }
}

static inline uint32_t rv_r(uint32_t f7, int rs2, int rs1, uint32_t f3, int rd, uint32_t op)
{
    return f7 << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (uint32_t)rd << 7 | op;
}

static inline uint32_t rv_i(int32_t imm, int rs1, uint32_t f3, int rd, uint32_t op)
{
    return ((uint32_t)imm & 0xFFF) << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (uint32_t)rd << 7 | op;
}

static inline uint32_t rv_s(int32_t imm, int rs2, int rs1, uint32_t f3, uint32_t op)
{
    uint32_t u = (uint32_t)imm;
    return ((u >> 5) & 0x7F) << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (u & 0x1F) << 7 | op;
}

static inline uint32_t rv_b(int32_t imm, int rs2, int rs1, uint32_t f3)
{
    uint32_t u = (uint32_t)imm;
    return ((u >> 12) & 1) << 31 | ((u >> 5) & 0x3F) << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 |
           f3 << 12 | ((u >> 1) & 0xF) << 8 | ((u >> 11) & 1) << 7 | 0x63;
}

static inline uint32_t rv_j(int32_t imm, int rd)
{
    uint32_t u = (uint32_t)imm;
    return ((u >> 20) & 1) << 31 | ((u >> 1) & 0x3FF) << 21 | ((u >> 11) & 1) << 20 | ((u >> 12) & 0xFF) << 12 |
           (uint32_t)rd << 7 | 0x6F;
}

/* ---- Register and immediate arithmetic ---- */
#define RV_OP(name, f7, f3) \
    static inline void rv_##name(rv_t *r, int rd, int rs1, int rs2) { rv_word(r, rv_r(f7, rs2, rs1, f3, rd, 0x33)); }
RV_OP(add, 0x00, 0) RV_OP(sub, 0x20, 0) RV_OP(sll, 0x00, 1) RV_OP(slt, 0x00, 2) RV_OP(sltu, 0x00, 3)
RV_OP(xor, 0x00, 4) RV_OP(srl, 0x00, 5) RV_OP(sra, 0x20, 5) RV_OP(or, 0x00, 6) RV_OP(and, 0x00, 7)
RV_OP(mul, 0x01, 0) RV_OP(mulh, 0x01, 1) RV_OP(mulhu, 0x01, 3) RV_OP(div, 0x01, 4) RV_OP(divu, 0x01, 5)
RV_OP(rem, 0x01, 6) RV_OP(remu, 0x01, 7)
#undef RV_OP

#define RV_OPI(name, f3) \
    static inline void rv_##name(rv_t *r, int rd, int rs1, int32_t imm) { rv_word(r, rv_i(imm, rs1, f3, rd, 0x13)); }
RV_OPI(addi, 0) RV_OPI(slti, 2) RV_OPI(sltiu, 3) RV_OPI(xori, 4) RV_OPI(ori, 6) RV_OPI(andi, 7)
#undef RV_OPI

static inline void rv_slli(rv_t *r, int rd, int rs1, int sh) { rv_word(r, rv_i(sh & 31, rs1, 1, rd, 0x13)); }
static inline void rv_srli(rv_t *r, int rd, int rs1, int sh) { rv_word(r, rv_i(sh & 31, rs1, 5, rd, 0x13)); }
static inline void rv_srai(rv_t *r, int rd, int rs1, int sh) { rv_word(r, rv_i(0x400 | (sh & 31), rs1, 5, rd, 0x13)); }
static inline void rv_lui(rv_t *r, int rd, uint32_t hi20) { rv_word(r, (hi20 << 12) | (uint32_t)rd << 7 | 0x37); }

static inline void rv_mv(rv_t *r, int rd, int rs) { rv_addi(r, rd, rs, 0); }

/* rd = v: lui and/or addi */
static inline void rv_li(rv_t *r, int rd, uint32_t v)
{
    int32_t lo = (int32_t)(v << 20) >> 20;
    uint32_t hi = (v - (uint32_t)lo) >> 12;
    if (hi) {
        rv_lui(r, rd, hi);
        if (lo) {
            rv_addi(r, rd, rd, lo);
        }
    } else {
        rv_addi(r, rd, X0, lo);
    }
}

/* ---- Loads and stores ---- */
static inline void rv_lb(rv_t *r, int rd, int32_t off, int rs1) { rv_word(r, rv_i(off, rs1, 0, rd, 0x03)); }
static inline void rv_lh(rv_t *r, int rd, int32_t off, int rs1) { rv_word(r, rv_i(off, rs1, 1, rd, 0x03)); }
static inline void rv_lw(rv_t *r, int rd, int32_t off, int rs1) { rv_word(r, rv_i(off, rs1, 2, rd, 0x03)); }
static inline void rv_lbu(rv_t *r, int rd, int32_t off, int rs1) { rv_word(r, rv_i(off, rs1, 4, rd, 0x03)); }
static inline void rv_lhu(rv_t *r, int rd, int32_t off, int rs1) { rv_word(r, rv_i(off, rs1, 5, rd, 0x03)); }
static inline void rv_sb(rv_t *r, int rs2, int32_t off, int rs1) { rv_word(r, rv_s(off, rs2, rs1, 0, 0x23)); }
static inline void rv_sh(rv_t *r, int rs2, int32_t off, int rs1) { rv_word(r, rv_s(off, rs2, rs1, 1, 0x23)); }
static inline void rv_sw(rv_t *r, int rs2, int32_t off, int rs1) { rv_word(r, rv_s(off, rs2, rs1, 2, 0x23)); }

/* ---- Jumps ---- */
static inline void rv_jalr(rv_t *r, int rd, int rs1, int32_t off) { rv_word(r, rv_i(off, rs1, 0, rd, 0x67)); }
static inline void rv_ret(rv_t *r) { rv_jalr(r, X0, RA, 0); }

/* a call of the function at an absolute address (t6 holds its upper bits) */
static inline void rv_call(rv_t *r, uint32_t addr)
{
    int32_t lo = (int32_t)(addr << 20) >> 20;
    rv_lui(r, T6, (addr - (uint32_t)lo) >> 12);
    rv_jalr(r, RA, T6, lo);
}

/* ---- Labels ---- */
enum { RV_FIX_B, RV_FIX_J };
enum { RV_BEQ = 0, RV_BNE = 1, RV_BLT = 4, RV_BGE = 5, RV_BLTU = 6, RV_BGEU = 7 };

static inline int rv_new_label(rv_t *r)
{
    if (r->nlabels >= RV_LABELS) {
        r->full = true;
        return 0;
    }
    r->label[r->nlabels] = -1;
    return r->nlabels++;
}

static inline void rv_place(rv_t *r, int l)
{
    r->label[l] = (int32_t)(r->p - r->start);
}

static inline void rv_fix(rv_t *r, int l, int kind)
{
    if (r->nfixups >= RV_FIXUPS) {
        r->full = true;
        return;
    }
    r->fixup[r->nfixups].at = (int32_t)(r->p - r->start);
    r->fixup[r->nfixups].label = (int16_t)l;
    r->fixup[r->nfixups].kind = (uint8_t)kind;
    r->nfixups++;
}

/* branch (f3: RV_BEQ ...) on rs1 against rs2 to label l */
static inline void rv_bcc(rv_t *r, int f3, int rs1, int rs2, int l)
{
    rv_fix(r, l, RV_FIX_B);
    rv_word(r, rv_b(0, rs2, rs1, (uint32_t)f3));
}

static inline void rv_beqz(rv_t *r, int rs, int l) { rv_bcc(r, RV_BEQ, rs, X0, l); }
static inline void rv_bnez(rv_t *r, int rs, int l) { rv_bcc(r, RV_BNE, rs, X0, l); }
static inline void rv_blez(rv_t *r, int rs, int l) { rv_bcc(r, RV_BGE, X0, rs, l); }
static inline void rv_bgtz(rv_t *r, int rs, int l) { rv_bcc(r, RV_BLT, X0, rs, l); }

static inline void rv_jl(rv_t *r, int l)
{
    rv_fix(r, l, RV_FIX_J);
    rv_word(r, rv_j(0, X0));
}

/* the fixups resolved: false if a branch can't reach its label */
static inline bool rv_link(rv_t *r)
{
    for (int i = 0; i < r->nfixups; i++) {
        int32_t at = r->fixup[i].at, to = r->label[r->fixup[i].label];
        if (to < 0) {
            return false;
        }
        int32_t off = (to - at) * 4;
        uint32_t w = r->start[at];
        if (r->fixup[i].kind == RV_FIX_B) {
            if (off < -4096 || off > 4094) {
                return false;
            }
            r->start[at] = rv_b(off, (int)((w >> 20) & 31), (int)((w >> 15) & 31), (w >> 12) & 7);
        } else {
            if (off < -(1 << 20) || off >= (1 << 20)) {
                return false;
            }
            r->start[at] = rv_j(off, (int)((w >> 7) & 31));
        }
    }
    return true;
}

#endif
