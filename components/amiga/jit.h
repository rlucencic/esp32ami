/* The 68EC020's translated blocks (TODO 19, docs/jit-design-2026-11-27.md).
 *
 * A block is a run of instructions from a PC; the dispatcher (jit_run(),
 * from m68k_host_run()) finds or translates the block at the PC and runs
 * it. Stage 1: each instruction is a call of its Musashi handler, with the
 * fetch timing and the state the interpreter's loop gives it (mem.c's
 * jit_exec_block()). Writes into translated code drop the blocks they
 * touch (jit_written(), jit_check_write()); a new memory map or a reset
 * drops them all (jit_flush()).
 */
#ifndef JIT_H
#define JIT_H

#include <stdbool.h>
#include <stdint.h>

#define JIT_PAGE_SHIFT      8           /* 256-byte pages of code */
#define JIT_PAGES           (1u << (24 - JIT_PAGE_SHIFT))

/* An instruction: the call of its handler */
typedef struct {
    uint32_t pc;                        /* where it is */
    uint32_t next;                      /* the PC after it when it doesn't jump */
    const uint16_t *host;               /* its opcode in host memory */
    void (*handler)(void);
    uint16_t op;
    uint8_t cycles;                     /* Musashi's table's (the handler may add) */
    uint8_t wait;                       /* its bank's (s_m.wait): the fetch's */
} jit_insn_t;

typedef struct jit_block {
    uint32_t pc, end;                   /* the bytes it was made from: [pc, end) */
    struct jit_block *hash_next;
    struct jit_block *page_next[2];     /* on the lists of its first and last page */
    uint32_t page[2];
    uint16_t n;
    bool dead;
    jit_insn_t insn[];
} jit_block_t;

/* 256-byte pages with translated code (bit set): a write there is looked at */
extern uint8_t jit_code_pages[JIT_PAGES / 8];
/* the block running should stop after this instruction (its code written,
 * the memory map changed) */
extern volatile bool jit_stop;

int jit_run(void);
void jit_flush(void);
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

/* mem.c's side: run a block as the interpreter's loop would, one
 * instruction the interpreter's way, and what translation needs to know */
void jit_exec_block(const jit_block_t *b);
void mem_interp_one(void);
const uint16_t *mem_code_ptr(uint32_t a, uint8_t *wait);

#endif
