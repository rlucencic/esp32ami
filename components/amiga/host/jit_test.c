/* The translated blocks against the interpreter, an instruction at a time
 * (TODO 19, stage 3): random instructions of the families jit_rv.c
 * translates (and their invalid neighbours, left to the handlers), from
 * random registers, flags, fetch state and instruction cache, in chip RAM
 * (its fetch waits) and in the ROM (none). Each case runs once through
 * mem_interp_one() and once through jit_run() from the same state; the
 * registers, SR, PC, PPC, IR, the cycles left, the fetch state, the cache
 * tags and chip RAM must come out the same. Then the translated code
 * again without a flush (the exits the first run linked jump from block to
 * block now) and, in chip RAM, after the word behind the instruction was
 * written over (its block dropped: a link to it goes back to the
 * dispatcher, which links the new one). A stub ROM, no Kickstart.
 *   jit_test [cases] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_internal.h"
#include "jit.h"
#include "m68k.h"

#define CHIP        (2048 * 1024)
#define CODE_CHIP   0x1F0000u
#define CODE_ROM    0xF80100u

static uint8_t chip[CHIP], kick[256 * 1024], rom[256 * 1024];
static uint8_t snap[CHIP], after_interp[CHIP];
static uint32_t s_rng;
static int failures, natives, cases_run;

void m68k_host_instruction(unsigned int pc)
{
    (void)pc;
}

void ami_host_watch_hit(uint32_t addr, int bytes, uint32_t value)
{
    (void)addr, (void)bytes, (void)value;
}

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t rn(uint32_t n)
{
    return rnd() % n;
}

static void make_rom(void)
{
    memset(kick, 0, sizeof(kick));
    kick[0] = kick[1] = 0x11;
    kick[5] = 0xF8;
    kick[7] = 0x10;
    kick[12] = 0x00, kick[13] = 0x28;           /* (40.x) */
    kick[15] = 0x44;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < sizeof(kick); i += 4) {
        uint32_t w = ((uint32_t)kick[i] << 24) | (kick[i + 1] << 16) | (kick[i + 2] << 8) | kick[i + 3];
        uint32_t t = sum + w;
        sum = t + (t < sum);
    }
    uint32_t fix = 0xFFFFFFFF - sum;
    for (int k = 0; k < 4; k++) {
        kick[sizeof(kick) - 24 + k] = fix >> (24 - 8 * k);
    }
}

/* ---- Instructions ---- */

typedef struct {
    uint16_t w[12];
    int n;
} insn_t;

static void put(insn_t *i, uint16_t w)
{
    if (i->n < 12) {
        i->w[i->n++] = w;
    }
}

/* a brief index word with a small index register (D4-D7, A0-A7) */
static uint16_t index_word(void)
{
    uint32_t xn = 4 + rn(12);
    /* (an address register as a word only: An + Am * 8 would reach the chips,
     * whose state the cases don't restore) */
    uint32_t l = xn >= 8 ? 0 : rn(2);
    return (uint16_t)(xn << 12 | l << 11 | rn(4) << 9 | (rnd() & 0xFF));
}

/* an EA's mode/reg field and its extension words; data: Dn allowed,
 * alt: alterable only */
static uint16_t ea(insn_t *i, int size, bool data, bool an, bool alt, bool control)
{
    for (;;) {
        int mode = (int)rn(8), reg = (int)rn(8);
        if (mode == 7) {
            reg = (int)rn(5);
        }
        if (!data && mode == 0) continue;
        if (!an && mode == 1) continue;
        if (alt && mode == 7 && reg > 1) continue;
        if (control && (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4))) continue;
        switch (mode) {
        case 5:
            put(i, (uint16_t)rnd());
            break;
        case 6:
            put(i, index_word());
            break;
        case 7:
            switch (reg) {
            case 0: put(i, (uint16_t)(0x2000 + rn(0x5FFF))); break;
            case 1: { uint32_t a = 0x40000 + rn(0x180000); put(i, (uint16_t)(a >> 16)); put(i, (uint16_t)a); break; }
            case 2: put(i, (uint16_t)(rnd() & 0x7FFF) - 0x4000); break;
            case 3: put(i, index_word()); break;
            case 4:
                if (size == 4) {
                    put(i, (uint16_t)rnd());
                }
                put(i, (uint16_t)rnd());
                break;
            }
            break;
        }
        return (uint16_t)(mode << 3 | reg);
    }
}

