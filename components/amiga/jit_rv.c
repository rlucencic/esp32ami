/* The 68EC020's blocks translated into RISC-V (TODO 19, stage 3;
 * docs/jit-design-2026-11-27.md).
 *
 * Exact: each instruction does what Musashi's handler does, in its order:
 * its extension words' fetch timing where the handler reads them, its
 * data accesses as the callbacks do them (REG_PPC set for their slow
 * paths: the chips' poll skip reads it), its flags as Musashi's tests see
 * them, its cycles after its body; and after each instruction the block
 * stops where the interpreter's slice would (no cycles left, its code
 * written or the map changed: jit_stop). Instructions it doesn't know
 * call their handler (jit_fallback()). The 68k registers stay in
 * Musashi's state (m68ki_cpu); the flags pending in registers (below)
 * until something reads them in memory; the cycles left in s1.
 *
 * Registers: s0 &m68ki_cpu (Musashi's state, mem.c's fetch state,
 * jit_stop and jit_ctx within its reach: AMI_SDATA), s1 the cycles left,
 * s2 the tag of the block's first longword (the cache's tags from it),
 * s3 s4 s10 s11 values across calls, s5 the instruction's address
 * (REG_PPC for the memory routines' slow paths), s6 s7 s8 the pending
 * flags' source, destination and result, s9 1 while the cache fills;
 * t0-t5 and a0-a5 scratch, t6 a call's address.
 *
 * The blocks share routines at the arena's start (jit_rv_stubs()): the
 * prologue, the exit, a handler's call, the flags to memory, and the data
 * accesses, each the callback's plain memory path inline (the bank's
 * pointer and wait, jit_code_pages for a write) and the callback itself
 * for the rest.
 *
 * The opcode's fetch: an opcode reached in sequence costs a longword's
 * first word (ic_longword()), one reached by a jump its longword whatever
 * the half. A block knows how each of its instructions is reached except
 * the first and those after a handler's (the fetch state then in memory).
 * A Bcc.W or DBcc not taken skips its displacement without fetching it:
 * the next opcode then counts as reached by a jump (s_ic.next behind it).
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef AMI_JIT
#include "jit.h"
#include "rv.h"
#include "m68k.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "m68kcpu.h"
#pragma GCC diagnostic pop

#define MAX_INSNS       16
#define MAX_BYTES       256                 /* of 68k code */
#define BODY_LIMIT      2800                /* bytes of code before the exits: branches reach them */

#define OFF(f)          ((int32_t)offsetof(m68ki_cpu_core, f))
#define DREG(n)         (OFF(dar) + 4 * (n))
#define AREG(n)         (OFF(dar) + 32 + 4 * (n))

typedef struct {
    uint32_t pc, next;
    const uint16_t *host;
    uint16_t op;
    uint8_t wait;
    bool target;                            /* an internal branch comes here */
    int body;                               /* label: after its opcode's fetch */
} tinsn_t;

static struct {
    rv_t r;
    jit_block_t *b;
    tinsn_t in[MAX_INSNS];
    int n;
    jit_env_t env;
    uint32_t cpu;                           /* &m68ki_cpu */
    int32_t ic_tag_off, ic_next_off, ic_cacr_off;   /* the fetch state from s0 */
    /* the instruction being translated */
    tinsn_t *u;
    uint32_t fetch;                         /* its next word to fetch */
    bool wrote;                             /* it wrote memory: look at jit_stop */
    int64_t s5;                             /* s5's value here (-1: unknown) */
    /* how the next opcode is reached */
    bool fe_dyn;                            /* the fetch state in memory (a handler's) */
    uint32_t fe;                            /* else: the end of the last word fetched */
    /* exits and handlers */
    jit_exit_t *exit;
    int nexits, max_exits;
    int exit_label[2 * MAX_INSNS + 8];
    uint8_t exit_pk[2 * MAX_INSNS + 8], exit_ps[2 * MAX_INSNS + 8];
    uint8_t exit_chk[2 * MAX_INSNS + 8];    /* a link exit's checks (CHK_*) */
    int entry_l[2];                         /* the entries' labels (a jump, in sequence) */
    int pk, psize;                          /* the pending flags (PK_NONE: in memory) */
    jit_fb_t *fb;
    int nfb;
    uint32_t *res;                          /* the instructions' resume words (JIT_RES) */
    int common;
} T;

#define R (&T.r)

#ifndef ESP_PLATFORM
/* (AMI_JITSTAT) the arena's bytes by what they are for: counted in an
 * attempt (s_bt), kept when the block is made (jit_bytes); the outermost
 * part counts what it holds */
unsigned long jit_bytes[JB_N];
static unsigned long s_bt[JB_N];
static int s_btd;
#define BT(cat, stmt) do { uint32_t *bt0_ = R->p; s_btd++; stmt; if (--s_btd == 0) { \
        s_bt[cat] += (unsigned long)(R->p - bt0_) * 4; } } while (0)
#else
#define BT(cat, stmt) do { stmt; } while (0)
#endif

/* values across calls: an address, a source, a destination, a result */
#define RA_ S3
#define RS_ S4
#define RD_ S10
#define RR_ S11

/* ---- Addresses ---- */

