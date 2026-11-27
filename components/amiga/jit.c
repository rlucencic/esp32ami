/* The 68EC020's translated blocks: the cache, translation, invalidation and
 * the dispatcher (jit.h, docs/jit-design-2026-11-27.md). Stage 1: every
 * instruction a call of its handler.
 */
#include <string.h>
#include "amiga_internal.h"

#ifdef AMI_JIT
#include "jit.h"
#include "m68k.h"

#define HASH_SIZE       4096
#define MAX_INSNS       64
#define MAX_BYTES       256
#define ARENA_SIZE      (1024 * 1024)

uint8_t jit_code_pages[JIT_PAGES / 8];
volatile bool jit_stop;

static jit_block_t *s_hash[HASH_SIZE];
static jit_block_t **s_page;                /* per page: the blocks on it */
static uint8_t *s_arena;
static size_t s_used;
static bool s_running, s_flush_pending;

static inline uint32_t hash(uint32_t pc)
{
    return (pc >> 1) & (HASH_SIZE - 1);
}

static bool ready(void)
{
    if (!s_arena) {
        s_arena = ami_malloc(ARENA_SIZE);
        s_page = ami_malloc(JIT_PAGES * sizeof(*s_page));
        if (!s_arena || !s_page) {
            return false;
        }
        memset(s_page, 0, JIT_PAGES * sizeof(*s_page));
    }
    return true;
}

static void flush_now(void)
{
    memset(s_hash, 0, sizeof(s_hash));
    if (s_page) {
        memset(s_page, 0, JIT_PAGES * sizeof(*s_page));
    }
    memset(jit_code_pages, 0, sizeof(jit_code_pages));
    s_used = 0;
    s_flush_pending = false;
}

/* Everything goes (a new memory map, a reset); while a block runs, after it */
void jit_flush(void)
{
    if (s_running) {
        s_flush_pending = true;
        jit_stop = true;
    } else {
        flush_now();
    }
}

/* ---- Pages ---- */

static void page_add(jit_block_t *b, int i)
{
    uint32_t p = b->page[i];
    b->page_next[i] = s_page[p];
    s_page[p] = b;
    jit_code_pages[p >> 3] |= (uint8_t)(1u << (p & 7));
}

static void page_remove(jit_block_t *b, int i)
{
    uint32_t p = b->page[i];
    for (jit_block_t **l = &s_page[p]; *l; ) {
        jit_block_t *o = *l;
        int k = o->page[0] == p ? 0 : 1;    /* (its link for this page) */
        if (o == b) {
            *l = o->page_next[k];
            break;
        }
        l = &o->page_next[k];
    }
    if (!s_page[p]) {
        jit_code_pages[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
}

static void drop(jit_block_t *b)
{
    if (b->dead) {
        return;
    }
    b->dead = true;
    for (jit_block_t **l = &s_hash[hash(b->pc)]; *l; l = &(*l)->hash_next) {
        if (*l == b) {
            *l = b->hash_next;
            break;
        }
    }
    page_remove(b, 0);
    if (b->page[1] != b->page[0]) {
        page_remove(b, 1);
    }
    jit_stop = true;                        /* (the one running may be it) */
}

/* The blocks with code in [lo, hi) go */
void jit_written(uint32_t lo, uint32_t hi)
{
    if (!s_page || hi <= lo) {
        return;
    }
    lo &= 0xFFFFFF;
    hi = hi - lo > 0x1000000 ? lo + 0x1000000 : hi;
    for (uint32_t p = lo >> JIT_PAGE_SHIFT; p <= ((hi - 1) & 0xFFFFFF) >> JIT_PAGE_SHIFT && p < JIT_PAGES; p++) {
        if (!(jit_code_pages[p >> 3] & (1u << (p & 7)))) {
            continue;
        }
        for (jit_block_t *b = s_page[p]; b; ) {
            jit_block_t *next = b->page_next[b->page[0] == p ? 0 : 1];
            if (b->pc < hi && lo < b->end) {
                drop(b);
            }
            b = next;
        }
    }
}

void jit_cpu_wrote(uint32_t a, int bytes)
{
    jit_written(a & 0xFFFFFF, (a & 0xFFFFFF) + (uint32_t)bytes);
}

/* ---- Translation ---- */

/* An instruction after which the PC is always elsewhere: the block ends */
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
    case 0x4E70:                            /* RESET */
    case 0x4E72:                            /* STOP */
    case 0x4E73:                            /* RTE */
    case 0x4E74:                            /* RTD */
    case 0x4E75:                            /* RTS */
    case 0x4E77:                            /* RTR */
    case 0x4AFC:                            /* ILLEGAL */
        return true;
    }
    return false;
}