static int rsize(void)
{
    return (int)rn(3);                          /* 0 byte, 1 word, 2 long */
}

static int bytes(int s)
{
    return s == 0 ? 1 : s == 1 ? 2 : 4;
}

static void gen(insn_t *i)
{
    memset(i, 0, sizeof(*i));
    int s = rsize(), b = bytes(s);
    uint16_t op;
    put(i, 0);                                  /* (the opcode, below) */
    switch (rn(30)) {
    case 0: case 1: case 2: {                   /* MOVE, MOVEA */
        static const int sz[3] = { 1, 3, 2 };
        uint16_t src = ea(i, b, true, true, false, false);
        bool movea = rn(5) == 0;
        uint16_t dst;
        if (movea) {
            dst = (uint16_t)(1 << 3 | rn(8));
        } else {
            dst = ea(i, b, true, false, true, false);
        }
        op = (uint16_t)(sz[s] << 12 | (dst & 7) << 9 | (dst >> 3) << 6 | src);
        break;
    }
    case 3:
        op = (uint16_t)(0x7000 | rn(8) << 9 | (rnd() & 0x1FF & (rn(8) ? 0xFF : 0x1FF)));
        break;
    case 4:                                     /* LEA, PEA */
        op = rn(2) ? (uint16_t)(0x41C0 | rn(8) << 9 | ea(i, 4, false, false, false, true))
                   : (uint16_t)(0x4840 | ea(i, 4, false, false, false, true));
        break;
    case 5: {                                   /* TST, CLR, NEG, NOT */
        static const uint16_t base[4] = { 0x4A00, 0x4200, 0x4400, 0x4600 };
        int k = (int)rn(4);
        op = (uint16_t)(base[k] | s << 6 | ea(i, b, true, k == 0, k != 0, false));
        break;
    }
    case 6: {                                   /* EXT, EXTB, SWAP */
        static const uint16_t base[4] = { 0x4880, 0x48C0, 0x49C0, 0x4840 };
        op = (uint16_t)(base[rn(4)] | rn(8));
        break;
    }
    case 7: case 8:                             /* ADDQ, SUBQ */
        op = (uint16_t)(0x5000 | rn(8) << 9 | rn(2) << 8 | s << 6 | ea(i, b, true, s != 0, true, false));
        break;
    case 9: case 10: case 11: case 12: {        /* ADD, SUB, AND, OR, CMP, EOR (<ea>,Dn / Dn,<ea>) */
        static const uint16_t base[5] = { 0xD000, 0x9000, 0xC000, 0x8000, 0xB000 };
        int k = (int)rn(5);
        bool to_ea = k == 4 ? rn(2) : rn(3) == 0;
        if (to_ea) {
            op = (uint16_t)(base[k] | rn(8) << 9 | (4 + s) << 6 | ea(i, b, k == 4, false, true, false));
        } else {
            op = (uint16_t)(base[k] | rn(8) << 9 | s << 6 | ea(i, b, true, k != 2 && k != 3 && s != 0, false, false));
        }
        break;
    }
    case 13: {                                  /* ADDA, SUBA, CMPA */
        static const uint16_t base[3] = { 0xD000, 0x9000, 0xB000 };
        bool l = rn(2);
        op = (uint16_t)(base[rn(3)] | rn(8) << 9 | (l ? 7 : 3) << 6 | ea(i, l ? 4 : 2, true, true, false, false));
        break;
    }
    case 14: case 15: {                         /* ORI ANDI SUBI ADDI EORI CMPI */
        static const uint16_t base[6] = { 0x0000, 0x0200, 0x0400, 0x0600, 0x0A00, 0x0C00 };
        int k = (int)rn(6);
        if (s == 2) {
            put(i, (uint16_t)rnd());
        }
        put(i, (uint16_t)rnd());
        op = (uint16_t)(base[k] | s << 6 | ea(i, b, true, false, k != 5, false));
        break;
    }
    case 16: {                                  /* BTST BCHG BCLR BSET */
        if (rn(2)) {
            put(i, (uint16_t)(rnd() & 0xFF));
            op = (uint16_t)(0x0800 | rn(4) << 6 | ea(i, 1, true, false, false, false));
        } else {
            op = (uint16_t)(0x0100 | rn(8) << 9 | rn(4) << 6 | ea(i, 1, true, false, false, false));
        }
        break;
    }
    case 17: case 18:                           /* shifts and rotates of Dn */
        op = (uint16_t)(0xE000 | rn(8) << 9 | rn(2) << 8 | s << 6 | (rn(6) == 0) << 5 | rn(4) << 3 | rn(8));
        break;
    case 19:                                    /* ADDX, SUBX */
        op = (uint16_t)((rn(2) ? 0xD100 : 0x9100) | rn(8) << 9 | s << 6 | (rn(6) == 0) << 3 | rn(8));
        break;
    case 20:                                    /* MULU, MULS */
        op = (uint16_t)(0xC0C0 | rn(8) << 9 | rn(2) << 8 | ea(i, 2, true, false, false, false));
        break;
    case 21: {                                  /* EXG */
        static const uint16_t m[3] = { 0x140, 0x148, 0x188 };
        op = (uint16_t)(0xC000 | rn(8) << 9 | m[rn(3)] | rn(8));
        break;
    }
    case 22:                                    /* Scc */
        op = (uint16_t)(0x50C0 | rn(16) << 8 | ea(i, 1, true, false, true, false));
        break;
    case 23:                                    /* DBcc */
        op = (uint16_t)(0x50C8 | rn(16) << 8 | rn(8));
        put(i, rn(2) ? 2 : (uint16_t)(rnd() & 0xFFFE));
        break;
    case 24: case 25: {                         /* Bcc, BRA, BSR */
        int cc = (int)rn(16);
        int form = (int)rn(4);
        if (form == 0) {
            op = (uint16_t)(0x6000 | cc << 8);  /* .W */
            put(i, rn(2) ? 2 : (uint16_t)(rnd() & 0xFFFE));
        } else if (form == 1) {
            op = (uint16_t)(0x6000 | cc << 8 | 0xFE);   /* (to itself) */
        } else {
            op = (uint16_t)(0x6000 | cc << 8 | ((rnd() & 0xFE) | 2));
        }
        break;
    }
    case 26:                                    /* JSR, JMP, RTS */
        if (rn(3) == 0) {
            op = 0x4E75;
        } else {
            op = (uint16_t)((rn(2) ? 0x4E80 : 0x4EC0) | ea(i, 4, false, false, false, true));
        }
        break;
    case 27: {                                  /* MOVEM */
        bool to_regs = rn(2), l = rn(2);
        put(i, (uint16_t)(rnd() & (rn(2) ? 0x7FFF : 0x00FF)));
        uint16_t e;
        for (;;) {
            insn_t t = *i;
            e = ea(&t, 4, false, false, !to_regs, false);
            int mode = e >> 3;
            if (mode >= 2 && !(to_regs && mode == 4) && !(!to_regs && mode == 3) && !(mode == 7 && (e & 7) > (to_regs ? 3 : 1))) {
                *i = t;
                break;
            }
        }
        op = (uint16_t)(0x4880 | to_regs << 10 | l << 6 | e);
        break;
    }
    default:                                    /* NOP, and opcodes near the families */
        op = rn(3) == 0 ? 0x4E71 : (uint16_t)(rnd() & 0xFFFF);
        if ((op & 0xFFF0) == 0x4E70 || (op & 0xF000) == 0x4000 || (op & 0xF000) == 0x0000) {
            op = 0x4E71;                        /* (no RESET, STOP, SR or random EA words) */
        }
        if ((op & 0xF000) != 0x6000 && (op & 0xF000) != 0x7000 && (op & 0xF000) != 0xE000 && (op & 0xF000) != 0xA000 &&
            (op & 0xF000) != 0xF000) {
            op = 0x4E71;
        }
        if ((op & 0xF0C0) == 0xE0C0) {
            op = 0x4E71;                        /* (memory shifts, bit fields: random words as addresses) */
        }
        break;
    }
    i->w[0] = op;
}

