/* The 68EC020's translated blocks: the cache, invalidation and the
 * dispatcher (jit.h, docs/jit-design-2026-11-27.md). The code itself:
 * jit_rv.c.
 */
#include <string.h>
#include "amiga_internal.h"

#ifdef AMI_JIT
#include <stdlib.h>
#include "jit.h"
#include "m68k.h"
#ifdef ESP_PLATFORM
#include <stdio.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif

#define HASH_SIZE       4096
/* The arena: the shared routines (jit_rv.c) in its middle, in STUB_ROOM, so
 * a jump (1 MB either way) reaches all of them from anywhere in it: 2 MB
 * less that room at most. Smaller ones while PSRAM is short (ready()). */
#define STUB_ROOM       (4 * 1024)          /* (they take 1408 bytes) */
#define ARENA_MAX       (2 * 1024 * 1024 - STUB_ROOM)
#define ARENA_MIN       (256 * 1024)
#define JIT_RESERVE     (512 * 1024)        /* PSRAM left to the rest (uploads, RP9s, the setup screen) */
#define BANKS           256                 /* 64 KB banks */
#define BANK_PAGES      (1u << (16 - JIT_PAGE_SHIFT))

uint8_t jit_code_pages[JIT_PAGES / 8] __attribute__((aligned(4)));
volatile bool jit_stop AMI_SDATA;          /* (beside Musashi's state: the code reaches it) */
uint32_t jit_ctx[4] AMI_SDATA;              /* (the bases the code's memory routines take: jit_rv.c) */

static jit_block_t *s_hash[HASH_SIZE];
/* Per 64 KB bank, the block lists of its pages, made in the arena as a
 * block first lands there (all 16 MB's lists took 256 KB): the directory
 * in the arena's first bytes */
static jit_block_t ***s_dir;
static uint8_t *s_arena;
static size_t s_used, s_start;              /* (s_start: after the directory) */
static size_t s_mid, s_mid_end;             /* the shared routines: [s_mid, s_mid_end) */
#ifndef ESP_PLATFORM
static size_t s_size;
/* (AMI_JITSTAT) the arena's size, the shared routines' bytes in *routines */
size_t jit_arena_size(size_t *routines)
{
    *routines = s_mid_end - s_mid;
    return s_size;
}
#endif
static unsigned s_retry;                    /* no arena: translations until the next try */
static bool s_running, s_flush_pending;
/* Code is translated once it has run HOT times (by its PC's hash: the
 * interpreter runs it until then). Most code runs a few times (loaders,
 * decrunchers, setup): translated, it filled the arena over and over
 * (Virtual Karting 2's first 4000 frames: 305000 blocks, 560 flushes). */
uint8_t jit_hot = 64;                       /* (host/jit_test.c: 1) */
static uint8_t s_heat[HASH_SIZE];
#ifndef ESP_PLATFORM
struct { unsigned long translations, flushes, full, drops, blocks_run, code_bytes, insns, distinct, exits, fbs, rv_words; } jit_stats;
uint32_t jit_fb_count[65536];               /* (the handlers called, by opcode: mem.c) */
#define STAT(x) (jit_stats.x)
#else
static struct { unsigned long translations, flushes, full, drops, blocks_run, code_bytes, insns; } jit_stats;
#define STAT(x) (jit_stats.x)
static uint64_t s_us_translate, s_us_sync;

static size_t s_size;