static inline uint32_t addr32(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static inline bool imm12(int32_t v)
{
    return v >= -2048 && v <= 2047;
}

/* a word at an absolute address: from s0 when within reach, else through t6 */
static void abs_access(int f3, bool store, int reg, uint32_t addr)
{
    int32_t off = (int32_t)(addr - T.cpu);
    int base = S0;
    if (!imm12(off)) {
        int32_t lo = (int32_t)(addr << 20) >> 20;
        rv_lui(R, T6, (addr - (uint32_t)lo) >> 12);
        base = T6;
        off = lo;
    }
    if (store) {
        rv_word(R, rv_s(off, reg, base, (uint32_t)f3, 0x23));
    } else {
        rv_word(R, rv_i(off, base, (uint32_t)f3, reg, 0x03));
    }
}

static void ld_rem(void) { abs_access(2, false, S1, addr32(&m68ki_remaining_cycles)); }
static void st_rem(void) { abs_access(2, true, S1, addr32(&m68ki_remaining_cycles)); }

/* ---- The shared routines ---- */

enum { ST_PROLOGUE, ST_EXIT, ST_EXIT_RF, ST_BAIL, ST_RD8, ST_RD16, ST_RD32, ST_WR8, ST_WR16, ST_WR32, ST_FB, ST_RESUME,
       ST_N };
static uint32_t s_stub[ST_N];
enum { PK_NONE, PK_LOGIC, PK_SUB, PK_ADD, PK_Z };   /* pending flags (below) */
static uint32_t s_mat[5][5];                /* their routines, by kind and size */
static uint32_t s_dummy_tags[64];           /* (the tags' stores while the cache doesn't fill) */

/* jal rd to an absolute address (within 1 MB) */
static void jal_abs(int rd, uint32_t to)
{
    int32_t off = (int32_t)(to - addr32(R->p));
    if (off < -(1 << 20) || off >= (1 << 20)) {
        R->full = true;
        return;
    }
    rv_word(R, rv_j(off, rd));
}

/* ---- Exits ---- */

enum { CHK_S1 = 1, CHK_STOP = 2 };         /* a link exit's checks before its jump */

static int new_exit(int kind, uint32_t pc, uint32_t fe)
{
    if (T.nexits >= T.max_exits) {
        R->full = true;
        return T.exit_label[0];
    }
    int k = T.nexits++;
    jit_exit_t *e = &T.exit[k];
    e->kind = (uint8_t)kind;
    e->pc = pc;
    e->ppc = T.u->pc;
    e->ir = T.u->op;
    e->fe = fe;
    T.exit_pk[k] = (uint8_t)T.pk;
    T.exit_ps[k] = (uint8_t)T.psize;
    T.exit_chk[k] = 0;
    T.exit_label[k] = rv_new_label(R);
    return T.exit_label[k];
}

/* The flow on to pc out of the block: an exit the dispatcher links to pc's
 * block (jit_run()), once there is one. Its jump checks the cycles left
 * (CHK_S1: unless the instruction's own check just did) and jit_stop
 * (CHK_STOP: after a write; only a write sets it, and every other write's
 * instruction looks at it itself) */
/* A computed exit (the PC in memory: RTS, JSR/JMP through an address
 * register): its two last targets' blocks kept after its record, jumped
 * to when the PC is one of them (jit.c fills them) */
static int new_dyn(uint32_t fe)
{
    int l = new_exit(JIT_EXIT_DYN, 0, fe);
    if (T.nexits && T.exit_label[T.nexits - 1] == l) {
        T.exit_chk[T.nexits - 1] = (uint8_t)(CHK_S1 | (T.wrote ? CHK_STOP : 0));
    }
    return l;
}

static int new_link(uint32_t pc, uint32_t fe, int chk)
{
    int l = new_exit(JIT_EXIT_LINK, pc, fe);
    if (T.nexits && T.exit_label[T.nexits - 1] == l && chk) {
        T.exit_chk[T.nexits - 1] = (uint8_t)(chk | (T.wrote ? CHK_STOP : 0));
    }
    return l;
}

/* ---- The fetch timing ---- */

static void charge_longword_(uint32_t a);

/* ic_longword() for the longword holding a */
static void charge_longword(uint32_t a)
{
    BT(JB_FETCH, charge_longword_(a));
}

static void charge_longword_(uint32_t a)
{
    uint32_t wait = T.u->wait;
    int32_t tag = (int32_t)((a & ~3u) - (T.in[0].pc & ~3u));   /* (from s2: the block's first's) */
    int32_t line = T.ic_tag_off + (int32_t)(a & 0xFC);
    int done = rv_new_label(R);
    if (!wait) {
        /* (no wait: only the line filled while the cache fills: s9 the
         * tags then, else a dummy's) */
        rv_addi(R, T1, S2, tag);
        rv_sw(R, T1, (int32_t)(a & 0xFC), S9);
    } else {
        int miss = rv_new_label(R);
        rv_lw(R, T0, T.ic_cacr_off, S0);
        rv_andi(R, T0, T0, 1);
        rv_beqz(R, T0, miss);
        rv_lw(R, T1, line, S0);
        rv_addi(R, T2, S2, tag);
        rv_bcc(R, RV_BEQ, T1, T2, done);
        rv_sw(R, T2, (int32_t)(a & 0xFC), S9);
        rv_place(R, miss);
        rv_addi(R, S1, S1, -(int32_t)wait);
    }
    rv_place(R, done);
}

/* The opcode at a: reached after code whose last word ends at fe (static),
 * or (dyn) as the fetch state in memory says */
static void opcode_charge_(uint32_t a, bool dyn, uint32_t fe);

static void opcode_charge(uint32_t a, bool dyn, uint32_t fe)
{
    BT(JB_FETCH, opcode_charge_(a, dyn, fe));
}

static void opcode_charge_(uint32_t a, bool dyn, uint32_t fe)
{
    if (!(a & 2)) {
        charge_longword(a);
    } else if (dyn) {
        int skip = rv_new_label(R);
        rv_lw(R, T0, T.ic_next_off, S0);
        rv_li(R, T1, a);
        rv_bcc(R, RV_BEQ, T0, T1, skip);
        charge_longword(a);
        rv_place(R, skip);
    } else if (a != ((fe & 0xFFFF) ? fe : 1)) {
        charge_longword(a);
    }
}

#ifdef M68K_HOST_HOOK
bool jit_hooks = true;                      /* (the host's AMI_JITNOHOOK: the board's code, no PC log) */
#endif

static void hook(uint32_t a)
{
#ifdef M68K_HOST_HOOK
    if (!jit_hooks) {
        return;
    }
    BT(JB_HOOK, st_rem(); rv_li(R, A0, a);
       rv_sw(R, A0, OFF(pc), S0);           /* (the host's debugging reads it) */
       rv_call(R, addr32((const void *)m68k_host_instruction)));
#else
    (void)a;
#endif
}

/* An extension word: its value, its longword's fetch charged when it's a
 * first word (in sequence) */
static uint32_t ext16(void)
{
    uint32_t a = T.fetch;
    uint16_t w = T.u->host[(a - T.u->pc) >> 1];
    if (!(a & 2)) {
        charge_longword(a);
    }
    T.fetch += 2;
    return w;
}

static uint32_t ext32(void)
{
    uint32_t hi = ext16();
    return hi << 16 | ext16();
}

/* ---- Memory ---- */

/* s5 = the instruction's address, for the routine's slow path */
static void set_ppc(void)
{
    uint32_t a = T.u->pc;
    if (T.s5 == (int64_t)a) {
        return;
    }
    int32_t d = (int32_t)(a - (uint32_t)T.s5);
    if (T.s5 >= 0 && imm12(d)) {
        BT(JB_PPC, rv_addi(R, S5, S5, d));
    } else {
        BT(JB_PPC, rv_li(R, S5, a));
    }
    T.s5 = a;
}

/* a0 = the data read at a0 / written there from a1 (any 32-bit address) */
static void mem_routine(int stub)
{
    set_ppc();
    BT(JB_MEMCALL, jal_abs(RA, s_stub[stub]));
    if (stub >= ST_WR8) {
        T.wrote = true;
    }
}

/* ---- Operands ---- */

enum { EA_D, EA_A, EA_IMM, EA_MEM };
typedef struct {
    int kind, reg;
    uint32_t value;                         /* EA_IMM's; EA_MEM's address when stat */
    bool stat;
    int areg;                               /* EA_MEM: the register with the address */
} ea_t;

static void mask_reg(int rd, int rs, int size)
{
    if (size == 1) {
        rv_andi(R, rd, rs, 0xFF);
    } else if (size == 2) {
        rv_slli(R, rd, rs, 16);
        rv_srli(R, rd, rd, 16);
    } else if (rd != rs) {
        rv_mv(R, rd, rs);
    }
}

static void sext_reg(int rd, int rs, int size)
{
    if (size == 1) {
        rv_slli(R, rd, rs, 24);
        rv_srai(R, rd, rd, 24);
    } else if (size == 2) {
        rv_slli(R, rd, rs, 16);
        rv_srai(R, rd, rd, 16);
    } else if (rd != rs) {
        rv_mv(R, rd, rs);
    }
}

/* rd = (brief index extension's Xn, sized and scaled); false: the full format */
static bool index_reg(int rd, uint32_t ext)
{
    if (ext & 0x100) {
        return false;
    }
    rv_lw(R, rd, DREG(ext >> 12), S0);
    if (!(ext & 0x800)) {
        sext_reg(rd, rd, 2);
    }
    if ((ext >> 9) & 3) {
        rv_slli(R, rd, rd, (int)((ext >> 9) & 3));
    }
    return true;
}

static void add_const(int rd, int rs, int32_t v)
{
    if (imm12(v)) {
        if (v || rd != rs) {
            rv_addi(R, rd, rs, v);
        }
    } else {
        rv_li(R, T5, (uint32_t)v);
        rv_add(R, rd, rs, T5);
    }
}

/* An operand's effective address worked out as the handler does it (its
 * extension words fetched; (An)+ and -(An) done); its address in areg.
 * False: not translated (an invalid mode, the full index format) */
static bool ea_decode(int mode, int reg, int size, ea_t *e, int areg)
{
    memset(e, 0, sizeof(*e));
    e->areg = areg;
    e->kind = EA_MEM;
    switch (mode) {
    case 0:
        e->kind = EA_D;
        e->reg = reg;
        return true;
    case 1:
        e->kind = EA_A;
        e->reg = reg;
        return size != 1;
    case 2:
        rv_lw(R, areg, AREG(reg), S0);
        return true;
    case 3: {
        int inc = size == 1 && reg == 7 ? 2 : size;
        rv_lw(R, areg, AREG(reg), S0);
        rv_addi(R, T0, areg, inc);
        rv_sw(R, T0, AREG(reg), S0);
        return true;
    }
    case 4: {
        int dec = size == 1 && reg == 7 ? 2 : size;
        rv_lw(R, areg, AREG(reg), S0);
        rv_addi(R, areg, areg, -dec);
        rv_sw(R, areg, AREG(reg), S0);
        return true;
    }
    case 5: {
        int32_t d = (int16_t)ext16();
        rv_lw(R, areg, AREG(reg), S0);
        add_const(areg, areg, d);
        return true;
    }
    case 6: {
        uint32_t ext = ext16();
        if (!index_reg(T0, ext)) {
            return false;
        }
        rv_lw(R, areg, AREG(reg), S0);
        rv_add(R, areg, areg, T0);
        add_const(areg, areg, (int8_t)ext);
        return true;
    }
    case 7:
        switch (reg) {
        case 0:
            e->stat = true;
            e->value = (uint32_t)(int32_t)(int16_t)ext16();
            return true;
        case 1:
            e->stat = true;
            e->value = ext32();
            return true;
        case 2: {
            uint32_t base = T.fetch;
            e->stat = true;
            e->value = base + (uint32_t)(int32_t)(int16_t)ext16();
            return true;
        }
        case 3: {
            uint32_t base = T.fetch, ext = ext16();
            if (!index_reg(T0, ext)) {
                return false;
            }
            rv_li(R, areg, base + (uint32_t)(int32_t)(int8_t)ext);
            rv_add(R, areg, areg, T0);
            return true;
        }
        case 4:
            e->kind = EA_IMM;
            e->value = size == 1 ? ext16() & 0xFF : size == 2 ? ext16() : ext32();
            return true;
        }
    }
    return false;
}

/* a0 = the address (the routines take its 24 bits) */
static void addr_arg(const ea_t *e, int32_t plus)
{
    if (e->stat) {
        rv_li(R, A0, e->value + (uint32_t)plus);
    } else {
        add_const(A0, e->areg, plus);
        if (!plus && A0 != e->areg) {
            /* (add_const's mv) */
        }
    }
}

static const int s_read[5] = { 0, ST_RD8, ST_RD16, 0, ST_RD32 };
static const int s_write[5] = { 0, ST_WR8, ST_WR16, 0, ST_WR32 };

/* rd = the operand: registers masked to its size (as the handlers'
 * MASK_OUT_ABOVE), memory as the callback gives it */
static void ea_read(const ea_t *e, int size, int rd)
{
    switch (e->kind) {
    case EA_D:
        rv_lw(R, rd, DREG(e->reg), S0);
        mask_reg(rd, rd, size);
        break;
    case EA_A:
        rv_lw(R, rd, AREG(e->reg), S0);
        mask_reg(rd, rd, size);
        break;
    case EA_IMM:
        rv_li(R, rd, e->value);
        break;
    default:
        addr_arg(e, 0);
        mem_routine(s_read[size]);
        if (rd != A0) {
            rv_mv(R, rd, A0);
        }
    }
}

/* the operand = rs (masked to its size for memory) */
static void ea_write(const ea_t *e, int size, int rs)
{
    switch (e->kind) {
    case EA_D:
        if (size == 1) {
            rv_sb(R, rs, DREG(e->reg), S0);
        } else if (size == 2) {
            rv_sh(R, rs, DREG(e->reg), S0);
        } else {
            rv_sw(R, rs, DREG(e->reg), S0);
        }
        break;
    case EA_A:
        rv_sw(R, rs, AREG(e->reg), S0);
        break;
    default:
        if (rs != A1) {
            rv_mv(R, A1, rs);
        }
        addr_arg(e, 0);
        mem_routine(s_write[size]);
    }
}

/* ---- Flags ----
 *
 * In memory Musashi's form: N bit 7, Z the value (0: set), V bit 7, C and
 * X bit 8; its tests look at those bits alone (the others may differ
 * here). A flag-setting instruction leaves N Z V C pending instead: its
 * source, destination and result, masked to its size, in F_S F_D F_R and
 * its kind (T.pk). A condition comes from them directly (after CMP, BLT
 * is one signed compare); they reach memory (materialize(), the shared
 * routines') before anything that reads them there: a handler, an exit, a
 * jump within the block, an instruction that sets only some of them. X
 * is never pending.
 */
#define F_S S6
#define F_D S7
#define F_R S8

static void st_flag(int reg, int32_t off) { rv_sw(R, reg, off, S0); }

static int nshift(int size) { return size == 1 ? 0 : size == 2 ? 8 : 24; }

/* the pending flags to memory */
static void materialize(void)
{
    if (T.pk != PK_NONE) {
        BT(JB_MAT, jal_abs(T0, s_mat[T.pk][T.psize]));
        T.pk = PK_NONE;
    }
}

/* N and Z from a result masked to the size; V and C clear */
static void flags_logic(int res, int size)
{
    if (res != F_R) {
        rv_mv(R, F_R, res);
    }
    T.pk = PK_LOGIC;
    T.psize = size;
}

static void pend(int kind, int s, int d, int r, int size)
{
    if (s != F_S) {
        rv_mv(R, F_S, s);
    }
    if (d != F_D) {
        rv_mv(R, F_D, d);
    }
    mask_reg(F_R, r, size);
    T.pk = kind;
    T.psize = size;
}

/* ADD's: s + d = r (s, d masked to the size, r not); x: X too (the carry) */
static void flags_add(int s, int d, int r, int size, bool x)
{
    pend(PK_ADD, s, d, r, size);
    if (x) {
        rv_sltu(R, T4, F_R, F_S);
        rv_slli(R, T4, T4, 8);
        st_flag(T4, OFF(x_flag));
    }
}

/* SUB's and CMP's: d - s = r */
static void flags_sub(int s, int d, int r, int size, bool x)
{
    pend(PK_SUB, s, d, r, size);
    if (x) {
        rv_sltu(R, T4, F_D, F_S);
        rv_slli(R, T4, T4, 8);
        st_flag(T4, OFF(x_flag));
    }
}

/* Eager, in memory: ADDX and SUBX (r includes X; Z as Musashi's, |=) */
/* ADD's: s + d = r (r unmasked for bytes and words); x also sets X; xin:
 * r includes X (ADDX: the carry by Musashi's formula). Flags as Musashi's
 * where its tests look (N and V bit 7, C and X bit 8, Z nonzero): the
 * other bits may differ. */
static void eager_add(int s, int d, int r, int size, bool x, bool xin)
{
    int sh = nshift(size);
    if (sh) {
        rv_srli(R, T4, r, sh);
        st_flag(T4, OFF(n_flag));
    } else {
        st_flag(r, OFF(n_flag));
    }
    rv_xor(R, T4, s, r);
    rv_xor(R, T5, d, r);
    rv_and(R, T4, T4, T5);
    if (sh) {
        rv_srli(R, T4, T4, sh);
    }
    st_flag(T4, OFF(v_flag));
    if (size == 4 && !xin) {
        rv_sltu(R, T4, r, s);               /* (the carry: the sum below an operand) */
        rv_slli(R, T4, T4, 8);
    } else if (size == 4) {
        /* ((S & D) | (~R & (S | D))) >> 23 */
        rv_and(R, T4, s, d);
        rv_or(R, T5, s, d);
        rv_xori(R, T3, r, -1);
        rv_and(R, T5, T5, T3);
        rv_or(R, T4, T4, T5);
        rv_srli(R, T4, T4, 23);
    } else if (size == 2) {
        rv_srli(R, T4, r, 8);
    } else {
        rv_mv(R, T4, r);
    }
    st_flag(T4, OFF(c_flag));
    if (x) {
        st_flag(T4, OFF(x_flag));
    }
}

/* SUB's and CMP's: d - s = r (xin: SUBX's, r less X) */
static void eager_sub(int s, int d, int r, int size, bool x, bool xin)
{
    int sh = nshift(size);
    if (sh) {
        rv_srli(R, T4, r, sh);
        st_flag(T4, OFF(n_flag));
    } else {
        st_flag(r, OFF(n_flag));
    }
    rv_xor(R, T4, s, d);
    rv_xor(R, T5, r, d);
    rv_and(R, T4, T4, T5);
    if (sh) {
        rv_srli(R, T4, T4, sh);
    }
    st_flag(T4, OFF(v_flag));
    if (size == 4 && !xin) {
        rv_sltu(R, T4, d, s);               /* (the borrow) */
        rv_slli(R, T4, T4, 8);
    } else if (size == 4) {
        /* ((S & R) | (~D & (S | R))) >> 23 */
        rv_and(R, T4, s, r);
        rv_or(R, T5, s, r);
        rv_xori(R, T3, d, -1);
        rv_and(R, T5, T5, T3);
        rv_or(R, T4, T4, T5);
        rv_srli(R, T4, T4, 23);
    } else if (size == 2) {
        rv_srli(R, T4, r, 8);
    } else {
        rv_mv(R, T4, r);
    }
    st_flag(T4, OFF(c_flag));
    if (x) {
        st_flag(T4, OFF(x_flag));
    }
}

/* ---- Conditions ---- */

/* rd nonzero when condition cc (2-15) holds, from the flags in memory */
static void cond_mem(int cc, int rd)
{
    switch (cc) {
    case 0x2: /* HI: !C && Z */
    case 0x3: /* LS: C || !Z */
        rv_lw(R, T4, OFF(c_flag), S0);
        rv_srli(R, T4, T4, 8);
        rv_andi(R, T4, T4, 1);
        rv_lw(R, T5, OFF(not_z_flag), S0);
        rv_sltu(R, T5, X0, T5);
        if (cc == 2) {
            rv_xori(R, T4, T4, 1);
            rv_and(R, rd, T4, T5);
        } else {
            rv_xori(R, T5, T5, 1);
            rv_or(R, rd, T4, T5);
        }
        break;
    case 0x4: case 0x5:
        rv_lw(R, rd, OFF(c_flag), S0);
        rv_andi(R, rd, rd, 0x100);
        if (cc == 4) {
            rv_sltiu(R, rd, rd, 1);
        }
        break;
    case 0x6:
        rv_lw(R, rd, OFF(not_z_flag), S0);
        break;
    case 0x7:
        rv_lw(R, rd, OFF(not_z_flag), S0);
        rv_sltiu(R, rd, rd, 1);
        break;
    case 0x8: case 0x9:
        rv_lw(R, rd, OFF(v_flag), S0);
        rv_andi(R, rd, rd, 0x80);
        if (cc == 8) {
            rv_sltiu(R, rd, rd, 1);
        }
        break;
    case 0xA: case 0xB:
        rv_lw(R, rd, OFF(n_flag), S0);
        rv_andi(R, rd, rd, 0x80);
        if (cc == 0xA) {
            rv_sltiu(R, rd, rd, 1);
        }
        break;
    case 0xC: case 0xD: /* GE: !((N ^ V) & 0x80), LT */
        rv_lw(R, T4, OFF(n_flag), S0);
        rv_lw(R, T5, OFF(v_flag), S0);
        rv_xor(R, rd, T4, T5);
        rv_andi(R, rd, rd, 0x80);
        if (cc == 0xC) {
            rv_sltiu(R, rd, rd, 1);
        }
        break;
    case 0xE: case 0xF: /* GT: GE && Z; LE: LT || !Z */
        rv_lw(R, T4, OFF(n_flag), S0);
        rv_lw(R, T5, OFF(v_flag), S0);
        rv_xor(R, T4, T4, T5);
        rv_srli(R, T4, T4, 7);
        rv_andi(R, T4, T4, 1);              /* LT */
        rv_lw(R, T5, OFF(not_z_flag), S0);
        rv_sltu(R, T5, X0, T5);             /* NE */
        if (cc == 0xE) {
            rv_xori(R, T4, T4, 1);
            rv_and(R, rd, T4, T5);
        } else {
            rv_xori(R, T5, T5, 1);
            rv_or(R, rd, T4, T5);
        }
        break;
    }
}


/* To l when condition cc is `when` (pending flags or memory's) */
static void cond_branch(int cc, bool when, int l)
{
    if (cc < 2) {
        if ((cc == 0) == when) {
            rv_jl(R, l);                    /* (T true, F false) */
        }
        return;
    }
    if (T.pk == PK_ADD || (T.pk == PK_Z && cc != 6 && cc != 7)) {
        materialize();
    }
    if (T.pk == PK_NONE) {
        cond_mem(cc, T0);
        if (when) {
            rv_bnez(R, T0, l);
        } else {
            rv_beqz(R, T0, l);
        }
        return;
    }
    if (!when) {
        cc ^= 1;                            /* (each condition's opposite: its neighbour) */
    }
    int sh = 32 - 8 * T.psize;
    int n = F_R, d = F_D, s_ = F_S;
    if (sh && (cc == 0xA || cc == 0xB || ((cc == 0xC || cc == 0xD || cc == 0xE || cc == 0xF) && T.pk == PK_LOGIC))) {
        rv_slli(R, T0, F_R, sh);            /* (the sign at bit 31) */
        n = T0;
    }
    if (sh && T.pk == PK_SUB && cc >= 0xC) {
        rv_slli(R, T0, F_D, sh);
        rv_slli(R, T1, F_S, sh);
        d = T0, s_ = T1;
    }
    if (T.pk == PK_Z) {
        if (cc == 6) {
            rv_bnez(R, F_R, l);
        } else {
            rv_beqz(R, F_R, l);
        }
        return;
    }
    if (T.pk == PK_LOGIC) {
        switch (cc) {
        case 0x2: case 0x6: rv_bnez(R, F_R, l); break;          /* HI, NE */
        case 0x3: case 0x7: rv_beqz(R, F_R, l); break;          /* LS, EQ */
        case 0x4: case 0x8: rv_jl(R, l); break;                 /* CC, VC */
        case 0x5: case 0x9: break;                              /* CS, VS */
        case 0xA: case 0xC: rv_bcc(R, RV_BGE, n, X0, l); break; /* PL, GE */
        case 0xB: case 0xD: rv_bcc(R, RV_BLT, n, X0, l); break; /* MI, LT */
        case 0xE: rv_bcc(R, RV_BLT, X0, n, l); break;           /* GT */
        case 0xF: rv_bcc(R, RV_BGE, X0, n, l); break;           /* LE */
        }
        return;
    }
    /* PK_SUB: d - s */
    switch (cc) {
    case 0x2: rv_bcc(R, RV_BLTU, F_S, F_D, l); break;           /* HI: d > s */
    case 0x3: rv_bcc(R, RV_BGEU, F_S, F_D, l); break;           /* LS */
    case 0x4: rv_bcc(R, RV_BGEU, F_D, F_S, l); break;           /* CC */
    case 0x5: rv_bcc(R, RV_BLTU, F_D, F_S, l); break;           /* CS */
    case 0x6: rv_bnez(R, F_R, l); break;
    case 0x7: rv_beqz(R, F_R, l); break;
    case 0x8: case 0x9:                                         /* VC, VS */
        rv_xor(R, T0, F_S, F_D);
        rv_xor(R, T1, F_R, F_D);
        rv_and(R, T0, T0, T1);
        if (sh) {
            rv_slli(R, T0, T0, sh);
        }
        rv_bcc(R, cc == 8 ? RV_BGE : RV_BLT, T0, X0, l);
        break;
    case 0xA: rv_bcc(R, RV_BGE, n, X0, l); break;
    case 0xB: rv_bcc(R, RV_BLT, n, X0, l); break;
    case 0xC: rv_bcc(R, RV_BGE, d, s_, l); break;               /* GE */
    case 0xD: rv_bcc(R, RV_BLT, d, s_, l); break;               /* LT */
    case 0xE: rv_bcc(R, RV_BLT, s_, d, l); break;               /* GT */
    case 0xF: rv_bcc(R, RV_BGE, s_, d, l); break;               /* LE */
    }
}

/* rd = 1 when condition cc holds, else 0 (rd: not t0-t2, t4, t5: the
 * flag routines' and cond_branch()'s) */
static void cond_value(int cc, int rd)
{
    if (T.pk == PK_ADD || (T.pk == PK_Z && cc != 6 && cc != 7)) {
        materialize();                      /* (before rd is set: the routine takes t1, t2) */
    }
    int l = rv_new_label(R);
    rv_addi(R, rd, X0, 1);
    cond_branch(cc, true, l);
    rv_addi(R, rd, X0, 0);
    rv_place(R, l);
}

/* ---- Flow ---- */

static int insn_at(uint32_t a)
{
    for (int i = 0; i < T.n; i++) {
        if (T.in[i].pc == a) {
            return i;
        }
    }
    return -1;
}

/* A branch taken to a: inside the block, on to its body (the cycles
 * already counted; the slice may end first); else out */
static void branch_to(uint32_t a, uint32_t fe)
{
    int i = insn_at(a);
    if (i < 0 || a == T.u->pc) {
        rv_jl(R, new_link(a, fe, CHK_S1));
        return;
    }
    BT(JB_CHECK, rv_blez(R, S1, new_exit(JIT_EXIT_PC, a, fe)));
    int pk = T.pk, ps = T.psize;
    materialize();                          /* (the target takes the flags from memory) */
    hook(a);
    opcode_charge(a, false, fe);
    rv_jl(R, T.in[i].body);
    T.pk = pk, T.psize = ps;                /* (as they are on the path not taken) */
}

static void cycles(int32_t c)
{
    if (c) {
        BT(JB_CYCLES, rv_addi(R, S1, S1, -c));
    }
}

/* ---- The instructions ---- */

static int size_bits(int s)          /* 00 byte, 01 word, 10 long */
{
    return s == 0 ? 1 : s == 1 ? 2 : s == 2 ? 4 : 0;
}

/* MOVE, MOVEA */
static bool t_move(uint16_t op)
{
    int s = (op >> 12) & 3, size = s == 1 ? 1 : s == 3 ? 2 : 4;
    int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    ea_t src, dst;
    if (dmode == 7 && dreg > 1) {
        return false;
    }
    if (!ea_decode((op >> 3) & 7, op & 7, size, &src, RA_) || (src.kind == EA_A && size == 1)) {
        return false;
    }
    int v = dmode == 1 ? RS_ : F_R;         /* (a flag setter's values straight in its flags' registers) */
    ea_read(&src, size, v);
    if (dmode == 1) {                       /* MOVEA: sign-extended, no flags */
        if (size == 1) {
            return false;
        }
        sext_reg(T0, RS_, size);
        rv_sw(R, T0, AREG(dreg), S0);
        return true;
    }
    if (!ea_decode(dmode, dreg, size, &dst, RA_)) {
        return false;
    }
    if (size == 4 && dmode == 4) {
        /* (MOVE.L to -(An): two words, the low one first) */
        rv_slli(R, A1, F_R, 16);
        rv_srli(R, A1, A1, 16);
        addr_arg(&dst, 2);
        mem_routine(ST_WR16);
        rv_srli(R, A1, F_R, 16);
        addr_arg(&dst, 0);
        mem_routine(ST_WR16);
    } else {
        ea_write(&dst, size, F_R);
    }
    flags_logic(F_R, size);
    return true;
}

static bool t_moveq(uint16_t op)
{
    if (op & 0x100) {
        return false;
    }
    uint32_t v = (uint32_t)(int32_t)(int8_t)op;
    rv_li(R, F_R, v);
    rv_sw(R, F_R, DREG((op >> 9) & 7), S0);
    flags_logic(F_R, 4);
    return true;
}

/* LEA, PEA */
static bool t_lea(uint16_t op, bool pea)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    ea_t e;
    if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) {
        return false;
    }
    if (!ea_decode(mode, reg, 4, &e, RA_)) {
        return false;
    }
    if (e.stat) {
        rv_li(R, RA_, e.value);
    }
    if (!pea) {
        rv_sw(R, RA_, AREG((op >> 9) & 7), S0);
        return true;
    }
    /* m68ki_push_32() */
    rv_lw(R, T0, AREG(7), S0);
    rv_addi(R, T0, T0, -4);
    rv_sw(R, T0, AREG(7), S0);
    rv_mv(R, A1, RA_);
    rv_mv(R, A0, T0);
    mem_routine(ST_WR32);
    return true;
}