/* a second instruction that reads the first one's flags: Bcc, Scc, DBcc
 * (the translated code works them out from its pending flags) */
static bool add_reader(insn_t *i)
{
    if (rn(3)) {
        return false;
    }
    int cc = 2 + (int)rn(14);
    switch (rn(3)) {
    case 0: {
        int d = (int)rn(3);                     /* past the bra.s *, onto it, to itself */
        put(i, (uint16_t)(0x6000 | cc << 8 | (d == 0 ? 2 : d == 1 ? 0 : 0xFE)));
        break;
    }
    case 1:
        put(i, (uint16_t)(0x50C0 | cc << 8 | rn(8)));
        break;
    default:
        put(i, (uint16_t)(0x50C8 | cc << 8 | rn(8)));
        put(i, rn(2) ? 2 : (uint16_t)0xFFFE);   /* (past, or to itself) */
        break;
    }
    return true;
}

/* ---- State ---- */

typedef struct {
    uint32_t r[16], sr, pc, ppc, ir, ic_next, tag[64];
    int rem;
} state_t;

static jit_env_t s_env;

static void capture(state_t *s)
{
    for (int r = 0; r < 16; r++) {
        s->r[r] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + r));
    }
    s->sr = m68k_get_reg(NULL, M68K_REG_SR);
    s->pc = m68k_get_reg(NULL, M68K_REG_PC);
    s->ppc = m68k_get_reg(NULL, M68K_REG_PPC);
    s->ir = m68k_get_reg(NULL, M68K_REG_IR);
    extern int m68ki_remaining_cycles;
    s->rem = m68ki_remaining_cycles;
    s->ic_next = *s_env.ic_next;
    memcpy(s->tag, s_env.ic_tag, sizeof(s->tag));
}

