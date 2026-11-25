/*
 * Wait loops: short loops that test memory and branch back, the way
 * programs wait for a flag their interrupt code sets, a line of the beam,
 * a CIA's timer or a button. On the A1200 such loops took most of core 1
 * in titles that fell behind (TODO 18, docs/a1200-load-survey-2026-11-25.md:
 * Soccer Kid 82% of its instructions, Fatman 74-99%, Black Viper 64-99%;
 * TODO 19: Tactical Manager 2 96% in `move.l $dff004,d0 / andi.l #$1ff00,d0
 * / cmp.l #$12000,d0 / bne`, AmiBee 90% in twelve instructions of compiled
 * code waiting on a byte).
 *
 * What they read changes only where the CPU's slice ends: RAM through the
 * CPU itself (its interrupts come as a slice starts), the blitter (done at
 * a write, which ends the slice) and disk DMA (at the line's end); the
 * chips' registers custom_peek() takes at events and lines' ends (VHPOSR's
 * horizontal byte excepted: mem_peek() marks it unstable); a CIA's port,
 * TOD and serial data at lines' ends. A CIA timer polled with its
 * interrupt off underflows anywhere: a skip on ICR, CRA or CRB ends there.
 *
 * A loop here is up to MAX_INSNS instructions from a few kinds (moves into
 * registers, MOVEQ, pushes and pops on the stack, EXT, SWAP, AND, OR,
 * shifts, TST, CMP, CMPI, BTST, Scc, forward branches, NOP) ending in a
 * branch back to its start. As a slice starts where the last check found
 * the CPU (amiga.c checks as a line begins, and in the line's other slices
 * while the CPU is in a loop), the loop is decoded once (kept with its
 * words, compared on each use: code changes), and a pass of it worked out
 * from the registers and memory as they are, without the reads' side
 * effects: if the pass comes back to the loop's start with the registers
 * and flags it started with (a fixed point), wrote no memory but the stack
 * below the stack pointer, decided no branch on an unstable bit, and no
 * interrupt is due, every pass until the slice ends would be the same: the
 * slice passes without the CPU. A loop runs for a line or two before it's
 * found, and the slice in which it ends runs.
 */
#include <string.h>
#include "m68k.h"
#include "amiga_internal.h"

#define MAX_INSNS       16
#define MAX_WORDS       40          /* the loop's code: 80 bytes at most */
#define CANDIDATES      4           /* branches back found from a PC */
#define STACK           8           /* pushes within a pass */

enum { O_DN, O_AN, O_IND, O_D16, O_ABS, O_IMM, O_POP };     /* operands */
enum {
    I_MOVE, I_MOVEA, I_PUSH, I_MOVEQ, I_PUSHM, I_POPM, I_EXT, I_SWAP, I_AND, I_OR,
    I_SHIFT, I_TST, I_CMP, I_CMPI, I_BTST, I_SCC, I_BCC, I_NOP,
};
enum { SH_ASR, SH_LSR, SH_LSL };
#define BACK            0xFF        /* a branch's next: the loop's start */
#define EXIT            0xFE        /* out of the loop */

/* the flags, and which rest on unstable bits */
enum { F_C = 1, F_V = 2, F_Z = 4, F_N = 8, F_X = 16 };

typedef struct {
    uint8_t kind, reg;
    int16_t disp;
    uint32_t val;                   /* O_ABS: the address; O_IMM: the value */
} opnd_t;

typedef struct {
    uint8_t op, size, cc, reg;      /* reg: the register written or compared (0-7; MOVEA's An) */
    uint8_t sub;                    /* shifts: SH_*; EXT: 2 .W, 4 .L, 5 EXTB.L; BTST: 1 by Dn */
    uint8_t next;                   /* a branch's target: an index, BACK or EXIT */
    opnd_t src;
    uint32_t imm;                   /* the immediate, shift count, MOVEM's list, BTST's bit,
                                     * (while decoding) a branch's target address */
} insn_t;

typedef struct {
    uint32_t t, end;                /* the first instruction's address, the address after the last */
    uint64_t starts;                /* the instructions' starts: bit (address - t) / 2 */
    uint16_t regs;                  /* the registers it reads or writes (D0-D7, A0-A7: bits 0-15) */
    uint16_t written;               /* those it writes */
    uint8_t n, nwords;
    uint16_t words[MAX_WORDS];      /* the code from t, as decoded */
    insn_t in[MAX_INSNS];
} loop_t;

#define LOOPS           8
#define NOT_LOOPS       16
static struct {
    loop_t loop[LOOPS];
    int next;                       /* the entry the next decoded loop replaces */
    loop_t *last;                   /* the last one found */
    uint32_t not_pc[NOT_LOOPS];     /* PCs found not to be in loops this frame + 1, by their low bits */
    uint32_t last_pc;               /* where the last slice started */
} s_w;