/* (the console's J) */
void jit_report(void)
{
    printf("JIT: %lu translations (%lu instructions, %lu bytes of code), %lu flushes (%lu full), %lu drops, "
           "%lu blocks run; %llu us translating, %llu us syncing; the arena %u KB, %u KB of PSRAM free\n",
           jit_stats.translations, jit_stats.insns, jit_stats.code_bytes, jit_stats.flushes, jit_stats.full,
           jit_stats.drops, jit_stats.blocks_run, (unsigned long long)s_us_translate, (unsigned long long)s_us_sync,
           (unsigned)(s_size / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
#endif

static inline uint32_t hash(uint32_t pc)
{
    return (pc >> 1) & (HASH_SIZE - 1);
}

/* Code written through the data cache, run through the instruction cache */
static void sync_code(void *p, size_t n)
{
#ifdef ESP_PLATFORM
    int64_t t0 = esp_timer_get_time();
    uintptr_t lo = (uintptr_t)p & ~(uintptr_t)127, hi = ((uintptr_t)p + n + 127) & ~(uintptr_t)127;
    esp_cache_msync((void *)lo, hi - lo, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_cache_msync((void *)lo, hi - lo, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    s_us_sync += (uint64_t)(esp_timer_get_time() - t0);
#else
    (void)p;
    (void)n;
#endif
}

/* The largest arena that leaves the rest JIT_RESERVE of PSRAM: 2 MB, else
 * 1 MB, 512 KB, 256 KB (the host: 2 MB, or AMI_JITARENA's KB) */
static uint8_t *arena_alloc(size_t *size)
{
#ifdef ESP_PLATFORM
    for (size_t n = ARENA_MAX; n >= ARENA_MIN; n = n == ARENA_MAX ? 1024 * 1024 : n / 2) {
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= n + JIT_RESERVE &&
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= n) {
            uint8_t *a = heap_caps_aligned_alloc(128, n, MALLOC_CAP_SPIRAM);
            if (a) {
                *size = n;
                return a;
            }
        }
    }
    return NULL;
#else
    const char *kb = getenv("AMI_JITARENA");
    size_t n = kb ? (size_t)strtoul(kb, NULL, 0) * 1024 : ARENA_MAX;
    n = n > ARENA_MAX ? ARENA_MAX : n < ARENA_MIN ? ARENA_MIN : n;
    uint8_t *a = malloc(n);
    if (a) {
        rvsim_arena(a, n);
        *size = n;
    }
    return a;
#endif
}

static void arena_free(void)
{
    free(s_arena);
    s_arena = NULL;
    s_dir = NULL;
    s_size = 0;
}

static bool ready(void)
{
    if (!s_arena) {
        if (s_retry) {
            s_retry--;
            return false;
        }
        s_arena = arena_alloc(&s_size);
        if (!s_arena) {
            s_retry = 4096;
            return false;
        }
        s_dir = (jit_block_t ***)s_arena;
        memset(s_dir, 0, BANKS * sizeof(*s_dir));
        s_start = (BANKS * sizeof(*s_dir) + 15) & ~(size_t)15;
        s_mid = ((s_size - STUB_ROOM) / 2) & ~(size_t)127;
        uint8_t *at = s_arena + s_mid;
        if (!jit_rv_stubs(&at, s_arena + s_mid + STUB_ROOM)) {
#ifndef ESP_PLATFORM
            fprintf(stderr, "jit: the shared routines didn't fit\n");
#endif
            arena_free();
            s_retry = ~0u;
            return false;
        }
        s_mid_end = (size_t)(((uintptr_t)at - (uintptr_t)s_arena + 127) & ~(uintptr_t)127);
        s_used = s_start;
        sync_code(s_arena + s_mid, s_mid_end - s_mid);
    }
    return true;
}


static void flush_now(void)
{
    STAT(flushes)++;
    memset(s_hash, 0, sizeof(s_hash));
    if (s_dir) {
        memset(s_dir, 0, BANKS * sizeof(*s_dir));
    }
    memset(jit_code_pages, 0, sizeof(jit_code_pages));
    memset(s_heat, 0, sizeof(s_heat));
    s_used = s_start;
    s_flush_pending = false;
}

/* The arena given back (a disk's buffer wants the room; the machine
 * stopped between frames): taken again, as large as there's room for, at
 * the next translation */
void jit_release(void)
{
    if (s_running || !s_arena) {
        return;
    }
    flush_now();
    arena_free();
    s_retry = 0;
}

/* Everything goes (a new memory map, a reset); while a block runs, after it */
void jit_flush(void)
{
    s_retry = 0;                            /* (no arena: try again) */
    if (s_running) {
        s_flush_pending = true;
        jit_stop = true;
    } else {
        flush_now();
    }
}

/* ---- Pages ---- */

/* page p's list (its bank's table made already) */
static inline jit_block_t **page_list(uint32_t p)
{
    return &s_dir[p / BANK_PAGES][p % BANK_PAGES];
}

/* the tables of the banks page[0] and page[1] are in, made at *at (up to
 * end) where missing; false: no room */
static bool page_tables(const uint32_t page[2], uint8_t **at, uint8_t *end)
{
    for (int i = 0; i < 2; i++) {
        uint32_t k = page[i] / BANK_PAGES;
        if (!s_dir[k]) {
            size_t n = BANK_PAGES * sizeof(jit_block_t *);
            if ((size_t)(end - *at) < n) {
                return false;
            }
            s_dir[k] = (jit_block_t **)*at;
            memset(s_dir[k], 0, n);
            *at += n;
        }
    }
    return true;
}

static void page_add(jit_block_t *b, int i)
{
    uint32_t p = b->page[i];
    jit_block_t **l = page_list(p);
    b->page_next[i] = *l;
    *l = b;
    jit_code_pages[p >> 3] |= (uint8_t)(1u << (p & 7));
}

static void page_remove(jit_block_t *b, int i)
{
    uint32_t p = b->page[i];
    jit_block_t **head = page_list(p);
    for (jit_block_t **l = head; *l; ) {
        jit_block_t *o = *l;
        int k = o->page[0] == p ? 0 : 1;    /* (its link for this page) */
        if (o == b) {
            *l = o->page_next[k];
            break;
        }
        l = &o->page_next[k];
    }
    if (!*head) {
        jit_code_pages[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
}

static void drop(jit_block_t *b)
{
    if (b->dead) {
        return;
    }
    b->dead = true;
    STAT(drops)++;
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
    if (!s_dir || hi <= lo) {
        return;
    }
    lo &= 0xFFFFFF;
    hi = hi - lo > 0x1000000 ? lo + 0x1000000 : hi;
    uint32_t last = ((hi - 1) & 0xFFFFFF) >> JIT_PAGE_SHIFT;
    for (uint32_t p = lo >> JIT_PAGE_SHIFT; p <= last && p < JIT_PAGES; p++) {
        uint32_t w;
        if (!(p & 31) && (memcpy(&w, &jit_code_pages[p >> 3], 4), !w)) {
            p += 31;                        /* (32 pages without code: a blit's range in a few steps) */
            continue;
        }
        if (!(jit_code_pages[p >> 3] & (1u << (p & 7)))) {
            continue;
        }
        for (jit_block_t *b = *page_list(p); b; ) {
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

/* ---- Translation and the dispatcher ---- */

static jit_block_t *translate_(uint32_t pc);

static jit_block_t *translate(uint32_t pc)
{
#ifdef ESP_PLATFORM
    int64_t t0 = esp_timer_get_time();
    jit_block_t *b = translate_(pc);
    s_us_translate += (uint64_t)(esp_timer_get_time() - t0);
    return b;
#else
    return translate_(pc);
#endif
}

static jit_block_t *translate_(uint32_t pc)
{
    if (!ready()) {
        return NULL;
    }
    for (int tries = 0; tries < 3; tries++) {
        if (s_used >= s_mid && s_used < s_mid_end) {
            s_used = s_mid_end;             /* (the first half full to its end: never over the routines) */
        }
        uint8_t *at = s_arena + s_used, *end = s_arena + (s_used < s_mid ? s_mid : s_size);
        if ((size_t)(end - at) < sizeof(jit_block_t) + 64) {
            if (s_used < s_mid) {
                s_used = s_mid_end;         /* (on past the shared routines) */
            } else {
                flush_now();
            }
            continue;
        }
        jit_block_t *b = (jit_block_t *)at;
        memset(b, 0, sizeof(*b));
        at += (sizeof(*b) + 15) & ~(size_t)15;
        uint8_t *code = at;
        if (!jit_translate(b, pc, &at, end)) {
            if (b->n == 0xFFFF) {
                if (s_used < s_mid) {
                    s_used = s_mid_end;     /* (no room before the routines: after them) */
                } else {
                    STAT(full)++;
                    flush_now();            /* (no room: once more in an empty arena) */
                }
                continue;
            }
            return NULL;
        }
        if (b->end - 1 > 0xFFFFFF) {
            return NULL;                    /* (round the top of the address space: left to the interpreter) */
        }
        uint8_t *code_end = at;
        b->page[0] = pc >> JIT_PAGE_SHIFT;
        b->page[1] = (b->end - 1) >> JIT_PAGE_SHIFT;
        at = (uint8_t *)(((uintptr_t)at + 15) & ~(uintptr_t)15);
        if (!page_tables(b->page, &at, end)) {
            if (s_used < s_mid) {
                s_used = s_mid_end;         /* (the bank's table: after the routines) */
            } else {
                STAT(full)++;
                flush_now();
            }
            continue;
        }
        s_used = (size_t)(((uintptr_t)at - (uintptr_t)s_arena + 15) & ~(uintptr_t)15);
        STAT(translations)++;
        STAT(code_bytes) += (unsigned long)(code_end - code);
        STAT(insns) += b->n;
#ifndef ESP_PLATFORM
        jit_stats.exits += b->nexits;
        jit_stats.fbs += b->nfb;
        jit_stats.rv_words += (unsigned long)((const uint8_t *)b->exit - (const uint8_t *)b->code) / 4;
#endif
#ifndef ESP_PLATFORM
        {
            static uint8_t *seen;
            if (!seen) {
                seen = calloc(1 << 20, 1);
            }
            uint32_t k = (pc & 0xFFFFFF) >> 1;
            if (!(seen[k >> 3] & (1 << (k & 7)))) {
                seen[k >> 3] |= (uint8_t)(1 << (k & 7));
                jit_stats.distinct++;
            }
        }
        static const char *dump = (const char *)1;
        if (dump == (const char *)1) {
            dump = getenv("AMI_JITDUMP");
        }
        if (dump && strtoul(dump, NULL, 16) == pc) {
            FILE *f = fopen("jitdump.bin", "wb");
            fwrite(b->code, 1, (size_t)(code_end - (uint8_t *)b->code), f);
            fclose(f);
        }
#endif
        sync_code(code, (size_t)(code_end - code));
        b->pc = pc;
        uint32_t h = hash(pc);
        b->hash_next = s_hash[h];
        s_hash[h] = b;
        page_add(b, 0);
        if (b->page[1] != b->page[0]) {
            page_add(b, 1);
        }
        return b;
    }
    return NULL;
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

#ifndef ESP_PLATFORM
/* (host/jit_test.c) 1: the block at pc runs its first instruction itself */
int jit_native_at(uint32_t pc)
{
    jit_block_t *b = lookup(pc);
    return b && !(b->nfb && b->fb[0].pc == pc);
}
#endif

/* The state at a block's exit k */
static inline void exit_state(const jit_block_t *b, uint32_t k)
{
    const jit_exit_t *e = &b->exit[k];
    if (e->kind == JIT_EXIT_FB) {
        return;
    }
    if (e->kind == JIT_EXIT_PC) {
        *m68k_loop.pc = e->pc;
    }
    *m68k_loop.ppc = e->ppc;
    *m68k_loop.ir = e->ir;
    mem_fetch_state(e->fe);
}

static inline uint32_t run_block(const jit_block_t *b)
{
#ifdef ESP_PLATFORM
    return ((uint32_t (*)(void))b->code)();
#else
    return rvsim_call((uint32_t)(uintptr_t)b->code);
#endif
}

/* ---- The 68EC020's slice from where the PC is ---- */

int AMI_HOT jit_run(void)
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
        if (!b && a <= 0xFFFFFF && ++s_heat[hash(a)] >= jit_hot) {
            s_heat[hash(a)] = 0;
            b = translate(a);
        }
        if (!b) {
            mem_interp_one();
            continue;
        }
        s_running = true;
        jit_stop = false;
        STAT(blocks_run)++;
        uint32_t k = run_block(b);
        exit_state(b, k);
        s_running = false;
        if (s_flush_pending) {
            flush_now();
            mem_fetch_reset();              /* (the exit's state is the old map's) */
        }
    } while (*remaining > 0 && !*tracing);
    return 1;
}
#endif