/* TST, CLR */
static bool t_tst_clr(uint16_t op, bool clr)
{
    int size = size_bits((op >> 6) & 3), mode = (op >> 3) & 7, reg = op & 7;
    ea_t e;
    if (!size || (clr && (mode == 1 || (mode == 7 && reg > 1)))) {
        return false;
    }
    if (!ea_decode(mode, reg, size, &e, RA_)) {
        return false;
    }
    if (clr) {
        ea_write(&e, size, X0);
        flags_logic(X0, size);
        return true;
    }
    ea_read(&e, size, F_R);
    flags_logic(F_R, size);
    return true;
}

/* EXT.W, EXT.L, EXTB.L, SWAP */
static bool t_ext_swap(uint16_t op)
{
    int reg = op & 7;
    rv_lw(R, T0, DREG(reg), S0);
    switch (op & 0xFFF8) {
    case 0x4880:                            /* EXT.W: N and Z of the low word */
        sext_reg(T1, T0, 1);
        rv_sh(R, T1, DREG(reg), S0);
        mask_reg(F_R, T1, 2);
        flags_logic(F_R, 2);
        break;
    case 0x48C0:                            /* EXT.L */
    case 0x49C0:                            /* EXTB.L */
        sext_reg(F_R, T0, (op & 0xFFF8) == 0x48C0 ? 2 : 1);
        rv_sw(R, F_R, DREG(reg), S0);
        flags_logic(F_R, 4);
        break;
    case 0x4840:                            /* SWAP */
        rv_slli(R, T1, T0, 16);
        rv_srli(R, T2, T0, 16);
        rv_or(R, F_R, T1, T2);
        rv_sw(R, F_R, DREG(reg), S0);
        flags_logic(F_R, 4);
        break;
    default:
        return false;
    }
    return true;
}

/* ADDQ, SUBQ */
static bool t_addq(uint16_t op)
{
    int size = size_bits((op >> 6) & 3), mode = (op >> 3) & 7, reg = op & 7;
    uint32_t data = (((op >> 9) - 1) & 7) + 1;
    bool sub = op & 0x100;
    ea_t e;
    if (!size || (mode == 7 && reg > 1) || (mode == 1 && size == 1)) {
        return false;
    }
    if (mode == 1) {                        /* to An: the whole register, no flags */
        rv_lw(R, T0, AREG(reg), S0);
        rv_addi(R, T0, T0, sub ? -(int32_t)data : (int32_t)data);
        rv_sw(R, T0, AREG(reg), S0);
        return true;
    }
    if (!ea_decode(mode, reg, size, &e, RA_)) {
        return false;
    }
    ea_read(&e, size, F_D);
    rv_li(R, F_S, data);
    if (sub) {
        rv_sub(R, F_R, F_D, F_S);
        flags_sub(F_S, F_D, F_R, size, true);
    } else {
        rv_add(R, F_R, F_D, F_S);
        flags_add(F_S, F_D, F_R, size, true);
    }
    ea_write(&e, size, F_R);
    return true;
}

/* ADD, SUB, AND, OR, EOR, CMP between an operand and Dn; ADDA, SUBA, CMPA */
enum { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_EOR, OP_CMP };

static void alu(int kind, int size, int s, int d, int res)
{
    switch (kind) {
    case OP_ADD: rv_add(R, res, d, s); flags_add(s, d, res, size, true); break;
    case OP_SUB: rv_sub(R, res, d, s); flags_sub(s, d, res, size, true); break;
    case OP_CMP: rv_sub(R, res, d, s); flags_sub(s, d, res, size, false); break;
    case OP_AND: rv_and(R, res, d, s); flags_logic(res, size); break;
    case OP_OR: rv_or(R, res, d, s); flags_logic(res, size); break;
    case OP_EOR: rv_xor(R, res, d, s); flags_logic(res, size); break;
    }
}