void wait_reset(void)
{
    memset(&s_w, 0, sizeof(s_w));
}

static inline uint16_t word_at(uint32_t a)
{
    return amiga_peek16(a & 0xFFFFFF);
}

static inline uint32_t long_at(uint32_t a)
{
    return (uint32_t)word_at(a) << 16 | word_at(a + 2);
}

/* ---- Decoding ---- */

/* A source operand (mode, reg) of size bytes with its extension words at
 * e: their count in bytes, or -1 (not one of ours: postincrement but the
 * stack's pop, predecrement, indexed) */
static int decode_ea(opnd_t *o, int mode, int reg, int size, uint32_t e)
{
    o->reg = reg;
    switch (mode) {
    case 0:
        o->kind = O_DN;
        return 0;
    case 1:
        o->kind = O_AN;
        return size == 1 ? -1 : 0;
    case 2:
        o->kind = O_IND;
        return 0;
    case 3:
        if (reg != 7 || size == 1) {
            return -1;
        }
        o->kind = O_POP;
        return 0;
    case 5:
        o->kind = O_D16;
        o->disp = (int16_t)word_at(e);
        return 2;
    case 7:
        switch (reg) {
        case 0:
            o->kind = O_ABS;
            o->val = (uint32_t)(int32_t)(int16_t)word_at(e) & 0xFFFFFF;
            return 2;
        case 1:
            o->kind = O_ABS;
            o->val = long_at(e) & 0xFFFFFF;
            return 4;
        case 2:
            o->kind = O_ABS;
            o->reg = 8;             /* (PC-relative: see data_only()) */
            o->val = (e + (int16_t)word_at(e)) & 0xFFFFFF;
            return 2;
        case 4:
            o->kind = O_IMM;
            if (size == 4) {
                o->val = long_at(e);
                return 4;
            }
            o->val = size == 2 ? word_at(e) : word_at(e) & 0xFF;
            return 2;
        default:
            return -1;
        }
    default:
        return -1;
    }
}

/* (TST, CMPI and BTST #n take no address register, immediate or, on the
 * 68000, PC-relative operand: the opcode is another's or illegal) */
static bool data_only(const opnd_t *o, bool pcrel_ok)
{
    return o->kind != O_AN && o->kind != O_IMM && !(o->kind == O_ABS && o->reg == 8 && !pcrel_ok);
}

