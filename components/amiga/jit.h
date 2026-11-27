/* The 68EC020's translated blocks (TODO 19, docs/jit-design-2026-11-27.md).
 *
 * A block is a run of instructions from a PC, translated into RISC-V
 * (jit_rv.c); the dispatcher (jit_run(), from m68k_host_run()) finds or
 * translates the block at the PC and runs it: the board calls its code,
 * the host runs the same code through host/rvsim.c. Instructions with no
 * code of their own call their Musashi handler (jit_fallback()). A block
 * returns the number of its exit; the exit says what state to leave
 * (jit_exit_t). Writes into translated code drop the blocks they touch
 * (jit_written(), jit_check_write()); a new memory map or a reset drops
 * them all (jit_flush()).
 */
#ifndef JIT_H
#define JIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JIT_PAGE_SHIFT      8           /* 256-byte pages of code */
#define JIT_PAGES           (1u << (24 - JIT_PAGE_SHIFT))

/* An instruction left to its handler (the rest from its opcode and the
 * maps: jit_fallback()) */
typedef struct {
    uint32_t pc;                        /* where it is */
    uint16_t op;
    uint8_t len;                        /* its bytes: the PC after it when it doesn't jump */
    uint8_t wait;                       /* its bank's (s_m.wait): the fetch's */
} jit_fb_t;

/* A way out of a block: the state the interpreter would have there. Each
 * exit ends in a jal a0 to the shared exit with its record after it: the
 * block returns the record's address. */
enum {
    JIT_EXIT_PC,                        /* PC = pc (and the rest below): the slice over, or jit_stop */
    JIT_EXIT_DYN,                       /* the code set the PC; the rest below */
    JIT_EXIT_FB,                        /* after a handler: as it left everything */
    JIT_EXIT_LINK,                      /* as JIT_EXIT_PC, the flow going on to pc: the two
                                         * words 12 bytes before the record a jump to pc's
                                         * block once linked (jit_rv_link()) */
};
typedef struct {
    uint32_t pc;                        /* (JIT_EXIT_PC) */
    uint32_t ppc;                       /* the last instruction's address, */
    uint32_t fe;                        /* the end of its last word fetched, */
    uint16_t ir;                        /* its opcode */
    uint8_t kind;
} jit_exit_t;

typedef struct jit_block {
    uint32_t pc, end;                   /* the bytes it was made from: [pc, end) */
    struct jit_block *hash_next;
    struct jit_block *page_next[2];     /* on the lists of its first and last page */
    uint32_t page[2];
    const uint32_t *code;               /* the dispatcher's entry */
    uint32_t *entry[2];                 /* a linked exit's: reached by a jump, in sequence
                                         * (the same unless pc & 2: the opcode's fetch) */
    jit_fb_t *fb;
    uint16_t n, nexits, nfb;            /* instructions, exits, handlers */
    bool dead;
} jit_block_t;

/* What the code reaches directly (mem.c's) */
typedef struct {
    uint32_t *ic_next, *ic_cacr, *ic_tag;
    const void *rmap, *wmap;
    const uint8_t *wait;
} jit_env_t;

/* 256-byte pages with translated code (bit set): a write there is looked at */
extern uint8_t jit_code_pages[JIT_PAGES / 8];
/* the block running should stop after this instruction (its code written,
 * the memory map changed) */
extern volatile bool jit_stop;
extern uint32_t jit_ctx[4];

int jit_run(void);
void jit_flush(void);
void jit_release(void);                         /* the arena given back (the machine stopped) */
void jit_written(uint32_t lo, uint32_t hi);   /* [lo, hi) written by DMA or a helper */
void jit_cpu_wrote(uint32_t a, int bytes);

#ifdef AMI_JIT
#define JIT_CHECK_WRITE(a, n)   jit_check_write((a), (n))
#define JIT_WRITTEN(lo, hi)     jit_written((lo), (hi))
#define JIT_FLUSH()             jit_flush()
#else
#define JIT_CHECK_WRITE(a, n)   ((void)0)
#define JIT_WRITTEN(lo, hi)     ((void)0)
#define JIT_FLUSH()             ((void)0)
#endif

/* the CPU's write of bytes at a: into translated code? */
static inline void jit_check_write(uint32_t a, int bytes)
{
    uint32_t b = a + (uint32_t)bytes - 1;
    if (__builtin_expect((jit_code_pages[(a & 0xFFFFFF) >> 11] | jit_code_pages[(b & 0xFFFFFF) >> 11]) != 0, 0)) {
        jit_cpu_wrote(a, bytes);
    }
}

/* mem.c's side */
int jit_fallback(const jit_fb_t *f);
void mem_fetch_state(uint32_t fe);
void mem_fetch_reset(void);
void mem_jit_env(jit_env_t *e);
void mem_interp_one(void);
const uint16_t *mem_code_ptr(uint32_t a, uint8_t *wait);

/* jit_rv.c: the block at pc translated into code at *at (up to end; moved
 * on past it), its handlers' entries first, its exits' records in the
 * code. False: none (no room: flush and try again; or nothing to
 * translate) */
bool jit_translate(jit_block_t *b, uint32_t pc, uint8_t **at, uint8_t *end);
/* the routines the blocks share (prologue, exit, memory accesses), once in
 * the arena's middle */
bool jit_rv_stubs(uint8_t **at, uint8_t *end);
/* a link exit's two words (its record less 12 bytes) made a jump to a
 * block's entry; a dropped block's entry made a jump back to the
 * dispatcher, as the exit that came there (the caller syncs the caches) */
void jit_rv_link(uint32_t *site, const uint32_t *to);
void jit_rv_kill(uint32_t *entry);

#ifndef ESP_PLATFORM
/* host/rvsim.c */
uint32_t rvsim_call(uint32_t entry);
void rvsim_arena(const void *base, size_t size);
int jit_native_at(uint32_t pc);
#endif

#endif