static bool t_alu(uint16_t op, int kind)
{
    int opmode = (op >> 6) & 7, dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
    ea_t e;
    if (opmode == 3 || opmode == 7) {       /* ADDA, SUBA, CMPA */
        if (kind != OP_ADD && kind != OP_SUB && kind != OP_CMP) {
            return false;
        }
        int size = opmode == 3 ? 2 : 4;
        if (!ea_decode(mode, reg, size, &e, RA_)) {
            return false;
        }
        ea_read(&e, size, RS_);
        if (size == 2) {
            sext_reg(RS_, RS_, 2);
        }
        rv_lw(R, RD_, AREG(dn), S0);
        if (kind == OP_CMP) {
            rv_mv(R, F_S, RS_);
            rv_mv(R, F_D, RD_);
            rv_sub(R, F_R, F_D, F_S);
            flags_sub(F_S, F_D, F_R, 4, false);
        } else {
            if (kind == OP_ADD) {
                rv_add(R, RR_, RD_, RS_);
            } else {
                rv_sub(R, RR_, RD_, RS_);
            }
            rv_sw(R, RR_, AREG(dn), S0);
        }
        return true;
    }
    int size = size_bits(opmode & 3);
    if (!(opmode & 4) || kind == OP_CMP) {  /* <ea> op Dn -> Dn */
        if ((opmode & 4) || (kind == OP_EOR)) {
            return false;
        }
        if ((kind == OP_AND || kind == OP_OR) && mode == 1) {
            return false;
        }
        if (!ea_decode(mode, reg, size, &e, RA_) || (e.kind == EA_A && size == 1)) {
            return false;
        }
        ea_read(&e, size, F_S);
        rv_lw(R, F_D, DREG(dn), S0);
        mask_reg(F_D, F_D, size);
        alu(kind, size, F_S, F_D, F_R);
        if (kind != OP_CMP) {
            ea_t d = { .kind = EA_D, .reg = dn };
            ea_write(&d, size, F_R);
        }
        return true;
    }
    /* Dn op <ea> -> <ea> (EOR here; AND/OR/ADD/SUB to memory) */
    if (mode == 1 || (mode == 0 && kind != OP_EOR) || (mode == 7 && reg > 1)) {
        return false;                       /* (ADDX/SUBX, ABCD, EXG ... elsewhere) */
    }
    if (!ea_decode(mode, reg, size, &e, RA_)) {
        return false;
    }
    if (kind == OP_EOR && mode == 0) {
        rv_lw(R, F_S, DREG(dn), S0);
        mask_reg(F_S, F_S, size);
        rv_lw(R, F_D, DREG(reg), S0);
        mask_reg(F_D, F_D, size);
        alu(kind, size, F_S, F_D, F_R);
        ea_write(&e, size, F_R);
        return true;
    }
    ea_read(&e, size, F_D);                  /* dst */
    rv_lw(R, F_S, DREG(dn), S0);
    mask_reg(F_S, F_S, size);
    alu(kind, size, F_S, F_D, F_R);
    ea_write(&e, size, F_R);
    return true;
}

/* ORI, ANDI, SUBI, ADDI, EORI, CMPI */
static bool t_imm(uint16_t op)
{
    static const int kinds[8] = { OP_OR, OP_AND, OP_SUB, OP_ADD, -1, OP_EOR, OP_CMP, -1 };
    int kind = kinds[(op >> 9) & 7], size = size_bits((op >> 6) & 3), mode = (op >> 3) & 7, reg = op & 7;
    ea_t e;
    if (kind < 0 || !size || mode == 1 || (mode == 7 && (reg == 4 || (kind != OP_CMP && reg > 1)))) {
        return false;                       /* (to CCR/SR: privileged or special) */
    }
    uint32_t imm = size == 1 ? ext16() & 0xFF : size == 2 ? ext16() : ext32();
    if (!ea_decode(mode, reg, size, &e, RA_)) {
        return false;
    }
    ea_read(&e, size, F_D);
    rv_li(R, F_S, imm);
    alu(kind, size, F_S, F_D, F_R);
    if (kind != OP_CMP) {
        ea_write(&e, size, F_R);
    }
    return true;
}

/* BTST, BCHG, BCLR, BSET: #n or Dn */
static bool t_bit(uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, kind = (op >> 6) & 3;
    bool imm = !(op & 0x100);
    ea_t e;
    if (mode == 1 || (mode == 7 && reg > (kind == 0 ? 4 : 1)) || (imm && (op & 0x0E00) != 0x0800)) {
        return false;
    }
    if (imm && mode == 7 && reg == 4) {
        return false;
    }
    int size = mode == 0 ? 4 : 1, bits = mode == 0 ? 0x1F : 7;
    uint32_t n = 0;
    materialize();                          /* (it sets Z alone) */
    if (imm) {
        n = ext16() & 0xFF & (uint32_t)bits;
    }
    if (!ea_decode(mode, reg, size, &e, RA_)) {
        return false;
    }
    /* (Dn's whole register; memory a byte) */
    if (e.kind == EA_D) {
        rv_lw(R, RD_, DREG(e.reg), S0);
    } else {
        ea_read(&e, size, RD_);
    }
    if (imm) {
        rv_li(R, RS_, 1u << n);
    } else {
        rv_lw(R, T0, DREG((op >> 9) & 7), S0);
        rv_andi(R, T0, T0, bits);
        rv_addi(R, RS_, X0, 1);
        rv_sll(R, RS_, RS_, T0);
    }
    rv_and(R, F_R, RD_, RS_);
    T.pk = PK_Z;
    T.psize = 4;
    if (kind == 0) {
        return true;
    }
    if (kind == 1) {
        rv_xor(R, RR_, RD_, RS_);
    } else if (kind == 2) {
        rv_xori(R, T0, RS_, -1);
        rv_and(R, RR_, RD_, T0);
    } else {
        rv_or(R, RR_, RD_, RS_);
    }
    if (e.kind == EA_D) {
        rv_sw(R, RR_, DREG(e.reg), S0);
    } else {
        ea_write(&e, 1, RR_);
    }
    return true;
}

/* Shifts and rotates of Dn by an immediate count */
static bool t_shift(uint16_t op)
{
    int size = size_bits((op >> 6) & 3), reg = op & 7, type = (op >> 3) & 3;
    bool left = op & 0x100;
    if (!size || (op & 0x20) || type == 2 || (type == 3 && size != 4)) {
        return false;                       /* (by a register; ROXL/ROXR; ROL/ROR .B .W) */
    }
    int shift = (int)((((op >> 9) - 1) & 7) + 1);
    rv_lw(R, T0, DREG(reg), S0);
    mask_reg(T0, T0, size);                 /* src */
    /* res in T1, C in T2 */
    switch (type << 1 | left) {
    case 0:                                 /* ASR */
        sext_reg(T1, T0, size);
        rv_srai(R, T1, T1, shift);
        mask_reg(T1, T1, size);
        rv_slli(R, T2, T0, 9 - shift);      /* C = src << (9 - shift) */
        break;
    case 1:                                 /* ASL */
        rv_slli(R, T1, T0, shift);
        mask_reg(T1, T1, size);
        if (size == 4) {
            rv_srli(R, T2, T0, 24 - shift);
        } else if (size == 2) {
            rv_srli(R, T2, T0, 8 - shift);
        } else {
            rv_slli(R, T2, T0, shift);
        }
        break;
    case 2:                                 /* LSR */
        rv_srli(R, T1, T0, shift);
        rv_slli(R, T2, T0, 9 - shift);
        break;
    case 3:                                 /* LSL */
        rv_slli(R, T1, T0, shift);
        mask_reg(T1, T1, size);
        if (size == 4) {
            rv_srli(R, T2, T0, 24 - shift);
        } else if (size == 2) {
            rv_srli(R, T2, T0, 8 - shift);
        } else {
            rv_slli(R, T2, T0, shift);
        }
        break;
    case 6:                                 /* ROR */
        if (size != 4) {
            return false;
        }
        rv_srli(R, T1, T0, shift);
        rv_slli(R, T3, T0, 32 - shift);
        rv_or(R, T1, T1, T3);
        rv_slli(R, T2, T0, 9 - shift);
        break;
    case 7:                                 /* ROL */
        if (size != 4) {
            return false;
        }
        rv_slli(R, T1, T0, shift);
        rv_srli(R, T3, T0, 32 - shift);
        rv_or(R, T1, T1, T3);
        rv_srli(R, T2, T0, 24 - shift);
        break;
    default:
        return false;
    }
    ea_t d = { .kind = EA_D, .reg = reg };
    ea_write(&d, size, T1);
    if (type == 3) {
        cycles(shift << m68ki_cpu.cyc_shift);  /* (ROL/ROR: whatever the CPU, unlike the others) */
    }
    /* N */
    if (type == 1 && !left) {
        st_flag(X0, OFF(n_flag));           /* LSR: N clear */
    } else if (nshift(size)) {
        rv_srli(R, T3, T1, nshift(size));
        st_flag(T3, OFF(n_flag));
    } else {
        st_flag(T1, OFF(n_flag));
    }
    st_flag(T1, OFF(not_z_flag));
    st_flag(T2, OFF(c_flag));
    if (type != 3) {
        st_flag(T2, OFF(x_flag));
    }
    if (type == 0 && left) {
        /* ASL's V: src's top shift + 1 bits neither all clear nor all set
         * (Musashi's tables; a byte shifted by 8: set unless all clear) */
        uint32_t top = size == 4 ? m68ki_shift_32_table[shift + 1]
                     : size == 2 ? m68ki_shift_16_table[shift + 1] : m68ki_shift_8_table[shift + 1];
        rv_li(R, T3, top);
        rv_and(R, T4, T0, T3);
        rv_xor(R, T5, T4, T3);
        rv_sltu(R, T4, X0, T4);             /* != 0 */
        if (!(size == 1 && shift >= 8)) {
            rv_sltu(R, T5, X0, T5);         /* != top */
            rv_and(R, T4, T4, T5);
        }
        rv_slli(R, T4, T4, 7);
        st_flag(T4, OFF(v_flag));
    } else {
        st_flag(X0, OFF(v_flag));
    }
    T.pk = PK_NONE;                         /* (N Z V C all in memory) */
    return true;
}

/* An instruction on data registers alone (no extension words, no memory,
 * no branch, no exception) by its handler, called directly: REG_IR (its
 * operands) and the cycles left (it may add some) set, the flags in
 * memory before and after */
static bool t_handler(uint16_t op)
{
    materialize();
    st_rem();
    rv_li(R, T0, op);
    rv_sw(R, T0, OFF(ir), S0);
    rv_call(R, addr32((const void *)(*m68k_loop.handlers)[op]));
    ld_rem();
    T.pk = PK_NONE;
    return true;
}

/* ADDX, SUBX between data registers */
static bool t_addx(uint16_t op, bool sub)
{
    int size = size_bits((op >> 6) & 3);
    if (!size || (op & 8)) {
        return false;
    }
    int rx = (op >> 9) & 7, ry = op & 7;
    materialize();                          /* (it reads Z and X) */
    rv_lw(R, RS_, DREG(ry), S0);
    mask_reg(RS_, RS_, size);                 /* src */
    rv_lw(R, RD_, DREG(rx), S0);
    mask_reg(RD_, RD_, size);                 /* dst */
    rv_lw(R, T0, OFF(x_flag), S0);
    rv_srli(R, T0, T0, 8);
    rv_andi(R, T0, T0, 1);
    if (sub) {
        rv_sub(R, RR_, RD_, RS_);
        rv_sub(R, RR_, RR_, T0);
        eager_sub(RS_, RD_, RR_, size, true, true);
    } else {
        rv_add(R, RR_, RD_, RS_);
        rv_add(R, RR_, RR_, T0);
        eager_add(RS_, RD_, RR_, size, true, true);
    }
    mask_reg(RR_, RR_, size);
    rv_lw(R, T0, OFF(not_z_flag), S0);
    rv_or(R, T0, T0, RR_);
    st_flag(T0, OFF(not_z_flag));
    ea_t d = { .kind = EA_D, .reg = rx };
    ea_write(&d, size, RR_);
    return true;
}

/* MULU.W, MULS.W */
static bool t_mul(uint16_t op)
{
    bool sign = op & 0x100;
    ea_t e;
    if (!ea_decode((op >> 3) & 7, op & 7, 2, &e, RA_) || e.kind == EA_A) {
        return false;
    }
    ea_read(&e, 2, RS_);
    int dn = (op >> 9) & 7;
    rv_lw(R, T0, DREG(dn), S0);
    if (sign) {
        sext_reg(T1, RS_, 2);
        sext_reg(T0, T0, 2);
    } else {
        mask_reg(T1, RS_, 2);
        mask_reg(T0, T0, 2);
    }
    rv_mul(R, F_R, T0, T1);
    rv_sw(R, F_R, DREG(dn), S0);
    flags_logic(F_R, 4);
    return true;
}

/* NEG, NOT on a data register */
static bool t_neg_not(uint16_t op, bool not)
{
    int size = size_bits((op >> 6) & 3), reg = op & 7;
    if (!size || ((op >> 3) & 7)) {
        return false;
    }
    if (not) {
        rv_lw(R, F_R, DREG(reg), S0);
        rv_xori(R, F_R, F_R, -1);
        mask_reg(F_R, F_R, size);
        flags_logic(F_R, size);
    } else {
        /* res = 0 - dst: a subtraction of dst from 0 (V = dst & res, C = X
         * = dst nonzero, as Musashi's) */
        rv_lw(R, F_S, DREG(reg), S0);
        mask_reg(F_S, F_S, size);
        rv_sub(R, F_R, X0, F_S);
        flags_sub(F_S, X0, F_R, size, true);
    }
    ea_t d = { .kind = EA_D, .reg = reg };
    ea_write(&d, size, F_R);
    return true;
}