/* The instruction at pc: its length in bytes, or 0 (not one of ours) */
static int decode_insn(insn_t *in, uint32_t pc)
{
    static const uint8_t sizes[4] = { 1, 2, 4, 0 };
    uint16_t op = word_at(pc);
    int mode = (op >> 3) & 7, reg = op & 7, size = sizes[(op >> 6) & 3], ext;
    bool cpu020 = ami.cpu_shift != 0;
    memset(in, 0, sizeof(*in));
    if (op == 0x4E71) {
        in->op = I_NOP;
        return 2;
    }
    if ((op & 0xF000) == 0x6000) {                  /* Bcc, BRA (not BSR) */
        int8_t d8 = (int8_t)(op & 0xFF);
        in->cc = (op >> 8) & 15;
        if (in->cc == 1 || d8 == -1) {
            return 0;
        }
        in->op = I_BCC;
        in->imm = (pc + 2 + (d8 ? d8 : (int16_t)word_at(pc + 2))) & 0xFFFFFF;
        return d8 ? 2 : 4;
    }
    if ((op & 0xF100) == 0x7000) {                  /* MOVEQ */
        in->op = I_MOVEQ;
        in->size = 4;
        in->reg = (op >> 9) & 7;
        in->imm = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
        return 2;
    }
    if ((op & 0xC000) == 0 && (op & 0x3000)) {      /* MOVE, MOVEA */
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        in->size = (op & 0x3000) == 0x1000 ? 1 : (op & 0x3000) == 0x3000 ? 2 : 4;
        if (dmode == 0) {
            in->op = I_MOVE;
        } else if (dmode == 1 && in->size > 1) {
            in->op = I_MOVEA;
        } else if (dmode == 4 && dreg == 7 && in->size > 1) {
            in->op = I_PUSH;
        } else {
            return 0;
        }
        in->reg = dreg;
        ext = decode_ea(&in->src, mode, reg, in->size, pc + 2);
        return ext < 0 ? 0 : 2 + ext;
    }
    if ((op & 0xFFF8) == 0x4880 || (op & 0xFFF8) == 0x48C0 || (op & 0xFFF8) == 0x49C0) {
        if ((op & 0xFFF8) == 0x49C0 && !cpu020) {
            return 0;
        }
        in->op = I_EXT;                             /* EXT.W, EXT.L, EXTB.L */
        in->sub = (op & 0xFFF8) == 0x4880 ? 2 : (op & 0xFFF8) == 0x48C0 ? 4 : 5;
        in->reg = reg;
        return 2;
    }
    if ((op & 0xFFF8) == 0x4840) {
        in->op = I_SWAP;
        in->reg = reg;
        return 2;
    }
    if (op == 0x48E7 || op == 0x4CDF) {             /* MOVEM.L list,-(A7); (A7)+,list */
        in->op = op == 0x48E7 ? I_PUSHM : I_POPM;
        in->imm = word_at(pc + 2);
        uint16_t a7 = op == 0x48E7 ? 0x0001 : 0x8000;   /* (A7 itself: not here) */
        return in->imm & a7 ? 0 : 4;
    }
    if ((op & 0xFF00) == 0x4A00 && size) {          /* TST */
        in->op = I_TST;
        in->size = size;
        ext = decode_ea(&in->src, mode, reg, size, pc + 2);
        if (ext < 0 || in->src.kind == O_POP || (!cpu020 && !data_only(&in->src, false))) {
            return 0;
        }
        return 2 + ext;
    }
    if (((op & 0xFF00) == 0x0200 || (op & 0xFF00) == 0x0000) && size && mode == 0) {
        in->op = (op & 0xFF00) == 0x0200 ? I_AND : I_OR;    /* ANDI, ORI #imm,Dn */
        in->size = size;
        in->reg = reg;
        ext = decode_ea(&in->src, 7, 4, size, pc + 2);
        return 2 + ext;
    }
    if ((op & 0xFF00) == 0x0C00 && size) {          /* CMPI #imm,<ea> */
        in->op = I_CMPI;
        in->size = size;
        in->imm = size == 4 ? long_at(pc + 2) : size == 2 ? word_at(pc + 2) : word_at(pc + 2) & 0xFF;
        int len = size == 4 ? 6 : 4;
        ext = decode_ea(&in->src, mode, reg, size, pc + len);
        if (ext < 0 || in->src.kind == O_POP || !data_only(&in->src, cpu020)) {
            return 0;
        }
        return len + ext;
    }
    if ((op & 0xFFC0) == 0x0800 || ((op & 0xF1C0) == 0x0100 && mode != 1)) {
        in->op = I_BTST;                            /* BTST #n,<ea>; BTST Dn,<ea> */
        in->size = mode == 0 ? 4 : 1;
        int len = 2;
        if ((op & 0xFFC0) == 0x0800) {
            in->imm = word_at(pc + 2) & (mode == 0 ? 31 : 7);
            len = 4;
        } else {
            in->sub = 1;
            in->reg = (op >> 9) & 7;
        }
        ext = decode_ea(&in->src, mode, reg, in->size, pc + len);
        if (ext < 0 || in->src.kind == O_POP || in->src.kind == O_AN || (!in->sub && in->src.kind == O_IMM)) {
            return 0;
        }
        return len + ext;
    }
    if (((op & 0xF100) == 0xC000 || (op & 0xF100) == 0x8000) && size && mode != 1) {
        in->op = (op & 0xF000) == 0xC000 ? I_AND : I_OR;    /* AND, OR <ea>,Dn */
        in->size = size;
        in->reg = (op >> 9) & 7;
        ext = decode_ea(&in->src, mode, reg, size, pc + 2);
        return ext < 0 ? 0 : 2 + ext;
    }
    if ((op & 0xF100) == 0xB000 && size) {          /* CMP <ea>,Dn */
        in->op = I_CMP;
        in->size = size;
        in->reg = (op >> 9) & 7;
        ext = decode_ea(&in->src, mode, reg, size, pc + 2);
        return ext < 0 ? 0 : 2 + ext;
    }
    if ((op & 0xF020) == 0xE000 && size && ((op >> 3) & 3) < 2) {   /* ASx, LSx #n,Dn */
        bool left = (op >> 8) & 1, logical = (op >> 3) & 1;
        if (left && !logical) {
            return 0;                               /* (ASL's V: not here) */
        }
        in->op = I_SHIFT;
        in->sub = left ? SH_LSL : logical ? SH_LSR : SH_ASR;
        in->size = size;
        in->reg = reg;
        in->imm = (op >> 9) & 7 ? (op >> 9) & 7 : 8;
        return 2;
    }
    if ((op & 0xF0F8) == 0x50C0) {                  /* Scc Dn */
        in->op = I_SCC;
        in->cc = (op >> 8) & 15;
        in->reg = reg;
        return 2;
    }
    return 0;
}