typedef struct {
    uint32_t r[16], sr, pc;
    int rem, fetch;                             /* fetch: 0 reset, 1 at pc, 2 behind it */
    uint32_t cacr, tag[64];
} setup_t;

static void apply(const setup_t *u)
{
    for (int r = 0; r < 16; r++) {
        m68k_set_reg((m68k_register_t)(M68K_REG_D0 + r), u->r[r]);
    }
    m68k_set_reg(M68K_REG_SR, u->sr);
    m68k_set_reg(M68K_REG_A7, u->r[15]);
    m68k_set_reg(M68K_REG_PC, u->pc);
    extern int m68ki_remaining_cycles;
    extern unsigned int m68ki_tracing;
    m68ki_remaining_cycles = u->rem;
    m68ki_tracing = 0;
    if (u->fetch == 0) {
        mem_fetch_reset();
    } else {
        mem_fetch_state(u->fetch == 1 ? u->pc : u->pc - 2);
    }
    *s_env.ic_cacr = u->cacr;
    memcpy(s_env.ic_tag, u->tag, sizeof(u->tag));
}

/* the instruction, then bra.s *, a few (branches past the first); or a
 * loop first that runs until the cycles are gone, so the cycles left show
 * every fetch's charge (a linked entry's): 1 a nop and a bra.s to it, 2
 * twenty nops and a bra.s to the first (blocks cut at 16 instructions:
 * links into the next in sequence) */
static void place(uint32_t at, const insn_t *i, int loop)
{
    int nops = loop == 1 ? 1 : loop == 2 ? 20 : 0;
    for (int k = 0; k < i->n + 40; k++) {
        int f = k - i->n;                           /* (the filler's word) */
        uint16_t w = k < i->n ? i->w[k] : f < nops ? 0x4E71 : nops && f == nops ? (uint16_t)(0x6000 | (uint8_t)(-2 * nops - 2))
                                                                                 : 0x60FE;
        uint32_t a = at + 2 * (uint32_t)k;
        if (at >= 0xF80000) {
            *(uint16_t *)(ami.rom + ((a - 0xF80000) & (ami.rom_size - 1))) = w;
        } else {
            chip_write16(a, w);
        }
    }
}

