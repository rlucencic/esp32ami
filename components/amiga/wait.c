/*
 * Wait loops: a test of memory and a conditional branch back to it, or a
 * branch to itself, the way programs wait for a flag their interrupt code
 * sets, a line of the beam, a CIA's timer or a button. On the A1200 such
 * loops took most of core 1 in titles that fell behind (TODO 18,
 * docs/a1200-load-survey-2026-11-25.md: Soccer Kid 82% of its
 * instructions, Fatman 74-99%, Black Viper 64-99%).
 *
 * What they test changes only where the CPU's slice ends: RAM through the
 * CPU itself (its interrupts come as a slice starts), the blitter (done at
 * a write, which ends the slice) and disk DMA (at the line's end); the
 * chips' registers custom_peek() takes at events and lines' ends; a CIA's
 * port, TOD and serial data at lines' ends. A CIA timer polled with its
 * interrupt off underflows anywhere: a skip on ICR, CRA or CRB ends there.
 *
 * As a slice starts where the last check found the CPU (within a few
 * bytes; amiga.c checks as a line begins, and in the line's other slices
 * while the CPU is in a loop), the code there is decoded once (kept with
 * its words, compared on each use: code changes) and the test worked out
 * from memory as it is, without the reads' side effects: if the branch
 * will be taken and no interrupt is due, the slice passes without the
 * CPU. A loop runs for a line or two before it's found, and the slice in
 * which it ends runs.
 */
#include <string.h>
#include "m68k.h"
#include "amiga_internal.h"

enum { K_SELF, K_TST, K_MOVE, K_BTST, K_CMPI, K_CMP };     /* the test */
enum { EA_DN, EA_IND, EA_D16, EA_FIXED };                   /* its operand */

#define MAX_WORDS       9           /* the test (up to 7 words) and the Bcc (up to 2) */

typedef struct {
    uint32_t t, b;                  /* the test's address, the branch's (t == b: a branch to itself) */
    uint16_t words[MAX_WORDS];      /* the code from t, as decoded */
    uint8_t nwords;
    uint8_t kind, size, ea, reg;    /* the test, its size (1, 2, 4), operand, its register */
    uint8_t dn;                     /* CMP's register, BTST Dn's */
    int8_t bit;                     /* BTST #n's bit (-1: Dn's) */
    uint8_t cc;                     /* the Bcc's condition */
    int32_t disp;                   /* (d16,An)'s */
    uint32_t addr;                  /* abs.W, abs.L, (d16,PC): the address */
    uint32_t imm;                   /* CMPI's */
} loop_t;