/* The registers an instruction reads and writes */
static void regs_of(const insn_t *in, uint16_t *uses, uint16_t *writes)
{
    uint16_t u = 0, w = 0;
    const opnd_t *o = &in->src;
    if (in->op != I_BCC && in->op != I_NOP && in->op != I_PUSHM && in->op != I_POPM &&
        in->op != I_MOVEQ && in->op != I_EXT && in->op != I_SWAP && in->op != I_SHIFT && in->op != I_SCC) {
        switch (o->kind) {
        case O_DN:  u |= 1 << o->reg; break;
        case O_AN:
        case O_IND:
        case O_D16: u |= 0x100 << o->reg; break;
        case O_POP: u |= 0x8000; w |= 0x8000; break;
        default:    break;
        }
    }
    switch (in->op) {
    case I_MOVE:
    case I_MOVEQ:
    case I_EXT:
    case I_SWAP:
    case I_AND:
    case I_OR:
    case I_SHIFT:
    case I_SCC:
        u |= 1 << in->reg;
        w |= 1 << in->reg;
        break;
    case I_MOVEA:
        w |= 0x100 << in->reg;
        break;
    case I_PUSH:
        u |= 0x8000;
        w |= 0x8000;
        break;
    case I_PUSHM:                   /* (its list reversed: bit 0 is A7) */
        for (int r = 0; r < 16; r++) {
            if (in->imm & (0x8000 >> r)) {
                u |= 1 << r;
            }
        }
        u |= 0x8000;
        w |= 0x8000;
        break;
    case I_POPM:
        w |= in->imm | 0x8000;
        u |= in->imm | 0x8000;
        break;
    case I_CMP:
        u |= 1 << in->reg;
        break;
    case I_BTST:
        if (in->sub) {
            u |= 1 << in->reg;
        }
        break;
    default:
        break;
    }
    *uses |= u;
    *writes |= w;
}

/* The loop from t to end, decoded; pc must be one of its instructions */
static bool decode_loop(loop_t *l, uint32_t t, uint32_t end, uint32_t pc)
{
    uint32_t at[MAX_INSNS];
    memset(l, 0, sizeof(*l));
    l->t = t;
    l->end = end;
    if (end - t > 2 * MAX_WORDS) {
        return false;
    }
    uint32_t a = t;
    while (a < end) {
        if (l->n == MAX_INSNS) {
            return false;
        }
        int len = decode_insn(&l->in[l->n], a);
        if (!len) {
            return false;
        }
        at[l->n++] = a;
        l->starts |= 1ull << ((a - t) / 2);
        a += len;
    }
    if (a != end || !(l->starts & (1ull << ((pc - t) / 2)))) {
        return false;
    }
    for (int i = 0; i < l->n; i++) {
        insn_t *in = &l->in[i];
        regs_of(in, &l->regs, &l->written);
        if (in->op != I_BCC) {
            continue;
        }
        uint32_t target = in->imm;
        if (target == t) {
            in->next = BACK;
        } else if (target < t || target >= end) {
            in->next = EXIT;
        } else if (target <= at[i]) {
            return false;           /* (a loop inside: not here) */
        } else {
            int k = i + 1;
            while (k < l->n && at[k] != target) {
                k++;
            }
            if (k == l->n) {
                return false;       /* (into an instruction) */
            }
            in->next = (uint8_t)k;
        }
    }
    const insn_t *last = &l->in[l->n - 1];
    if (last->op != I_BCC || last->next != BACK) {
        return false;
    }
    l->nwords = (end - t) / 2;
    for (int i = 0; i < l->nwords; i++) {
        l->words[i] = word_at(t + 2 * i);
    }
    return true;
}

/* The loop pc is in: from pc on, the branches back to it or before it
 * (within reach), the outermost first that decodes as a loop */
static bool decode(loop_t *l, uint32_t pc)
{
    uint32_t cand_t[CANDIDATES], cand_end[CANDIDATES];
    int ncand = 0;
    uint32_t a = pc;
    insn_t in;
    for (int k = 0; k < MAX_INSNS; k++) {
        int len = decode_insn(&in, a);
        if (!len) {
            break;
        }
        if (in.op == I_BCC && in.imm <= pc && pc - in.imm + (a + len - pc) <= 2 * MAX_WORDS) {
            if (ncand == CANDIDATES) {
                memmove(cand_t, cand_t + 1, sizeof(cand_t) - sizeof(cand_t[0]));
                memmove(cand_end, cand_end + 1, sizeof(cand_end) - sizeof(cand_end[0]));
                ncand--;
            }
            cand_t[ncand] = in.imm;
            cand_end[ncand++] = a + len;
        }
        a += len;
        if (in.op == I_BCC && in.cc == 0) {
            break;                  /* (BRA: the straight line ends) */
        }
    }
    while (ncand--) {
        if (decode_loop(l, cand_t[ncand], cand_end[ncand], pc)) {
            return true;
        }
    }
    return false;
}