/* EXG */
static bool t_exg(uint16_t op)
{
    int x = (op >> 9) & 7, y = op & 7, m = op & 0x1F8;
    int32_t ox, oy;
    if (m == 0x140) {
        ox = DREG(x), oy = DREG(y);
    } else if (m == 0x148) {
        ox = AREG(x), oy = AREG(y);
    } else if (m == 0x188) {
        ox = DREG(x), oy = AREG(y);
    } else {
        return false;
    }
    rv_lw(R, T0, ox, S0);
    rv_lw(R, T1, oy, S0);
    rv_sw(R, T1, ox, S0);
    rv_sw(R, T0, oy, S0);
    return true;
}

/* Scc */
static bool t_scc(uint16_t op)
{
    int cc = (op >> 8) & 15, mode = (op >> 3) & 7, reg = op & 7;
    ea_t e;
    if (mode == 1 || (mode == 7 && reg > 1)) {
        return false;
    }
    if (!ea_decode(mode, reg, 1, &e, RA_)) {
        return false;
    }
    if (cc < 2) {
        rv_addi(R, RS_, X0, cc == 0 ? -1 : 0);
    } else {
        cond_value(cc, T3);
        rv_sub(R, RS_, X0, T3);
    }
    if (e.kind == EA_D) {
        rv_sb(R, RS_, DREG(reg), S0);        /* (CYC_SCC_R_TRUE: 0 on the 68020) */
        return m68ki_cpu.cyc_scc_r_true == 0;
    }
    ea_write(&e, 1, RS_);
    return true;
}

/* MOVEM */
static bool t_movem(uint16_t op)
{
    bool to_regs = op & 0x400, l = op & 0x40;
    int mode = (op >> 3) & 7, reg = op & 7, size = l ? 4 : 2;
    uint32_t list = ext16();
    ea_t e;
    if (mode < 2 || (to_regs && mode == 4) || (!to_regs && mode == 3) || (mode == 7 && reg > (to_regs ? 3 : 1))) {
        return false;
    }
    int count = __builtin_popcount(list);
    int32_t step = l ? 4 : 2;
    if (mode == 4) {                        /* registers to -(An): from A7 down */
        rv_lw(R, RA_, AREG(reg), S0);
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) {
                continue;
            }
            rv_addi(R, RA_, RA_, -step);
            rv_lw(R, RS_, DREG(15 - i), S0);
            if (l) {
                rv_slli(R, A1, RS_, 16);
                rv_srli(R, A1, A1, 16);
                ea_t a = { .kind = EA_MEM, .areg = RA_ };
                addr_arg(&a, 2);
                mem_routine(s_write[2]);
                rv_srli(R, A1, RS_, 16);
                addr_arg(&a, 0);
                mem_routine(s_write[2]);
            } else {
                mask_reg(A1, RS_, 2);
                ea_t a = { .kind = EA_MEM, .areg = RA_ };
                addr_arg(&a, 0);
                mem_routine(s_write[2]);
            }
        }
        rv_sw(R, RA_, AREG(reg), S0);
        cycles(count << (l ? m68ki_cpu.cyc_movem_l : m68ki_cpu.cyc_movem_w));
        return true;
    }
    if (mode == 3) {
        rv_lw(R, RA_, AREG(reg), S0);
    } else {
        if (!ea_decode(mode, reg, size, &e, RA_)) {
            return false;
        }
        if (e.stat) {
            rv_li(R, RA_, e.value);
        }
    }
    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) {
            continue;
        }
        ea_t a = { .kind = EA_MEM, .areg = RA_ };
        addr_arg(&a, 0);
        if (to_regs) {
            mem_routine(s_read[size]);
            if (!l) {
                sext_reg(A0, A0, 2);
            }
            rv_sw(R, A0, DREG(i), S0);
        } else {
            rv_lw(R, A1, DREG(i), S0);
            if (!l) {
                mask_reg(A1, A1, 2);
            }
            mem_routine(s_write[size]);
        }
        rv_addi(R, RA_, RA_, step);
    }
    if (mode == 3) {
        rv_sw(R, RA_, AREG(reg), S0);
    }
    cycles(count << (l ? m68ki_cpu.cyc_movem_l : m68ki_cpu.cyc_movem_w));
    return true;
}

/* m68ki_push_32(v in rs) */
static void push32(int rs)
{
    rv_lw(R, T0, AREG(7), S0);
    rv_addi(R, T0, T0, -4);
    rv_sw(R, T0, AREG(7), S0);
    rv_mv(R, A1, rs);
    rv_mv(R, A0, T0);
    mem_routine(ST_WR32);
}

/* ---- One instruction ---- */

static uint32_t s_cyc;                      /* (its table's cycles) */

/* An instruction that ends the block or branches: the cycles are its
 * business. Returns true when translated; *flow when it ended the flow
 * (nothing falls through) */
static bool t_flow(uint16_t op, bool *flow, bool *done)
{
    uint32_t a = T.u->pc;
    *done = true;
    if ((op & 0xF000) == 0x6000) {
        int cc = (op >> 8) & 15;
        int32_t d8 = (int8_t)op;
        if (d8 == -1) {
            return false;                   /* (.L) */
        }
        uint32_t target;
        bool w = d8 == 0;
        if (cc == 1) {                      /* BSR */
            uint32_t ret;
            if (w) {
                target = a + 2 + (uint32_t)(int32_t)(int16_t)ext16();
                ret = a + 4;
            } else {
                target = a + 2 + (uint32_t)d8;
                ret = a + 2;
            }
            rv_li(R, RS_, ret);
            push32(RS_);
            cycles((int32_t)s_cyc);
            rv_jl(R, new_link(target, w ? a + 4 : a + 2, CHK_S1));
            *flow = true;
            return true;
        }
        if (cc == 0) {                      /* BRA */
            target = w ? a + 2 + (uint32_t)(int32_t)(int16_t)T.u->host[1] : a + 2 + (uint32_t)d8;
            if (target == a) {
                return false;               /* (USE_ALL_CYCLES: the handler's) */
            }
            if (w) {
                ext16();
            }
            cycles((int32_t)s_cyc);
            branch_to(target, w ? a + 4 : a + 2);
            *flow = true;
            return true;
        }
        /* Bcc */
        int not_taken = rv_new_label(R);
        cond_branch(cc, false, not_taken);
        if (w) {
            target = a + 2 + (uint32_t)(int32_t)(int16_t)ext16();
        } else {
            target = a + 2 + (uint32_t)d8;
        }
        cycles((int32_t)s_cyc);
        branch_to(target, w ? a + 4 : a + 2);
        rv_place(R, not_taken);
        cycles((int32_t)s_cyc + (w ? m68ki_cpu.cyc_bcc_notake_w : m68ki_cpu.cyc_bcc_notake_b));
        T.fetch = a + (w ? 4 : 2);          /* (the displacement skipped) */
        T.fe = a + 2;
        *flow = false;
        return true;
    }
    if ((op & 0xF0F8) == 0x50C8) {          /* DBcc */
        int cc = (op >> 8) & 15, reg = op & 7;
        int done_l = rv_new_label(R), expired = rv_new_label(R);
        uint32_t target = a + 2 + (uint32_t)(int32_t)(int16_t)T.u->host[1];
        if (cc == 0) {
            return false;                   /* (DBT: rare) */
        }
        if (cc > 1) {
            cond_branch(cc, true, done_l);  /* condition true: on, no count */
        }
        rv_lw(R, T0, DREG(reg), S0);
        rv_addi(R, T1, T0, -1);
        rv_sh(R, T1, DREG(reg), S0);
        rv_slli(R, T2, T0, 16);
        rv_beqz(R, T2, expired);            /* (was 0: now $FFFF) */
        ext16();
        cycles((int32_t)s_cyc + m68ki_cpu.cyc_dbcc_f_noexp);
        branch_to(target, a + 4);
        rv_place(R, expired);
        cycles((int32_t)s_cyc + m68ki_cpu.cyc_dbcc_f_exp);
        int join = rv_new_label(R);
        rv_jl(R, join);
        rv_place(R, done_l);
        cycles((int32_t)s_cyc);
        rv_place(R, join);
        T.fetch = a + 4;
        T.fe = a + 2;
        *flow = false;
        return true;
    }
    if (op == 0x4E75) {                     /* RTS */
        rv_lw(R, RA_, AREG(7), S0);
        rv_addi(R, T0, RA_, 4);
        rv_sw(R, T0, AREG(7), S0);
        ea_t e = { .kind = EA_MEM, .areg = RA_ };
        addr_arg(&e, 0);
        mem_routine(s_read[4]);
        rv_sw(R, A0, OFF(pc), S0);
        cycles((int32_t)s_cyc);
        rv_jl(R, new_dyn(a + 2));
        *flow = true;
        return true;
    }
    if ((op & 0xFF80) == 0x4E80) {          /* JSR, JMP */
        bool jsr = !(op & 0x40);
        int mode = (op >> 3) & 7, reg = op & 7;
        ea_t e;
        if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) {
            return false;
        }
        if (!ea_decode(mode, reg, 4, &e, RA_)) {
            return false;
        }
        uint32_t next = T.fetch;
        if (e.stat && !jsr && e.value == a) {
            return false;                   /* (JMP to itself: USE_ALL_CYCLES) */
        }
        if (jsr) {
            rv_li(R, RS_, next);
            push32(RS_);
        }
        if (e.stat) {
            cycles((int32_t)s_cyc);
            rv_jl(R, new_link(e.value, next, CHK_S1));
        } else {
            rv_sw(R, RA_, OFF(pc), S0);
            if (!jsr) {
                /* (to itself: USE_ALL_CYCLES, the cycles left modulo its count) */
                int skip = rv_new_label(R);
                rv_li(R, T0, a);
                rv_bcc(R, RV_BNE, RA_, T0, skip);
                rv_li(R, T0, s_cyc);
                rv_rem(R, S1, S1, T0);
                rv_place(R, skip);
            }
            cycles((int32_t)s_cyc);
            rv_jl(R, new_dyn(next));
        }
        *flow = true;
        return true;
    }
    *done = false;
    return false;
}

/* The rest: the body only (the cycles and the exit after it are the caller's) */
static bool t_body(uint16_t op)
{
    switch (op >> 12) {
    case 0x0:
        if (op & 0x100 || (op & 0x0F00) == 0x0800) {
            return (op & 0x38) != 0x08 && t_bit(op);
        }
        return t_imm(op);
    case 0x1: case 0x2: case 0x3:
        return t_move(op);
    case 0x4:
        if ((op & 0xFFF8) == 0x49C0) {
            return t_ext_swap(op);          /* EXTB.L (before LEA's pattern) */
        }
        if ((op & 0xF1C0) == 0x41C0) {
            return t_lea(op, false);
        }
        if ((op & 0xFFC0) == 0x4840 && (op & 0x38)) {
            return t_lea(op, true);
        }
        if ((op & 0xFFF8) == 0x4840 || (op & 0xFFB8) == 0x4880 || (op & 0xFFF8) == 0x49C0) {
            return t_ext_swap(op);
        }
        if ((op & 0xFF00) == 0x4A00 && (op & 0xC0) != 0xC0) {
            return t_tst_clr(op, false);
        }
        if ((op & 0xFF00) == 0x4200 && (op & 0xC0) != 0xC0) {
            return t_tst_clr(op, true);
        }
        if ((op & 0xFF00) == 0x4400 && (op & 0xC0) != 0xC0) {
            return t_neg_not(op, false);
        }
        if ((op & 0xFF00) == 0x4600 && (op & 0xC0) != 0xC0) {
            return t_neg_not(op, true);
        }
        if ((op & 0xFB80) == 0x4880 && (op & 0x38)) {
            return t_movem(op);
        }
        if (op == 0x4E71) {
            return true;                    /* NOP */
        }
        return false;
    case 0x5:
        if ((op & 0xC0) == 0xC0) {
            return (op & 0x38) != 0x08 && t_scc(op);
        }
        return t_addq(op);
    case 0x7:
        return t_moveq(op);
    case 0x8:
        return t_alu(op, OP_OR);
    case 0x9:
        if ((op & 0x130) == 0x100 && (op & 0xC0) != 0xC0) {
            return t_addx(op, true);
        }
        return t_alu(op, OP_SUB);
    case 0xB:
        if ((op & 0x100) && (op & 0xC0) != 0xC0) {
            return (op & 0x38) != 0x08 && t_alu(op, OP_EOR);
        }
        return t_alu(op, OP_CMP);
    case 0xC:
        if ((op & 0x1C0) == 0x0C0 || (op & 0x1C0) == 0x1C0) {
            return t_mul(op);
        }
        if ((op & 0x130) == 0x100) {
            return t_exg(op);
        }
        return t_alu(op, OP_AND);
    case 0xD:
        if ((op & 0x130) == 0x100 && (op & 0xC0) != 0xC0) {
            return t_addx(op, false);
        }
        return t_alu(op, OP_ADD);
    case 0xE:
        if ((op & 0xC0) == 0xC0) {
            return false;
        }
        /* (the rest of the shifts and rotates of Dn: by a register, ROXL,
         * ROXR, ROL and ROR of bytes and words) */
        return t_shift(op) || t_handler(op);
    }
    return false;
}

/* ---- The block ---- */