#define LOOPS           8
#define NOT_LOOPS       16
static struct {
    loop_t loop[LOOPS];
    int next;                       /* the entry the next decoded loop replaces */
    loop_t *last;                   /* the last one found */
    uint32_t not_pc[NOT_LOOPS];     /* PCs found not to be loops this frame + 1, by their low bits */
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

/* The operand's extension words at e: their count in bytes, or -1 */
static int decode_ea(loop_t *l, int mode, int reg, uint32_t e)
{
    l->reg = reg;
    switch (mode) {
    case 0:
        l->ea = EA_DN;
        return 0;
    case 2:
        l->ea = EA_IND;
        return 0;
    case 5:
        l->ea = EA_D16;
        l->disp = (int16_t)word_at(e);
        return 2;
    case 7:
        l->ea = EA_FIXED;
        if (reg == 0) {
            l->addr = (uint32_t)(int32_t)(int16_t)word_at(e) & 0xFFFFFF;
            return 2;
        }
        if (reg == 1) {
            l->addr = long_at(e) & 0xFFFFFF;
            return 4;
        }
        if (reg == 2) {
            l->addr = (e + (int16_t)word_at(e)) & 0xFFFFFF;
            return 2;
        }
        return -1;
    default:
        return -1;                  /* (An), postincrement and predecrement, indexed: not here */
    }
}

/* The test at t: its length in bytes, or 0 */
static int decode_test(loop_t *l, uint32_t t)
{
    uint16_t op = word_at(t);
    int mode = (op >> 3) & 7, reg = op & 7, len, ext;
    static const uint8_t sizes[3] = { 1, 2, 4 };
    if ((op & 0xFF00) == 0x4A00 && ((op >> 6) & 3) != 3) {
        l->kind = K_TST;                            /* TST <ea> */
        l->size = sizes[(op >> 6) & 3];
        len = 2;
        if (mode == 7 && reg == 2 && !ami.cpu_shift) {
            return 0;                               /* (the 68020's) */
        }
    } else if ((op & 0xFFC0) == 0x0800) {
        l->kind = K_BTST;                           /* BTST #n,<ea> */
        l->bit = (int8_t)(word_at(t + 2) & (mode == 0 ? 31 : 7));
        l->size = mode == 0 ? 4 : 1;
        len = 4;
    } else if ((op & 0xF1C0) == 0x0100 && mode != 1) {
        l->kind = K_BTST;                           /* BTST Dn,<ea> */
        l->bit = -1;
        l->dn = (op >> 9) & 7;
        l->size = mode == 0 ? 4 : 1;
        len = 2;
    } else if ((op & 0xFF00) == 0x0C00 && ((op >> 6) & 3) != 3) {
        l->kind = K_CMPI;                           /* CMPI #imm,<ea> */
        l->size = sizes[(op >> 6) & 3];
        l->imm = l->size == 4 ? long_at(t + 2) : l->size == 2 ? word_at(t + 2) : word_at(t + 2) & 0xFF;
        len = l->size == 4 ? 6 : 4;
        if (mode == 7 && reg == 2 && !ami.cpu_shift) {
            return 0;
        }
    } else if ((op & 0xF000) == 0xB000 && ((op >> 6) & 7) < 3) {
        l->kind = K_CMP;                            /* CMP <ea>,Dn */
        l->size = sizes[(op >> 6) & 7];
        l->dn = (op >> 9) & 7;
        len = 2;
    } else if ((op & 0xC000) == 0 && (op & 0x3000) && ((op >> 6) & 7) == 0) {
        l->kind = K_MOVE;                           /* MOVE <ea>,Dn */
        l->size = (op & 0x3000) == 0x1000 ? 1 : (op & 0x3000) == 0x3000 ? 2 : 4;
        len = 2;
    } else {
        return 0;
    }
    ext = decode_ea(l, mode, reg, t + len);
    if (ext < 0 || (l->ea == EA_FIXED && l->size > 1 && (l->addr & 1))) {
        return 0;
    }
    return len + ext;
}

/* A Bcc at b: its target (its condition in *cc, its length in *len), or 1
 * (odd: none) */
static uint32_t branch_target(uint32_t b, uint8_t *cc, int *len)
{
    uint16_t op = word_at(b);
    if ((op & 0xF000) != 0x6000 || ((op >> 8) & 0xF) == 1) {
        return 1;                   /* (not a branch; BSR) */
    }
    *cc = (op >> 8) & 0xF;
    int8_t d8 = (int8_t)(op & 0xFF);
    if (d8 == -1) {
        return 1;                   /* (the 68020's 32-bit displacement) */
    }
    *len = d8 ? 2 : 4;
    return (b + 2 + (d8 ? d8 : (int16_t)word_at(b + 2))) & 0xFFFFFF;
}

/* The loop pc is in, decoded (false: none) */
static bool decode(loop_t *l, uint32_t pc)
{
    int len, blen;
    uint8_t cc;
    memset(l, 0, sizeof(*l));
    uint32_t target = branch_target(pc, &cc, &blen);
    if (target == pc) {
        l->kind = K_SELF;           /* bra.s *, or a Bcc to itself */
        l->t = l->b = pc;
    } else if (target & 1) {
        len = decode_test(l, pc);
        if (!len || branch_target(pc + len, &cc, &blen) != pc || cc == 0) {
            return false;           /* (a BRA back is no wait: the test doesn't decide) */
        }
        l->t = pc;
        l->b = pc + len;
    } else {
        if (cc == 0 || target > pc || pc - target > 2 * MAX_WORDS) {
            return false;
        }
        len = decode_test(l, target);
        if (!len || target + len != pc) {
            return false;
        }
        l->t = target;
        l->b = pc;
    }
    l->cc = cc;
    l->nwords = (l->b + blen - l->t) / 2;
    if (l->nwords > MAX_WORDS) {
        return false;
    }
    for (int i = 0; i < l->nwords; i++) {
        l->words[i] = word_at(l->t + 2 * i);
    }
    return true;
}

static bool condition(int cc, int n, int z, int v, int c)
{
    switch (cc) {
    case 0x0: return true;              /* BRA */
    case 0x2: return !c && !z;          /* HI */
    case 0x3: return c || z;            /* LS */
    case 0x4: return !c;                /* CC */
    case 0x5: return c;                 /* CS */
    case 0x6: return !z;                /* NE */
    case 0x7: return z;                 /* EQ */
    case 0x8: return !v;                /* VC */
    case 0x9: return v;                 /* VS */
    case 0xA: return !n;                /* PL */
    case 0xB: return n;                 /* MI */
    case 0xC: return n == v;            /* GE */
    case 0xD: return n != v;            /* LT */
    case 0xE: return n == v && !z;      /* GT */
    case 0xF: return n != v || z;       /* LE */
    default:  return false;
    }
}

static inline uint32_t reg_d(int n)
{
    return m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + n));
}