/* ---- A pass ---- */

typedef struct {
    uint32_t r[16];                 /* D0-D7, A0-A7 */
    uint32_t u[8];                  /* the D registers' unstable bits */
    uint8_t ccr, uf;                /* the flags (F_*), those that rest on unstable bits */
    int until;
    struct stk *stk;                /* the pass's pushes (pass()'s own: not copied with the state) */
} st_t;

typedef struct stk {
    uint8_t n;
    uint8_t size[STACK];
    uint32_t addr[STACK], val[STACK];
} stk_t;

static inline uint32_t size_mask(int size)
{
    return size == 4 ? 0xFFFFFFFF : size == 2 ? 0xFFFF : 0xFF;
}

/* the condition, and whether it rests on an unstable flag */
static bool condition(int cc, int f, bool *unstable, int uf)
{
    static const uint8_t uses[16] = {
        0, 0, F_C | F_Z, F_C | F_Z, F_C, F_C, F_Z, F_Z,
        F_V, F_V, F_N, F_N, F_N | F_V, F_N | F_V, F_N | F_V | F_Z, F_N | F_V | F_Z,
    };
    bool n = f & F_N, z = f & F_Z, v = f & F_V, c = f & F_C;
    *unstable = (uses[cc] & uf) != 0;
    switch (cc) {
    case 0x0: return true;
    case 0x1: return false;
    case 0x2: return !c && !z;
    case 0x3: return c || z;
    case 0x4: return !c;
    case 0x5: return c;
    case 0x6: return !z;
    case 0x7: return z;
    case 0x8: return !v;
    case 0x9: return v;
    case 0xA: return !n;
    case 0xB: return n;
    case 0xC: return n == v;
    case 0xD: return n != v;
    case 0xE: return n == v && !z;
    default:  return n != v || z;
    }
}

/* N and Z from a result, V and C cleared (X as it was) */
static inline void logic_flags(st_t *s, uint32_t v, uint32_t u, int size)
{
    uint32_t m = size_mask(size), msb = m ^ (m >> 1);
    s->ccr = (s->ccr & F_X) | (v & msb ? F_N : 0) | (v & m ? 0 : F_Z);
    s->uf = (s->uf & F_X) | (u & m ? F_N | F_Z : 0);
}

static inline void set_d(st_t *s, int reg, uint32_t v, uint32_t u, int size)
{
    uint32_t m = size_mask(size);
    s->r[reg] = (s->r[reg] & ~m) | (v & m);
    s->u[reg] = (s->u[reg] & ~m) | (u & m);
}

static bool push(st_t *s, uint32_t v, int size)
{
    stk_t *k = s->stk;
    if (k->n == STACK) {
        return false;
    }
    s->r[15] -= size;
    k->addr[k->n] = s->r[15] & 0xFFFFFF;
    k->size[k->n] = size;
    k->val[k->n++] = v & size_mask(size);
    return true;
}

/* A read of size bytes at a: the stack's pushes in this pass, else memory */
static bool load(st_t *s, uint32_t a, int size, uint32_t *v, uint32_t *u)
{
    a &= 0xFFFFFF;
    const stk_t *st = s->stk;
    for (int k = st->n - 1; k >= 0; k--) {
        if (st->addr[k] == a && st->size[k] == size) {
            *v = st->val[k];
            *u = 0;
            return true;
        }
        if (a < st->addr[k] + st->size[k] && st->addr[k] < a + size) {
            return false;           /* (part of one) */
        }
    }
    int until;
    if ((size > 1 && (a & 1)) || !mem_peek(a, size, v, &until, u)) {
        return false;
    }
    if (until < s->until) {
        s->until = until;
    }
    return true;
}

static bool read_opnd(st_t *s, const opnd_t *o, int size, uint32_t *v, uint32_t *u)
{
    uint32_t a;
    *u = 0;
    switch (o->kind) {
    case O_DN:
        *v = s->r[o->reg] & size_mask(size);
        *u = s->u[o->reg] & size_mask(size);
        return true;
    case O_AN:
        *v = s->r[8 + o->reg] & size_mask(size);
        return true;
    case O_IMM:
        *v = o->val;
        return true;
    case O_POP:
        a = s->r[15];
        s->r[15] += size;
        break;
    case O_IND:
        a = s->r[8 + o->reg];
        break;
    case O_D16:
        a = s->r[8 + o->reg] + o->disp;
        break;
    default:
        a = o->val;
        break;
    }
    return load(s, a, size, v, u);
}