static bool ends_block(uint16_t op)
{
    if ((op & 0xF000) == 0x6000) {
        return ((op >> 8) & 0xF) <= 1;      /* BRA, BSR (Bcc go on when not taken) */
    }
    if ((op & 0xF000) == 0xA000 || (op & 0xF000) == 0xF000) {
        return true;                        /* line A, line F */
    }
    if ((op & 0xFF80) == 0x4E80 || (op & 0xFFC0) == 0x4EC0) {
        return true;                        /* JSR, JMP */
    }
    if ((op & 0xFFF0) == 0x4E40) {
        return true;                        /* TRAP */
    }
    switch (op) {
    case 0x4E70: case 0x4E72: case 0x4E73: case 0x4E74: case 0x4E75: case 0x4E77: case 0x4AFC:
        return true;                        /* RESET STOP RTE RTD RTS RTR ILLEGAL */
    }
    return false;
}

/* An operand's extension words (mode, reg; the size for #imm; w: its first
 * word): -1 not known here */
static int ea_words(int mode, int reg, int size, const uint16_t *w)
{
    if (mode < 5) {
        return 0;
    }
    if (mode == 5 || (mode == 7 && (reg == 0 || reg == 2))) {
        return 1;
    }
    if (mode == 6 || (mode == 7 && reg == 3)) {
        return w[0] & 0x100 ? -1 : 1;       /* (brief; the full format: the disassembler's) */
    }
    if (mode == 7 && reg == 1) {
        return 2;
    }
    if (mode == 7 && reg == 4) {
        return size == 4 ? 2 : 1;
    }
    return -1;
}

/* The bytes of the instruction at w, for the families translated here; 0:
 * ask the disassembler */
static unsigned insn_len(const uint16_t *w)
{
    uint16_t op = w[0];
    int mode = (op >> 3) & 7, reg = op & 7, n;
    switch (op >> 12) {
    case 0x1: case 0x2: case 0x3: {         /* MOVE */
        int s = (op >> 12) & 3, size = s == 1 ? 1 : s == 3 ? 2 : 4;
        int a = ea_words(mode, reg, size, w + 1);
        if (a < 0) {
            return 0;
        }
        int b = ea_words((op >> 6) & 7, (op >> 9) & 7, size, w + 1 + a);
        return b < 0 || (((op >> 6) & 7) == 7 && ((op >> 9) & 7) > 1) ? 0 : (unsigned)(2 + 2 * (a + b));
    }
    case 0x7:
        return 2;
    case 0x6:
        return (op & 0xFF) == 0 ? 4 : (op & 0xFF) == 0xFF ? 6 : 2;
    case 0x5:
        if ((op & 0xF8) == 0xC8) {
            return 4;                       /* DBcc */
        }
        if ((op & 0xC0) == 0xC0 && mode == 7 && reg > 1) {
            return 0;                       /* (TRAPcc) */
        }
        n = ea_words(mode, reg, 1, w + 1);
        return n < 0 ? 0 : (unsigned)(2 + 2 * n);
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        int opmode = (op >> 6) & 7;
        if (opmode >= 4 && opmode <= 6 && mode <= 1) {
            /* ADDX SUBX ABCD SBCD EXG CMPM; PACK and UNPK an adjustment word more */
            return (op >> 12) == 0x8 && opmode >= 5 ? 4 : 2;
        }
        /* (opmode 3 and 7: ADDA SUBA CMPA .W .L; MULU MULS DIVU DIVS .W) */
        int size = opmode == 3 ? 2 : opmode == 7 ? ((op >> 12) == 0x8 || (op >> 12) == 0xC ? 2 : 4)
                 : 1 << (opmode & 3);
        n = ea_words(mode, reg, size, w + 1);
        return n < 0 ? 0 : (unsigned)(2 + 2 * n);
    }
    case 0x4:
        if (op == 0x4E71 || op == 0x4E75 || (op & 0xFFF8) == 0x4840 || (op & 0xFFB8) == 0x4880 || (op & 0xFFF8) == 0x49C0) {
            return 2;                       /* NOP RTS SWAP EXT EXTB */
        }
        if ((op & 0xF1C0) == 0x41C0 || (op & 0xFFC0) == 0x4840 || (op & 0xFF80) == 0x4E80 ||
            ((op & 0xF900) == 0x4000 && (op & 0xC0) != 0xC0 && (op & 0xFF00) != 0x4800) || (op & 0xFF00) == 0x4A00) {
            /* LEA PEA JSR JMP; NEGX CLR NEG NOT; TST (TAS too) */
            int size = (op & 0xF1C0) == 0x41C0 || (op & 0xFF80) == 0x4E80 || (op & 0xFFC0) == 0x4840 ? 4
                     : 1 << ((op >> 6) & 3);
            n = ea_words(mode, reg, size, w + 1);
            return n < 0 ? 0 : (unsigned)(2 + 2 * n);
        }
        if ((op & 0xFB80) == 0x4880) {
            n = ea_words(mode, reg, 4, w + 2);  /* MOVEM: the list, then the operand */
            return n < 0 ? 0 : (unsigned)(4 + 2 * n);
        }
        return 0;
    case 0xE:
        if ((op & 0xC0) != 0xC0) {
            return 2;                       /* shifts of Dn */
        }
        return 0;
    case 0x0:
        if ((op & 0x0100) || (op & 0x0F00) == 0x0800) {
            if ((op & 0x0100) && mode == 1) {
                return 4;                   /* MOVEP */
            }
            int k = (op & 0x0100) ? 0 : 1;  /* (#n: a word) */
            n = ea_words(mode, reg, 1, w + 1 + k);
            return n < 0 ? 0 : (unsigned)(2 + 2 * (k + n));
        }
        if ((op & 0xC0) == 0xC0 || (op & 0x0E00) == 0x0800 || (op & 0x0E00) == 0x0E00) {
            return 0;                       /* (CMP2 CHK2 CAS MOVES CALLM ...) */
        }
        if (mode == 7 && reg == 4) {
            return 4;                       /* (to CCR, SR) */
        }
        {
            int size = 1 << ((op >> 6) & 3), k = size == 4 ? 2 : 1;
            n = ea_words(mode, reg, size, w + 1 + k);
            return n < 0 ? 0 : (unsigned)(2 + 2 * (k + n));
        }
    }
    return 0;
}

/* The instructions from pc (at most max) */
static int decode(uint32_t pc, int max)
{
    char text[100];
    uint32_t a = pc;
    int n = 0;
    while (n < max) {
        uint8_t w;
        const uint16_t *host = mem_code_ptr(a, &w);
        if (!host) {
            break;
        }
        /* (its words in this bank; an illegal one: the disassembler's 2) */
        unsigned len = (a & 0xFFFF) <= 0xFFF0 && (*m68k_loop.handlers)[*host] != (*m68k_loop.handlers)[0x4AFC]
                     ? insn_len(host) : 0;
        if (!len) {
            len = m68k_disassemble(text, a, M68K_CPU_TYPE_68EC020);
        }
#ifndef ESP_PLATFORM
        else {
            unsigned d = m68k_disassemble(text, a, M68K_CPU_TYPE_68EC020);
            if (d != len) {
                fprintf(stderr, "jit: $%04x at $%06x: %u bytes, the disassembler %u (%s)\n", *host, a, len, d, text);
                len = d;
            }
        }
#endif
        uint32_t next = a + len;
        if (len < 2 || (next - pc) > MAX_BYTES || ((next - 1) >> 16) != (pc >> 16)) {
            break;                          /* (never across a bank) */
        }
        tinsn_t *u = &T.in[n++];
        memset(u, 0, sizeof(*u));
        u->pc = a;
        u->next = next;
        u->host = host;
        u->op = *host;
        u->wait = w;
        a = next;
        if (ends_block(u->op)) {
            break;
        }
    }
    return n;
}

static void fallback(void)
{
    tinsn_t *u = T.u;
    materialize();
    jit_fb_t *f = &T.fb[T.nfb++];
    f->pc = u->pc;
    f->op = u->op;
    f->len = (uint8_t)(u->next - u->pc);
    f->wait = u->wait;
    BT(JB_FB, rv_li(R, A0, addr32(f)); jal_abs(RA, s_stub[ST_FB]); rv_bnez(R, A0, new_exit(JIT_EXIT_FB, 0, 0)));
    T.fe_dyn = true;
}

static int s_cut;                           /* (the instruction the code budget ran out at) */
static jit_exit_t s_exits[2 * MAX_INSNS + 8];   /* (each written after its exit's code) */
_Static_assert(sizeof(jit_exit_t) == 16, "an exit's record: four words in the code");

/* Internal branch targets: their instructions are reached by jumps too */
static void find_targets(void)
{
    for (int i = 0; i < T.n; i++) {
        tinsn_t *u = &T.in[i];
        uint16_t op = u->op;
        uint32_t t = 0;
        if ((op & 0xF000) == 0x6000 && ((op >> 8) & 15) != 1 && (int8_t)op != -1) {
            t = (op & 0xFF) ? u->pc + 2 + (uint32_t)(int32_t)(int8_t)op
                            : u->pc + 2 + (uint32_t)(int32_t)(int16_t)u->host[1];
        } else if ((op & 0xF0F8) == 0x50C8) {
            t = u->pc + 2 + (uint32_t)(int32_t)(int16_t)u->host[1];
        }
        int j = t ? insn_at(t) : -1;
        if (j >= 0) {
            T.in[j].target = true;
        }
    }
}