static jit_block_t *translate(uint32_t pc)
{
    uint8_t wait;
    if (!ready() || !mem_code_ptr(pc, &wait)) {
        return NULL;
    }
    if (s_used + sizeof(jit_block_t) + MAX_INSNS * sizeof(jit_insn_t) + 16 > ARENA_SIZE) {
        flush_now();
    }
    jit_block_t *b = (jit_block_t *)(s_arena + s_used);
    memset(b, 0, sizeof(*b));
    b->pc = pc;
    void (**handlers)(void) = *m68k_loop.handlers;
    const uint8_t *cycles = *m68k_loop.cycles;
    uint32_t a = pc;
    char text[100];
    while (b->n < MAX_INSNS) {
        uint8_t w;
        const uint16_t *host = mem_code_ptr(a, &w);
        if (!host) {
            break;
        }
        unsigned len = m68k_disassemble(text, a, M68K_CPU_TYPE_68EC020);
        uint32_t next = a + len;
        if (len < 2 || (next - pc) > MAX_BYTES || ((next - 1) >> 16) != (pc >> 16)) {
            break;                          /* (never across a bank) */
        }
        jit_insn_t *u = &b->insn[b->n++];
        u->pc = a;
        u->next = next;
        u->host = host;
        u->op = *host;
        u->handler = handlers[u->op];
        u->cycles = cycles[u->op];
        u->wait = w;
        a = next;
        if (ends_block(u->op)) {
            break;
        }
    }
    if (!b->n) {
        return NULL;
    }
    b->end = a;
    b->page[0] = pc >> JIT_PAGE_SHIFT;
    b->page[1] = (a - 1) >> JIT_PAGE_SHIFT;
    s_used += (sizeof(jit_block_t) + b->n * sizeof(jit_insn_t) + 15) & ~(size_t)15;
    uint32_t h = hash(pc);
    b->hash_next = s_hash[h];
    s_hash[h] = b;
    page_add(b, 0);
    if (b->page[1] != b->page[0]) {
        page_add(b, 1);
    }
    return b;
}

static inline jit_block_t *lookup(uint32_t pc)
{
    for (jit_block_t *b = s_hash[hash(pc)]; b; b = b->hash_next) {
        if (b->pc == pc) {
            return b;
        }
    }
    return NULL;
}

/* ---- The dispatcher: the 68EC020's slice from where the PC is ---- */

int jit_run(void)
{
    if (ami.chip_mask != 0x1FFFFF) {
        /* (the A1200 has 2 MB of chip RAM, no mirror: pages are addresses) */
        do {
            if (*m68k_loop.pc & 1) {
                return 1;
            }
            mem_interp_one();
        } while (*m68k_loop.remaining > 0 && !*m68k_loop.tracing);
        return 1;
    }
    unsigned int *pcr = m68k_loop.pc;
    int *remaining = m68k_loop.remaining;
    const unsigned int *tracing = m68k_loop.tracing;
    do {
        uint32_t a = *pcr;
        if (a & 1) {
            return 1;                       /* (Musashi's: the address error) */
        }
        jit_block_t *b = a <= 0xFFFFFF ? lookup(a) : NULL;
        if (!b && a <= 0xFFFFFF) {
            b = translate(a);
        }
        if (!b) {
            mem_interp_one();
        } else {
            s_running = true;
            jit_stop = false;
            jit_exec_block(b);
            s_running = false;
            if (s_flush_pending) {
                flush_now();
            }
        }
    } while (*remaining > 0 && !*tracing);
    return 1;
}
#endif