static uint32_t sign_extend(uint32_t v, int size)
{
    return size == 4 ? v : size == 2 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
}

/* One instruction; *next its successor's index (or BACK, EXIT). False: it
 * can't be worked out here */
static bool step(st_t *s, const insn_t *in, int i, int *next)
{
    uint32_t v, u, d, m = size_mask(in->size), msb = m ^ (m >> 1);
    bool unstable;
    *next = i + 1;
    switch (in->op) {
    case I_NOP:
        return true;
    case I_MOVE:
        if (!read_opnd(s, &in->src, in->size, &v, &u)) {
            return false;
        }
        set_d(s, in->reg, v, u, in->size);
        logic_flags(s, v, u, in->size);
        return true;
    case I_MOVEA:
        if (!read_opnd(s, &in->src, in->size, &v, &u) || u) {
            return false;
        }
        s->r[8 + in->reg] = sign_extend(v, in->size);
        return true;
    case I_PUSH:
        if (!read_opnd(s, &in->src, in->size, &v, &u) || u || !push(s, v, in->size)) {
            return false;
        }
        logic_flags(s, v, 0, in->size);
        return true;
    case I_MOVEQ:
        set_d(s, in->reg, in->imm, 0, 4);
        logic_flags(s, in->imm, 0, 4);
        return true;
    case I_PUSHM:
        for (int r = 15; r >= 0; r--) {
            if (in->imm & (0x8000 >> r)) {
                if ((r < 8 && s->u[r]) || !push(s, s->r[r], 4)) {
                    return false;
                }
            }
        }
        return true;
    case I_POPM:
        for (int r = 0; r < 16; r++) {
            if (in->imm & (1 << r)) {
                if (!load(s, s->r[15], 4, &v, &u) || u) {
                    return false;
                }
                s->r[15] += 4;
                s->r[r] = v;
                if (r < 8) {
                    s->u[r] = 0;
                }
            }
        }
        return true;
    case I_EXT: {
        int from = in->sub == 2 ? 1 : in->sub == 4 ? 2 : 1, to = in->sub == 2 ? 2 : 4;
        uint32_t fm = size_mask(from), fmsb = fm ^ (fm >> 1);
        d = s->r[in->reg];
        u = s->u[in->reg];
        v = sign_extend(d & fm, from);
        u = (u & fm) | (u & fmsb ? size_mask(to) & ~fm : 0);
        set_d(s, in->reg, v, u, to);
        logic_flags(s, v, u, to);
        return true;
    }
    case I_SWAP:
        d = s->r[in->reg];
        u = s->u[in->reg];
        v = d << 16 | d >> 16;
        u = u << 16 | u >> 16;
        set_d(s, in->reg, v, u, 4);
        logic_flags(s, v, u, 4);
        return true;
    case I_AND:
    case I_OR: {
        uint32_t sv, su;
        if (!read_opnd(s, &in->src, in->size, &sv, &su)) {
            return false;
        }
        d = s->r[in->reg] & m;
        uint32_t du = s->u[in->reg] & m;
        if (in->op == I_AND) {
            v = d & sv;
            u = (du & (sv | su)) | (su & (d | du));     /* (a stable 0 on either side: a stable 0) */
        } else {
            v = d | sv;
            u = (du & ~(sv & ~su)) | (su & ~(d & ~du)); /* (a stable 1: a stable 1) */
        }
        set_d(s, in->reg, v, u, in->size);
        logic_flags(s, v, u, in->size);
        return true;
    }
    case I_SHIFT: {
        int n = (int)in->imm, bits = 8 * in->size;
        d = s->r[in->reg] & m;
        u = s->u[in->reg] & m;
        int out_bit = in->sub == SH_LSL ? bits - n : n - 1;    /* (the last bit shifted out) */
        uint32_t out = (d >> out_bit) & 1, out_u = (u >> out_bit) & 1;
        if (in->sub == SH_LSL) {
            v = (d << n) & m;
            u = (u << n) & m;
        } else if (in->sub == SH_ASR) {
            v = (uint32_t)((int32_t)sign_extend(d, in->size) >> n) & m;
            u = (uint32_t)((int32_t)sign_extend(u, in->size) >> n) & m;
        } else {
            v = d >> n;
            u >>= n;
        }
        set_d(s, in->reg, v, u, in->size);
        logic_flags(s, v, u, in->size);
        s->ccr = (s->ccr & ~(F_X | F_C)) | (out ? F_X | F_C : 0);
        s->uf = (s->uf & ~(F_X | F_C)) | (out_u ? F_X | F_C : 0);
        return true;
    }
    case I_TST:
        if (!read_opnd(s, &in->src, in->size, &v, &u)) {
            return false;
        }
        logic_flags(s, v, u, in->size);
        return true;
    case I_CMP:
    case I_CMPI: {
        uint32_t dst, src, du, su, r;
        if (in->op == I_CMP) {
            if (!read_opnd(s, &in->src, in->size, &src, &su)) {
                return false;
            }
            dst = s->r[in->reg] & m;
            du = s->u[in->reg] & m;
        } else {
            if (!read_opnd(s, &in->src, in->size, &dst, &du)) {
                return false;
            }
            src = in->imm & m;
            su = 0;
        }
        r = (dst - src) & m;
        s->ccr = (s->ccr & F_X) | (r & msb ? F_N : 0) | (r ? 0 : F_Z) |
                 (((dst ^ src) & (dst ^ r) & msb) ? F_V : 0) | (src > dst ? F_C : 0);
        s->uf = (s->uf & F_X) | ((du | su) ? F_N | F_Z | F_V | F_C : 0);
        return true;
    }
    case I_BTST: {
        if (!read_opnd(s, &in->src, in->size, &v, &u)) {
            return false;
        }
        int bit = in->sub ? (int)(s->r[in->reg] & (in->size == 4 ? 31 : 7)) : (int)in->imm;
        if (in->sub && (s->u[in->reg] & (in->size == 4 ? 31 : 7))) {
            return false;
        }
        s->ccr = (s->ccr & ~F_Z) | ((v >> bit) & 1 ? 0 : F_Z);
        s->uf = (s->uf & ~F_Z) | ((u >> bit) & 1 ? F_Z : 0);
        return true;
    }
    case I_SCC: {
        bool t = condition(in->cc, s->ccr, &unstable, s->uf);
        if (unstable) {
            return false;
        }
        set_d(s, in->reg, t ? 0xFF : 0, 0, 1);
        return true;
    }
    case I_BCC: {
        bool t = condition(in->cc, s->ccr, &unstable, s->uf);
        if (unstable) {
            return false;
        }
        if (t) {
            *next = in->next;
        }
        return true;
    }
    default:
        return false;
    }
}