static void show(const char *what, const insn_t *i, const setup_t *u)
{
    char text[100];
    m68k_disassemble(text, u->pc, M68K_CPU_TYPE_68EC020);
    fprintf(stderr, "FAIL %s: $%06x %s [", what, u->pc, text);
    for (int k = 0; k < i->n; k++) {
        fprintf(stderr, "%s%04x", k ? " " : "", i->w[k]);
    }
    fprintf(stderr, "] rem %d fetch %d cacr %u sr %04x\n", u->rem, u->fetch, u->cacr, u->sr);
}

static void run_interp(void)
{
    extern int m68ki_remaining_cycles;
    extern unsigned int m68ki_tracing;
    for (int k = 0; k < 1000 && m68ki_remaining_cycles > 0 && !m68ki_tracing; k++) {
        if (m68k_get_reg(NULL, M68K_REG_PC) & 1) {
            break;
        }
        mem_interp_one();
    }
}

/* chip RAM back as it was before the run, the translated code told where
 * the run changed it (its blocks there were made from the run's bytes) */
static void restore(void)
{
    for (uint32_t k = 0; k < CHIP; k++) {
        if (chip[k] != snap[k]) {
            uint32_t lo = k;
            while (k < CHIP && chip[k] != snap[k]) {
                k++;
            }
            jit_written(lo, k);
        }
    }
    memcpy(chip, snap, CHIP);
}

static bool same(const char *what, const insn_t *i, const setup_t *u, const state_t *a, const state_t *b)
{
    bool ok = true;
    char where[64];
    for (int r = 0; r < 16; r++) {
        if (a->r[r] != b->r[r]) {
            if (ok) {
                snprintf(where, sizeof(where), "%s: registers", what);
                show(where, i, u);
            }
            fprintf(stderr, "  %c%d: interp %08x jit %08x (was %08x)\n", r < 8 ? 'D' : 'A', r & 7, a->r[r], b->r[r], u->r[r]);
            ok = false;
        }
    }
    if (a->sr != b->sr || a->pc != b->pc || a->ppc != b->ppc || a->ir != b->ir || a->rem != b->rem || a->ic_next != b->ic_next) {
        if (ok) {
            snprintf(where, sizeof(where), "%s: state", what);
            show(where, i, u);
        }
        fprintf(stderr, "  interp: sr %04x pc %06x ppc %06x ir %04x rem %d next %06x\n", a->sr, a->pc, a->ppc, a->ir, a->rem, a->ic_next);
        fprintf(stderr, "  jit:    sr %04x pc %06x ppc %06x ir %04x rem %d next %06x\n", b->sr, b->pc, b->ppc, b->ir, b->rem, b->ic_next);
        ok = false;
    }
    if (memcmp(a->tag, b->tag, sizeof(a->tag))) {
        if (ok) {
            snprintf(where, sizeof(where), "%s: cache tags", what);
            show(where, i, u);
        }
        ok = false;
    }
    if (memcmp(after_interp, chip, CHIP)) {
        if (ok) {
            snprintf(where, sizeof(where), "%s: chip RAM", what);
            show(where, i, u);
        }
        for (uint32_t k = 0; k < CHIP; k += 2) {
            if (memcmp(after_interp + k, chip + k, 2)) {
                fprintf(stderr, "  at $%06x: interp %04x jit %04x\n", k, *(uint16_t *)(after_interp + k), *(uint16_t *)(chip + k));
                break;
            }
        }
        ok = false;
    }
    return ok;
}