static inline uint32_t reg_a(int n)
{
    return m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + n));
}

/* Will the loop branch back once more, with memory as it is? The time it
 * may stay so in *until (NEVER: as long as the slice) */
static bool loops(const loop_t *l, uint32_t pc, uint32_t sr, int *until)
{
    int n = (sr >> 3) & 1, z = (sr >> 2) & 1, v = (sr >> 1) & 1, c = sr & 1;
    *until = NEVER;
    if (pc == l->b && !condition(l->cc, n, z, v, c)) {
        return false;               /* (at the branch: the flags the test left) */
    }
    if (l->kind == K_SELF) {
        return true;
    }
    uint32_t val;
    if (l->ea == EA_DN) {
        val = reg_d(l->reg);
    } else {
        uint32_t a = l->ea == EA_FIXED ? l->addr :
                     (reg_a(l->reg) + (l->ea == EA_D16 ? l->disp : 0)) & 0xFFFFFF;
        if ((l->size > 1 && (a & 1)) || !mem_peek(a, l->size, &val, until)) {
            return false;
        }
    }
    uint32_t mask = l->size == 4 ? 0xFFFFFFFF : l->size == 2 ? 0xFFFF : 0xFF;
    uint32_t msb = mask ^ (mask >> 1);
    val &= mask;
    switch (l->kind) {
    case K_TST:
    case K_MOVE:
        n = (val & msb) != 0;
        z = val == 0;
        v = c = 0;
        break;
    case K_BTST: {
        int bit = l->bit >= 0 ? l->bit : (int)(reg_d(l->dn) & (l->size == 4 ? 31 : 7));
        z = !((val >> bit) & 1);
        break;                      /* (N, V, C as they were) */
    }
    default: {                      /* CMPI: <ea> - imm; CMP: Dn - <ea> */
        uint32_t dst = l->kind == K_CMPI ? val : reg_d(l->dn) & mask;
        uint32_t src = l->kind == K_CMPI ? l->imm & mask : val;
        uint32_t r = (dst - src) & mask;
        n = (r & msb) != 0;
        z = r == 0;
        v = ((dst ^ src) & (dst ^ r) & msb) != 0;
        c = src > dst;
        break;
    }
    }
    return condition(l->cc, n, z, v, c);
}

/* A kept loop at pc with its code as decoded */
static bool kept(loop_t *l, uint32_t pc)
{
    if (!l->nwords || (pc != l->t && pc != l->b)) {
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

/* The loop pc is in: the last one found, a PC known to be none this frame,
 * another kept one, else decoded (NULL: none). (Code that loops otherwise
 * starts its slices near one PC too: answered from the table, about a
 * point of core 1 on an A500 title before.) */
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

/* (code changes: the PCs found to be no loop are forgotten each frame) */
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
    if ((sr & 0xC000) || !loops(l, pc, sr, &limit)) {
        return ami.cycle;           /* (tracing: each instruction an exception) */
    }
    /* (after the peeks: a CIA brought up to date may have raised one) */
    if (ami.ipl == 7 || ami.ipl > (int)((sr >> 8) & 7)) {
        return ami.cycle;           /* an interrupt comes as the slice starts */
    }
    return limit < until ? (limit > ami.cycle ? limit : ami.cycle) : until;
}

/* custom.c's polls, from the read in the test at pc: will the loop go on
 * with the register as it is now? (On the pass that leaves it, the slice's
 * rest belongs to the code after it: Pango Fun's wait for line 0, V8 then
 * the vertical byte, read V8 at the slice's end, and VERTB's interrupt
 * came before the second test, every frame.)
 */
bool wait_poll_loops(uint32_t pc)
{
    const loop_t *l = find(pc & 0xFFFFFF);
    int limit;
    return l && l->t == (pc & 0xFFFFFF) && l->kind != K_SELF &&
           loops(l, l->t, m68k_get_reg(NULL, M68K_REG_SR), &limit) && limit == NEVER;
}