static bool emit_block(void)
{
    void (*illegal)(void) = (*m68k_loop.handlers)[0x4AFC];
    T.fe_dyn = true;
    T.fe = 0;
    T.s5 = -1;
    T.pk = PK_NONE;
    T.psize = 4;
    s_cut = -1;
    find_targets();
    for (int i = 0; i < T.n; i++) {
        T.in[i].body = rv_new_label(R);     /* (forward branches reach them before they're placed) */
    }
    /* the dispatcher's entry: the prologue, then the entry the fetch state
     * says (the opcode reached in sequence or not: only a second word
     * differs). A linked exit comes to an entry directly: the tags' base
     * (s2: charge_longword()) and the first opcode's fetch */
    uint32_t pc0 = T.in[0].pc;
    T.u = &T.in[0];
#ifndef ESP_PLATFORM
    memset(s_bt, 0, sizeof(s_bt));
    s_btd = 0;
#endif
    BT(JB_ENTRY, jal_abs(T0, s_stub[ST_PROLOGUE]));
    T.entry_l[1] = -1;
    if (pc0 & 2) {
        T.entry_l[1] = rv_new_label(R);
        BT(JB_ENTRY, rv_lw(R, T0, T.ic_next_off, S0); rv_li(R, T1, pc0); rv_bcc(R, RV_BEQ, T0, T1, T.entry_l[1]));
    }
    T.entry_l[0] = rv_new_label(R);
    rv_place(R, T.entry_l[0]);
    hook(pc0);
    BT(JB_ENTRY, rv_li(R, S2, (pc0 & ~3u) | 1));
    charge_longword(pc0);
    if (pc0 & 2) {
        BT(JB_ENTRY, rv_jl(R, T.in[0].body));
        rv_place(R, T.entry_l[1]);
        hook(pc0);
        BT(JB_ENTRY, rv_li(R, S2, (pc0 & ~3u) | 1));
    }
    for (int i = 0; i < T.n; i++) {
        tinsn_t *u = &T.in[i];
        T.u = u;
        T.fetch = u->pc + 2;
        T.wrote = false;
        if (u->target) {
            materialize();                  /* (jumps here bring them in memory) */
        }
        if (i > 0) {
            hook(u->pc);
            opcode_charge(u->pc, T.fe_dyn, T.fe);
        }
        rv_place(R, u->body);
        if (u->target) {
            T.s5 = -1;                      /* (reached from elsewhere too) */
        }
        if (i > 0) {
            /* (the state here, for a slice resumed at this instruction) */
            uint32_t body = (uint32_t)(R->p - R->start);
            T.res[i] = body > 0x7FF ? 0 : JIT_RES(body, u->pc - pc0, T.s5 < 0 ? 0xFF : (uint32_t)T.s5 - pc0,
                                                    T.pk, T.psize == 1 ? 0 : T.psize == 2 ? 1 : 2);
        }
        s_cyc = (*m68k_loop.cycles)[u->op];
        /* the translation, or the handler */
        uint32_t *p0 = R->p;
        int nl = R->nlabels, nf = R->nfixups, ne = T.nexits;
        int64_t s5 = T.s5;
        int pk = T.pk, ps = T.psize;
#ifndef ESP_PLATFORM
        unsigned long bt[JB_N];
        memcpy(bt, s_bt, sizeof(bt));
#endif
        bool flow = false, done = false, ok = false;
        bool native = (*m68k_loop.handlers)[u->op] != illegal;
        if (native) {
            ok = t_flow(u->op, &flow, &done);
            if (!done) {
                T.fe = 0;
                ok = t_body(u->op);
                if (ok) {
                    cycles((int32_t)s_cyc);
                    T.fe = T.fetch;
                }
            }
            if (ok && T.fetch != u->next && !flow) {
                ok = false;                 /* (a length the disassembler disagrees with) */
#ifndef ESP_PLATFORM
                fprintf(stderr, "jit: $%04x at $%06x: %u bytes, the disassembler %u\n", u->op, u->pc,
                        T.fetch - u->pc, u->next - u->pc);
#endif
            }
        }
        if (!ok) {
            R->p = p0;
            R->full = false;
            R->nlabels = nl;
            R->nfixups = nf;
            T.nexits = ne;
            T.fetch = u->pc + 2;
            T.s5 = s5;
            T.pk = pk, T.psize = ps;
            T.wrote = false;
            flow = false;
#ifndef ESP_PLATFORM
            memcpy(s_bt, bt, sizeof(bt));
#endif
            fallback();
        } else {
            T.fe_dyn = false;
            if (!flow) {
                int ex = new_exit(JIT_EXIT_PC, u->next, T.fe);
                BT(JB_CHECK, rv_blez(R, S1, ex));
                if (T.wrote) {
                    BT(JB_CHECK, abs_access(4, false, T0, addr32((const void *)&jit_stop)); rv_bnez(R, T0, ex));
                }
            }
        }
        if (R->full) {
            return false;
        }
        if ((R->p - R->start) * 4 > BODY_LIMIT && i + 1 < T.n) {
            s_cut = i + 1;
            return false;
        }
    }
    /* the end: on to the next instruction's block (unless the last ended the flow) */
    T.u = &T.in[T.n - 1];
    bool last_flow = ends_block(T.u->op);
    if (!last_flow) {
        /* (after a handler its state stands; else on to the next block, the
         * last instruction's checks done) */
        int ex = T.fe_dyn ? new_exit(JIT_EXIT_FB, T.u->next, 0) : new_link(T.u->next, T.fe, 0);
        BT(JB_XJAL, rv_jl(R, ex));
    } else if (T.fe_dyn) {
        /* (a handler ended it: its own exit) */
        BT(JB_XJAL, rv_jl(R, new_exit(JIT_EXIT_FB, 0, 0)));
    }
#ifndef ESP_PLATFORM
    {
        unsigned long body = (unsigned long)(R->p - R->start) * 4;
        for (int c = JB_ENTRY; c < JB_REST; c++) {
            body -= s_bt[c];
        }
        s_bt[JB_REST] = body - s_bt[JB_XJAL];
    }
#endif
    /* the exits: the flags to memory, a link's checks and its two words
     * (nothing until linked), then on to the shared exit with the record's
     * address in a0 (it follows the jal) */
    for (int k = 0; k < T.nexits; k++) {
        rv_place(R, T.exit_label[k]);
        if (T.exit_pk[k] != PK_NONE) {
            BT(JB_XMAT, jal_abs(T0, s_mat[T.exit_pk[k]][T.exit_ps[k]]));
        }
        int kind = T.exit[k].kind;
        int out = rv_new_label(R);
        if (kind == JIT_EXIT_LINK || kind == JIT_EXIT_DYN) {
            if (T.exit_chk[k] & CHK_S1) {
                BT(JB_XCHK, rv_blez(R, S1, out));
            }
            if (T.exit_chk[k] & CHK_STOP) {
                BT(JB_XCHK, abs_access(4, false, T0, addr32((const void *)&jit_stop)); rv_bnez(R, T0, out));
            }
        }
        if (kind == JIT_EXIT_LINK) {
            BT(JB_XNOPS, rv_word(R, RV_NOP); rv_word(R, RV_NOP));
        } else if (kind == JIT_EXIT_DYN) {
            /* the PC one of the targets kept after the record (JIT_DYN_*
             * from the auipc: words 14-17), a0 the jal to the exit (as a
             * link's: ST_BAIL) */
            int hit = rv_new_label(R);
            uint32_t *x0 = R->p;
            rv_lw(R, T1, OFF(pc), S0);
            rv_word(R, (uint32_t)T2 << 7 | 0x17);              /* auipc t2, 0 */
            rv_lw(R, T0, JIT_DYN_PC0, T2);
            rv_addi(R, T3, T2, JIT_DYN_TO0);
            rv_bcc(R, RV_BEQ, T0, T1, hit);
            rv_lw(R, T0, JIT_DYN_PC1, T2);
            rv_addi(R, T3, T2, JIT_DYN_TO1);
            rv_bcc(R, RV_BNE, T0, T1, out);
            rv_place(R, hit);
            rv_lw(R, T0, 0, T3);
            rv_jalr(R, A0, T0, 0);
#ifndef ESP_PLATFORM
            s_bt[JB_XDYN] += (unsigned long)(R->p - x0) * 4;
#else
            (void)x0;
#endif
        }
        rv_place(R, out);
        /* (a slice may resume after a slice's, a write's or a handler's exit:
         * the registers kept) */
        BT(JB_XJAL, jal_abs(A0, s_stub[kind == JIT_EXIT_PC || kind == JIT_EXIT_FB ? ST_EXIT_RF : ST_EXIT]));
        uint32_t w[4];
        memcpy(w, &T.exit[k], sizeof(w));
        BT(JB_RECORD, for (int i = 0; i < 4; i++) { rv_word(R, w[i]); });
        if (kind == JIT_EXIT_DYN) {
            /* (no target yet: an odd PC never matches) */
            BT(JB_DYNTGT, rv_word(R, 1); rv_word(R, 0); rv_word(R, 1); rv_word(R, 0));
        }
    }
    return !R->full && rv_link(R);
}

/* ---- The shared routines ---- */

static int ptr_shift(void)
{
    return sizeof(void *) == 8 ? 3 : 2;
}

/* the slow path: the callback (a0, a1 as given), REG_PPC and the cycles
 * left as the interpreter has them */
static void slow_call(const void *f, int size_mask_a1)
{
    rv_sw(R, S5, OFF(ppc), S0);
    st_rem();
    rv_addi(R, SP, SP, -16);
    rv_sw(R, RA, 12, SP);
    if (size_mask_a1 == 1) {
        rv_andi(R, A1, A1, 0xFF);
    } else if (size_mask_a1 == 2) {
        rv_slli(R, A1, A1, 16);
        rv_srli(R, A1, A1, 16);
    }
    rv_call(R, addr32(f));
    rv_lw(R, RA, 12, SP);
    rv_addi(R, SP, SP, 16);
    ld_rem();
    rv_ret(R);
}

/* jit_ctx (jit.c, beside Musashi's state): the bases the routines take */
enum { CTX_RMAP, CTX_WMAP, CTX_WAIT, CTX_PAGES };

static int32_t ctx_off(int i)
{
    return (int32_t)(addr32(&jit_ctx[i]) - T.cpu);
}

/* jit_rf's and jit_rs's words from s0 */
static int32_t rf_off(int i)
{
    return (int32_t)(addr32(&jit_rf[i]) - T.cpu);
}

static int32_t rs_off(int i)
{
    return (int32_t)(addr32(&jit_rs[i]) - T.cpu);
}

/* the fetch state's offsets from s0 (AMI_SDATA keeps it within reach) */
static bool fetch_offsets(void)
{
    T.ic_next_off = (int32_t)(addr32(T.env.ic_next) - T.cpu);
    T.ic_tag_off = (int32_t)(addr32(T.env.ic_tag) - T.cpu);
    T.ic_cacr_off = (int32_t)(addr32(T.env.ic_cacr) - T.cpu);
    return imm12(T.ic_next_off) && imm12(T.ic_tag_off) && imm12(T.ic_tag_off + 252) && imm12(T.ic_cacr_off);
}

/* t2 = the bank's pointer from a map (t1 = the bank); to slow when NULL */
static void bank_ptr(int map, int slow)
{
    rv_srli(R, T1, A0, 16);
    rv_lw(R, T3, ctx_off(map), S0);
    rv_slli(R, T2, T1, ptr_shift());
    rv_add(R, T2, T2, T3);
    rv_lw(R, T2, 0, T2);
    rv_beqz(R, T2, slow);
}

/* the bank's wait (t1 = the bank) */
static void bank_wait(void)
{
    rv_lw(R, T3, ctx_off(CTX_WAIT), S0);
    rv_add(R, T1, T1, T3);
    rv_lbu(R, T1, 0, T1);
    rv_sub(R, S1, S1, T1);
}

/* to slow when the bytes [a0, a0 + n) touch a 2 KB page of translated code */
static void code_page(int n, int slow)
{
    rv_lw(R, T4, ctx_off(CTX_PAGES), S0);
    rv_srli(R, T3, A0, 11);
    rv_add(R, T3, T3, T4);
    rv_lbu(R, T3, 0, T3);
    rv_bnez(R, T3, slow);
    if (n == 4) {
        rv_addi(R, T3, A0, 3);
        rv_srli(R, T3, T3, 11);
        rv_add(R, T3, T3, T4);
        rv_lbu(R, T3, 0, T3);
        rv_bnez(R, T3, slow);
    }
}

/* the pending flags to memory (jal t0), each kind and size */
static void mat_routine(int kind, int size)
{
    int sh = nshift(size);
    s_mat[kind][size] = addr32(R->p);
    if (kind != PK_Z) {
        if (sh) {
            rv_srli(R, T1, F_R, sh);
            st_flag(T1, OFF(n_flag));
        } else {
            st_flag(F_R, OFF(n_flag));
        }
    }
    st_flag(F_R, OFF(not_z_flag));
    if (kind == PK_LOGIC) {
        st_flag(X0, OFF(v_flag));
        st_flag(X0, OFF(c_flag));
    } else if (kind == PK_SUB || kind == PK_ADD) {
        if (kind == PK_SUB) {
            rv_xor(R, T1, F_S, F_D);        /* (s ^ d) & (r ^ d) */
            rv_xor(R, T2, F_R, F_D);
        } else {
            rv_xor(R, T1, F_S, F_R);        /* (s ^ r) & (d ^ r) */
            rv_xor(R, T2, F_D, F_R);
        }
        rv_and(R, T1, T1, T2);
        if (sh) {
            rv_srli(R, T1, T1, sh);
        }
        st_flag(T1, OFF(v_flag));
        if (kind == PK_SUB) {
            rv_sltu(R, T1, F_D, F_S);
        } else {
            rv_sltu(R, T1, F_R, F_S);
        }
        rv_slli(R, T1, T1, 8);
        st_flag(T1, OFF(c_flag));
    }
    rv_jalr(R, X0, T0, 0);
}

/* to slow when a0's n bytes cross the bank (t1 = the bank) */
static void same_bank(int n, int slow)
{
    rv_addi(R, T3, A0, n - 1);
    rv_srli(R, T3, T3, 16);
    rv_bcc(R, RV_BNE, T3, T1, slow);
}

static void mask24(void)
{
    rv_slli(R, A0, A0, 8);
    rv_srli(R, A0, A0, 8);
}