/* From instruction i to the end of the pass: true when it branches back to
 * the loop's start, with the stack's pushes popped or the same as memory */
static bool pass_on(const loop_t *l, st_t *s, int i, const stk_t *pushes)
{
    for (int steps = 0; steps < 2 * MAX_INSNS; steps++) {
        int next;
        if (!step(s, &l->in[i], i, &next)) {
            return false;
        }
        if (next == BACK) {
            for (int k = 0; k < pushes->n; k++) {
                uint32_t v, u;
                int until;
                if (pushes->addr[k] >= (s->r[15] & 0xFFFFFF) &&
                    (!mem_peek(pushes->addr[k], pushes->size[k], &v, &until, &u) || v != pushes->val[k])) {
                    return false;   /* (a write above the stack pointer that changes memory) */
                }
            }
            return true;
        }
        if (next == EXIT || next >= l->n) {
            return false;
        }
        i = next;
    }
    return false;
}

static bool pass(const loop_t *l, st_t *s, int i)
{
    stk_t pushes;
    pushes.n = 0;
    s->stk = &pushes;
    bool ok = pass_on(l, s, i, &pushes);
    s->stk = NULL;
    return ok;
}

static void load_state(st_t *s, const loop_t *l, uint32_t sr)
{
    s->ccr = sr & 0x1F;
    s->uf = 0;
    s->until = NEVER;
    memset(s->u, 0, sizeof(s->u));
    for (uint32_t m = l->regs | l->written; m; m &= m - 1) {
        int r = __builtin_ctz(m);
        s->r[r] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + r));
    }
}

/* The registers the loop writes and the flags as they were (unstable bits
 * apart) */
static bool same(const st_t *a, const st_t *b, uint16_t written)
{
    for (uint32_t m = written; m; m &= m - 1) {
        int r = __builtin_ctz(m);
        uint32_t ignore = r < 8 ? a->u[r] | b->u[r] : 0;
        if ((a->r[r] ^ b->r[r]) & ~ignore) {
            return false;
        }
    }
    return ((a->ccr ^ b->ccr) & ~(a->uf | b->uf)) == 0;
}

/* Would the loop go on as it is, every pass the same? In *fixed the state
 * at the loop's start from which every pass comes back to it: the CPU's
 * own, or after the rest of this pass and at most one pass more, which the
 * CPU would run next with memory as it is (*moved: the CPU must be set to
 * it: the beam wait's register holds the line before's position as a line
 * begins). The time it may stay so in *until (NEVER: as long as the
 * slice). */