static bool one_case(void)
{
    insn_t i;
    setup_t u;
    gen(&i);
    bool pair = add_reader(&i);
    memset(&u, 0, sizeof(u));
    u.pc = rn(3) ? CODE_CHIP : CODE_ROM;
    u.pc += 2 * rn(4);                          /* (either half of a longword) */
    for (int r = 0; r < 4; r++) {
        u.r[r] = rnd();
    }
    for (int r = 4; r < 8; r++) {
        u.r[r] = (uint32_t)((int32_t)(rnd() & 0x7FFF) - 0x4000);
    }
    for (int r = 8; r < 15; r++) {
        u.r[r] = 0x40000 + rn(0x170000);
        if (rn(4)) {
            u.r[r] &= ~1u;
        }
    }
    u.r[15] = 0x1C0000 + 2 * rn(0x4000);
    u.sr = 0x2700 | rn(32);
    bool flow = i.w[0] == 0x4E75 || (i.w[0] & 0xFF80) == 0x4E80;   /* (RTS, JSR, JMP: on past them) */
    u.rem = pair || flow ? 8 + (int)rn(120) : rn(3) ? 1 : 1 + (int)rn(60);
    u.fetch = (int)rn(3);
    static const uint32_t cacrs[4] = { 0, 1, 3, 9 };
    u.cacr = cacrs[rn(4)];
    for (int k = 0; k < 64; k++) {
        u.tag[k] = rn(4) ? 0 : (((u.pc & ~0xFFu) | (uint32_t)k * 4) | 1);
    }
    place(u.pc, &i, (int)rn(3));
    /* an RTS's or a JSR/JMP (An)'s target mostly the code after it (a
     * computed exit with a block there: its targets kept, then taken) */
    if (rn(4)) {
        uint32_t to = u.pc + 2 * (uint32_t)i.n + 2 * rn(4);
        if (i.w[0] == 0x4E75) {
            chip_write16(u.r[15], (uint16_t)(to >> 16));
            chip_write16(u.r[15] + 2, (uint16_t)to);
        } else if ((i.w[0] & 0xFF80) == 0x4E80 && (i.w[0] & 0x38) == 0x10) {
            u.r[8 + (i.w[0] & 7)] = to;
        }
    }

    memcpy(snap, chip, CHIP);
    apply(&u);
    run_interp();
    state_t a, b;
    capture(&a);
    memcpy(after_interp, chip, CHIP);

    memcpy(chip, snap, CHIP);
    jit_flush();
    apply(&u);
    jit_run();
    capture(&b);
    cases_run++;
    natives += jit_native_at(u.pc);
    bool ok = same("translated", &i, &u, &a, &b);

    /* again: the links made */
    if (ok) {
        restore();
        apply(&u);
        jit_run();
        capture(&b);
        ok = same("linked", &i, &u, &a, &b);
    }
    /* the word after it (its first BRA.S *) written over by the CPU: the
     * block there dropped, its entries killed */
    if (ok && u.pc < 0xF80000) {
        restore();
        m68k_write_memory_16(u.pc + 2 * (uint32_t)i.n, (uint16_t)(0x7E00 | rn(256)));   /* moveq #n, D7 */
        memcpy(snap, chip, CHIP);
        apply(&u);
        run_interp();
        capture(&a);
        memcpy(after_interp, chip, CHIP);
        restore();
        apply(&u);
        jit_run();
        capture(&b);
        ok = same("killed", &i, &u, &a, &b);
    }
    if (!ok) {
        failures++;
    }
    /* (both start from the same memory next time) */
    memcpy(chip, snap, CHIP);
    return ok;
}

int main(int argc, char **argv)
{
    int cases = argc > 1 ? atoi(argv[1]) : 20000;
    s_rng = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 12345;
    make_rom();
    static uint8_t *tables;
    tables = malloc(AMIGA_OPCODE_TABLES_SIZE);
    amiga_config_t cfg = {
        .model = AMIGA_A1200, .chip = chip, .chip_size = CHIP,
        .kickstart = kick, .kickstart_size = sizeof(kick), .rom = rom, .opcode_tables = tables,
    };
    if (!amiga_init(&cfg)) {
        fprintf(stderr, "FAIL: the stub ROM\n");
        return 1;
    }
    m68k_write_memory_8(0xBFE201, 3);               /* the overlay off */
    m68k_write_memory_8(0xBFE001, 2);
    mem_jit_env(&s_env);
    extern uint8_t jit_hot;
    jit_hot = 1;                                    /* (every case translated at once) */
    for (int k = 0; k < cases && failures < 20; k++) {
        one_case();
    }
    extern struct { unsigned long translations, flushes, full, drops, blocks_run, code_bytes, insns, distinct, exits, fbs, rv_words, links, kills, fills; } jit_stats;
    printf("jit_test: %d cases, %d translated, %lu links, %lu targets kept, %lu entries killed, %d failed\n", cases_run,
           natives, jit_stats.links, jit_stats.fills, jit_stats.kills, failures);
    return failures != 0;
}