bool jit_rv_stubs(uint8_t **at, uint8_t *end)
{
    T.cpu = addr32(&m68ki_cpu);
    mem_jit_env(&T.env);
    jit_ctx[CTX_RMAP] = addr32(T.env.rmap);
    jit_ctx[CTX_WMAP] = addr32(T.env.wmap);
    jit_ctx[CTX_WAIT] = addr32(T.env.wait);
    jit_ctx[CTX_PAGES] = addr32(jit_code_pages);
    for (int i = 0; i < 4; i++) {
        if (!imm12(ctx_off(i))) {
            return false;
        }
    }
    for (int i = 0; i < JIT_RS_N; i++) {
        if (!imm12(rs_off(i)) || (i < 4 && !imm12(rf_off(i)))) {
            return false;
        }
    }
    if (!fetch_offsets()) {
        return false;
    }
    R->start = R->p = (uint32_t *)*at;
    R->end = (uint32_t *)end;
    R->full = false;

    /* the prologue (jal t0): ra and s0-s11 kept, the registers set up */
    s_stub[ST_PROLOGUE] = addr32(R->p);
    R->nlabels = R->nfixups = 0;
    rv_addi(R, SP, SP, -64);
    rv_sw(R, RA, 60, SP);
    for (int i = 0; i < 12; i++) {
        rv_sw(R, i < 2 ? S0 + i : S2 + i - 2, 56 - 4 * i, SP);
    }
    rv_li(R, S0, T.cpu);
    ld_rem();
    abs_access(2, false, S2, addr32(T.env.ic_cacr));
    /* s9: the tags while the cache fills (E without F), else a dummy's */
    rv_andi(R, S9, S2, 3);
    rv_addi(R, S9, S9, -1);
    rv_li(R, S2, addr32(T.env.ic_tag));
    {
        int fills = rv_new_label(R);
        rv_beqz(R, S9, fills);
        rv_li(R, S2, addr32(s_dummy_tags));
        rv_place(R, fills);
    }
    rv_mv(R, S9, S2);
    rv_jalr(R, X0, T0, 0);
    if (!rv_link(R)) {
        return false;                       /* (its branch: before the routines' labels start again) */
    }

    /* the exit (a0 = the exit's record); before it, for an exit a slice may
     * resume after, the pending flags' registers and s2 kept (jit_rf:
     * jit_rv_resume()) */
    s_stub[ST_EXIT_RF] = addr32(R->p);
    for (int i = 0; i < 3; i++) {
        rv_sw(R, F_S + i, rf_off(i), S0);
    }
    rv_sw(R, S2, rf_off(3), S0);
    s_stub[ST_EXIT] = addr32(R->p);
    st_rem();
    rv_lw(R, RA, 60, SP);
    for (int i = 0; i < 12; i++) {
        rv_lw(R, i < 2 ? S0 + i : S2 + i - 2, 56 - 4 * i, SP);
    }
    rv_addi(R, SP, SP, 64);
    rv_ret(R);

    /* a dropped block's entry (jit_rv_kill()): out as the link exit that
     * jumped there (a0 its jal to the exit, the record after it) */
    s_stub[ST_BAIL] = addr32(R->p);
    rv_addi(R, A0, A0, 4);
    jal_abs(X0, s_stub[ST_EXIT]);

    /* a handler (a0 = its entry): jit_fallback() with the cycles left in
     * memory */
    s_stub[ST_FB] = addr32(R->p);
    rv_addi(R, SP, SP, -16);
    rv_sw(R, RA, 12, SP);
    st_rem();
    rv_call(R, addr32((const void *)jit_fallback));
    ld_rem();
    rv_lw(R, RA, 12, SP);
    rv_addi(R, SP, SP, 16);
    rv_ret(R);

    /* a slice resumed inside a block (jit_rs: jit_rv_resume()): the
     * prologue, the code's state there (s2 its block's tags' base, s5, the
     * pending flags' registers), the opcode's fetch as the fetch state says
     * (opcode_charge(), its address in a0, the bank's wait in t3), then its
     * body */
    {
        R->nlabels = R->nfixups = 0;
        uint32_t *base = R->start;
        R->start = R->p;
        s_stub[ST_RESUME] = addr32(R->p);
        int charge = rv_new_label(R), nowait = rv_new_label(R), miss = rv_new_label(R), go = rv_new_label(R);
        jal_abs(T0, s_stub[ST_PROLOGUE]);
        rv_lw(R, S2, rs_off(JIT_RS_S2), S0);
        rv_lw(R, S5, rs_off(JIT_RS_S5), S0);
        for (int i = 0; i < 3; i++) {
            rv_lw(R, F_S + i, rs_off(JIT_RS_F + i), S0);
        }
        rv_lw(R, A0, rs_off(JIT_RS_PC), S0);
        rv_andi(R, T0, A0, 2);
        rv_beqz(R, T0, charge);
        rv_lw(R, T0, T.ic_next_off, S0);
        rv_bcc(R, RV_BEQ, T0, A0, go);      /* (its longword's second word, reached in sequence) */
        rv_place(R, charge);
        rv_lw(R, T3, rs_off(JIT_RS_WAIT), S0);
        rv_andi(R, T1, A0, -4);
        rv_ori(R, T1, T1, 1);               /* (its longword's tag) */
        rv_andi(R, T2, A0, 0xFC);           /* (its line's) */
        rv_add(R, T4, S9, T2);
        rv_beqz(R, T3, nowait);
        rv_lw(R, T0, T.ic_cacr_off, S0);
        rv_andi(R, T0, T0, 1);
        rv_beqz(R, T0, miss);
        rv_add(R, T5, S0, T2);
        rv_lw(R, T5, T.ic_tag_off, T5);
        rv_bcc(R, RV_BEQ, T5, T1, go);
        rv_sw(R, T1, 0, T4);
        rv_place(R, miss);
        rv_sub(R, S1, S1, T3);
        rv_jl(R, go);
        rv_place(R, nowait);
        rv_sw(R, T1, 0, T4);
        rv_place(R, go);
        rv_lw(R, T0, rs_off(JIT_RS_BODY), S0);
        rv_jalr(R, X0, T0, 0);
        if (!rv_link(R)) {
            return false;
        }
        R->start = base;
    }

    /* reads: a0 = the address; the value in a0 (mem.c's callbacks' 68EC020
     * paths, plain memory inline) */
    for (int size = 1; size <= 4; size *= 2) {
        int st = size == 1 ? ST_RD8 : size == 2 ? ST_RD16 : ST_RD32;
        R->nlabels = R->nfixups = 0;
        uint32_t *base = R->start;
        R->start = R->p;
        s_stub[st] = addr32(R->p);
        int slow = rv_new_label(R);
        mask24();
        if (size > 1) {
            rv_andi(R, T0, A0, 1);
            rv_bnez(R, T0, slow);
        }
        bank_ptr(CTX_RMAP, slow);
        if (size == 4) {
            same_bank(4, slow);
        }
        bank_wait();
        if (size == 1) {
            rv_xori(R, T3, A0, 1);
            rv_add(R, T2, T2, T3);
            rv_lbu(R, A0, 0, T2);
        } else if (size == 2) {
            rv_add(R, T2, T2, A0);
            rv_lhu(R, A0, 0, T2);
        } else {
            rv_add(R, T2, T2, A0);
            rv_lhu(R, T0, 0, T2);
            rv_lhu(R, A0, 2, T2);
            rv_slli(R, T0, T0, 16);
            rv_or(R, A0, A0, T0);
        }
        rv_ret(R);
        rv_place(R, slow);
        slow_call(size == 1 ? (const void *)m68k_read_memory_8 : size == 2 ? (const void *)m68k_read_memory_16
                                                                            : (const void *)m68k_read_memory_32, 0);
        if (!rv_link(R)) {
            return false;
        }
        R->start = base;
    }
    /* writes: a0 = the address, a1 the value */
    for (int size = 1; size <= 4; size *= 2) {
        int st = size == 1 ? ST_WR8 : size == 2 ? ST_WR16 : ST_WR32;
        R->nlabels = R->nfixups = 0;
        uint32_t *base = R->start;
        R->start = R->p;
        s_stub[st] = addr32(R->p);
        int slow = rv_new_label(R);
        mask24();
        if (size > 1) {
            rv_andi(R, T0, A0, 1);
            rv_bnez(R, T0, slow);
        }
        bank_ptr(CTX_WMAP, slow);
        if (size == 4) {
            same_bank(4, slow);
        }
        code_page(size, slow);
        bank_wait();
        if (size == 1) {
            rv_xori(R, T3, A0, 1);
            rv_add(R, T2, T2, T3);
            rv_sb(R, A1, 0, T2);
        } else if (size == 2) {
            rv_add(R, T2, T2, A0);
            rv_sh(R, A1, 0, T2);
        } else {
            rv_add(R, T2, T2, A0);
            rv_srli(R, T0, A1, 16);
            rv_sh(R, T0, 0, T2);
            rv_sh(R, A1, 2, T2);
        }
        rv_ret(R);
        rv_place(R, slow);
        slow_call(size == 1 ? (const void *)m68k_write_memory_8 : size == 2 ? (const void *)m68k_write_memory_16
                                                                             : (const void *)m68k_write_memory_32,
                  size == 4 ? 0 : size);
        if (!rv_link(R)) {
            return false;
        }
        R->start = base;
    }
    for (int kind = PK_LOGIC; kind <= PK_Z; kind++) {
        for (int size = 1; size <= 4; size *= 2) {
            mat_routine(kind, size);
        }
    }
    if (R->full) {
        return false;
    }
    *at = (uint8_t *)R->p;
    return true;
}

/* A link: the exit's two words a jump to a block's entry, a0 the exit's
 * jal after them either way (the record behind it: ST_BAIL) */
void jit_rv_link(uint32_t *site, const uint32_t *to)
{
    int32_t off = (int32_t)(addr32(to) - addr32(site + 1));
    if (off >= -(1 << 20) && off < (1 << 20)) {
        site[0] = RV_NOP;
        site[1] = rv_j(off, A0);
    } else {
        uint32_t d = addr32(to) - addr32(site);
        int32_t lo = (int32_t)(d << 20) >> 20;
        site[0] = ((d - (uint32_t)lo) & 0xFFFFF000u) | (uint32_t)T0 << 7 | 0x17;   /* auipc t0 */
        site[1] = rv_i(lo, T0, 0, A0, 0x67);                                     /* jalr a0, lo(t0) */
    }
}

/* A dropped block's entry: back to the dispatcher (the routines are within
 * a jal's reach of the whole arena) */
void jit_rv_kill(uint32_t *entry)
{
    *entry = rv_j((int32_t)(s_stub[ST_BAIL] - addr32(entry)), X0);
}

/* The flags in memory what the pending kind pk of size bytes makes of the
 * registers f (s, d, r) where Musashi's tests look (mat_routine()) */
static bool flags_match(int pk, int size, const uint32_t f[3])
{
    uint32_t s = f[0], d = f[1], r = f[2];
    int sh = nshift(size);
    if ((FLAG_Z == 0) != (r == 0)) {
        return false;
    }
    if (pk == PK_Z) {
        return true;
    }
    uint32_t n = r >> sh, v, c;
    if (pk == PK_LOGIC) {
        v = c = 0;
    } else if (pk == PK_SUB) {
        v = ((s ^ d) & (r ^ d)) >> sh;
        c = (uint32_t)(d < s) << 8;
    } else {
        v = ((s ^ r) & (d ^ r)) >> sh;
        c = (uint32_t)(r < s) << 8;
    }
    return !((FLAG_N ^ n) & 0x80) && !((FLAG_V ^ v) & 0x80) && !((FLAG_C ^ c) & 0x100);
}

#ifndef ESP_PLATFORM
unsigned long jit_resume_no[3];             /* (AMI_JITSTAT) not resumed: dropped, no instruction there, the flags */
#endif

const uint32_t *jit_rv_resume(const jit_block_t *b, uint32_t pc, const uint32_t f[3])
{
    uint32_t off = pc - b->pc;
    if (b->dead) {
#ifndef ESP_PLATFORM
        jit_resume_no[0]++;
#endif
        return NULL;
    }
    for (int j = 1; j < b->n; j++) {
        uint32_t w = b->res[j];
        if (JIT_RES_PC(w) != off) {
            continue;
        }
        int pk = (int)JIT_RES_PK(w), size = 1 << JIT_RES_PS(w);
        if (!JIT_RES_BODY(w) || (pk != PK_NONE && !flags_match(pk, size, f))) {
#ifndef ESP_PLATFORM
            jit_resume_no[2]++;
#endif
            return NULL;
        }
        jit_rs[JIT_RS_PC] = pc;
        jit_rs[JIT_RS_BODY] = addr32(b->code + JIT_RES_BODY(w));
        jit_rs[JIT_RS_S2] = (b->pc & ~3u) | 1;
        jit_rs[JIT_RS_S5] = JIT_RES_S5(w) == 0xFF ? 0 : b->pc + JIT_RES_S5(w);
        for (int i = 0; i < 3; i++) {
            jit_rs[JIT_RS_F + i] = f[i];
        }
        jit_rs[JIT_RS_WAIT] = b->wait;
#ifdef M68K_HOST_HOOK
        if (jit_hooks) {
            m68k_host_instruction(pc);      /* (as the code's hook before the opcode's fetch) */
        }
#endif
        return (const uint32_t *)(uintptr_t)s_stub[ST_RESUME];
    }
#ifndef ESP_PLATFORM
    jit_resume_no[1]++;
#endif
    return NULL;
}

#ifndef ESP_PLATFORM
#include <time.h>
double jit_t_decode, jit_t_emit, jit_t_all;
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#define TIMED(var, expr) do { double t0_ = now(); expr; var += now() - t0_; } while (0)
#else
#define TIMED(var, expr) expr
#endif

bool jit_translate(jit_block_t *b, uint32_t pc, uint8_t **at, uint8_t *end)
{
    int max = MAX_INSNS;
    mem_jit_env(&T.env);
    T.cpu = addr32(&m68ki_cpu);
    if (!fetch_offsets()) {
#ifndef ESP_PLATFORM
        fprintf(stderr, "jit: the fetch state out of s0's reach\n");
#endif
        return false;
    }
    for (int tries = 0; tries < 4; tries++) {
        TIMED(jit_t_decode, T.n = decode(pc, max));
        if (!T.n) {
            return false;
        }
        /* the handlers' entries, the code, then its exits (their numbers in
         * the code, their table after it at its size) */
        uint8_t *p = *at;
        T.fb = (jit_fb_t *)p;
        p += ((size_t)T.n * sizeof(jit_fb_t) + 3) & ~(size_t)3;
        T.res = (uint32_t *)p;
        p += (size_t)T.n * 4;
        T.exit = s_exits;
        T.max_exits = 2 * T.n + 8;
        if (p + 64 > end) {
            b->n = 0xFFFF;                  /* (no room) */
            return false;
        }
        memset(T.res, 0, (size_t)T.n * 4);
        T.nexits = T.nfb = 0;
        R->full = false;
        R->nlabels = R->nfixups = 0;
        R->start = R->p = (uint32_t *)p;
        R->end = (uint32_t *)end;
        T.b = b;
        bool emitted;
        TIMED(jit_t_emit, emitted = emit_block());
        if (!emitted) {
            if (s_cut > 0) {
                max = s_cut;
                continue;
            }
            if (R->full && R->p >= R->end) {
                b->n = 0xFFFF;
                return false;
            }
            max = T.n > 1 ? T.n / 2 : 1;    /* (a branch out of reach: fewer) */
            continue;
        }
        b->pc = pc;
        b->end = T.in[T.n - 1].next;
        b->code = R->start;
        b->entry[0] = R->start + R->label[T.entry_l[0]];
        b->entry[1] = T.entry_l[1] >= 0 ? R->start + R->label[T.entry_l[1]] : b->entry[0];
        b->fb = T.fb;
        b->res = T.res;
        b->wait = T.in[0].wait;
        b->n = (uint16_t)T.n;
        b->nexits = (uint16_t)T.nexits;
        b->nfb = (uint16_t)T.nfb;
        *at = (uint8_t *)R->p;
#ifndef ESP_PLATFORM
        for (int c = 0; c < JB_N; c++) {
            jit_bytes[c] += s_bt[c];
        }
        jit_bytes[JB_FBTAB] += (unsigned long)((uint8_t *)R->start - (uint8_t *)T.fb);
#endif
        return true;
    }
    return false;
}
#endif