static bool loops(const loop_t *l, uint32_t pc, uint32_t sr, int *until, st_t *fixed, bool *moved)
{
    st_t s;
    load_state(&s, l, sr);
    *moved = pc != l->t;
    if (pc != l->t) {
        /* (the rest of this pass, to the loop's start: pc's instruction is
         * the one after as many as start before it) */
        int i = __builtin_popcountll(l->starts & ((1ull << ((pc - l->t) / 2)) - 1));
        if (!pass(l, &s, i)) {
            return false;
        }
    }
    for (int k = 0; k < 2; k++) {
        *fixed = s;
        if (!pass(l, &s, 0)) {
            return false;
        }
        if (same(&s, fixed, l->written)) {
            *until = s.until;
            return true;
        }
        *moved = true;
    }
    return false;
}

/* The CPU to the loop's start with the state there */
static void commit(const loop_t *l, const st_t *s, uint32_t sr)
{
    for (uint32_t m = l->written; m; m &= m - 1) {
        int r = __builtin_ctz(m);
        m68k_set_reg((m68k_register_t)(M68K_REG_D0 + r), s->r[r]);
    }
    if ((sr & 0x1F) != s->ccr) {
        m68k_set_reg(M68K_REG_SR, (sr & ~0x1Fu) | s->ccr);
    }
    m68k_set_reg(M68K_REG_PC, l->t);
}

/* ---- Finding them ---- */

/* A kept loop with pc in it and its code as decoded */
static bool kept(loop_t *l, uint32_t pc)
{
    if (!l->nwords || pc - l->t >= l->end - l->t || !(l->starts & (1ull << ((pc - l->t) / 2)))) {
        return false;
    }
    for (int k = 0; k < l->nwords; k++) {
        if (word_at(l->t + 2 * k) != l->words[k]) {
            l->nwords = 0;          /* (other code now) */
            return false;
        }
    }
    return true;
}

/* The loop pc is in: the last one found, a PC known to be in none this
 * frame, another kept one, else decoded (NULL: none). (Code that loops
 * otherwise starts its slices near one PC too: answered from the table,
 * about a point of core 1 on an A500 title before.) */
static const loop_t *find(uint32_t pc)
{
    if (s_w.last && kept(s_w.last, pc)) {
        return s_w.last;
    }
    int i = (pc >> 1) & (NOT_LOOPS - 1);
    if (s_w.not_pc[i] == pc + 1) {
        return NULL;
    }
    for (int k = 0; k < LOOPS; k++) {
        if (kept(&s_w.loop[k], pc)) {
            return s_w.last = &s_w.loop[k];
        }
    }
    loop_t *l = &s_w.loop[s_w.next];
    if (!decode(l, pc)) {
        l->nwords = 0;
        s_w.not_pc[i] = pc + 1;
        return NULL;
    }
    s_w.next = (s_w.next + 1) % LOOPS;
    return s_w.last = l;
}

/* (code changes: the PCs found to be in no loop are forgotten each frame) */
void wait_frame(void)
{
    memset(s_w.not_pc, 0, sizeof(s_w.not_pc));
}

int wait_skip(int until, bool *in_loop)
{
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
    /* (where the last check found the CPU, or no loop yet) */
    bool near = pc - s_w.last_pc + 2 * MAX_WORDS <= 4 * MAX_WORDS;
    s_w.last_pc = pc;
    const loop_t *l = near ? find(pc) : NULL;
    *in_loop = l != NULL;
    if (!l) {
        return ami.cycle;
    }
    uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    int limit;
    st_t fixed;
    bool moved;
    if ((sr & 0xC000) || !loops(l, pc, sr, &limit, &fixed, &moved)) {
        return ami.cycle;           /* (tracing: each instruction an exception) */
    }
    /* (after the peeks: a CIA brought up to date may have raised one) */
    if (ami.ipl == 7 || ami.ipl > (int)((sr >> 8) & 7)) {
        return ami.cycle;           /* an interrupt comes as the slice starts */
    }
    int to = limit < until ? (limit > ami.cycle ? limit : ami.cycle) : until;
    if (to > ami.cycle && moved) {
        commit(l, &fixed, sr);
    }
    return to;
}

/* custom.c's polls, from the read in the instruction at pc, the loop's
 * first: will the loop go on with the registers as they are now? (On the
 * pass that leaves it, the slice's rest belongs to the code after it:
 * Pango Fun's wait for line 0, V8 then the vertical byte, read V8 at the
 * slice's end, and VERTB's interrupt came before the second test, every
 * frame.)
 */
bool wait_poll_loops(uint32_t pc)
{
    pc &= 0xFFFFFF;
    const loop_t *l = find(pc);
    int limit;
    st_t fixed;
    bool moved;
    return l && l->t == pc && loops(l, pc, m68k_get_reg(NULL, M68K_REG_SR), &limit, &fixed, &moved) &&
           limit == NEVER;
}
