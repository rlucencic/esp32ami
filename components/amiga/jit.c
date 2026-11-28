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
uint32_t jit_rf[4] AMI_SDATA;
uint32_t jit_rs[JIT_RS_N] AMI_SDATA;

static jit_block_t *s_hash[HASH_SIZE];
/* Where slices ended inside blocks (an exit's PC and the pending flags'
 * registers there), by the PC's hash: the next slice from there resumes
 * that block's code (a slice ends anywhere: a block made at each such PC
 * copied the code after it, Leading Lap's working set 2.6 MB of 3.2) */
#define RES_SLOTS       256
typedef struct {
    uint32_t pc;
    const jit_block_t *b;
    uint32_t f[3];
    uint32_t j;                             /* (its instruction, as the exit's record says) */
} jit_res_t;
static jit_res_t *s_res;                    /* (in the arena, after the directory) */
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
/* Blocks dropped while a block ran: their entries made dead (kill()) once
 * it is back (every drop is a write's, and the write's instruction stops
 * the block: no link reaches them meanwhile) */
#define DEAD_MAX        64
static jit_block_t *s_dead[DEAD_MAX];
static int s_ndead;
/* Code is translated once it has run HOT times (by its PC's hash: the
 * interpreter runs it until then). Most code runs a few times (loaders,
 * decrunchers, setup): translated, it filled the arena over and over
 * (Virtual Karting 2's first 4000 frames: 305000 blocks, 560 flushes). */
uint8_t jit_hot = 64;                       /* (host/jit_test.c: 1) */
static uint8_t s_heat[HASH_SIZE];
/* Only code reached by a jump heats (a branch's target, an exception's, an
 * exit's to a PC the code doesn't run on to; a link's or computed exit's):
 * a slice starts anywhere, and the interpreter running on from there made
 * every instruction of a loop hot at once after a flush, each a block's
 * start (Leading Lap: 1600 of its 3450 blocks) */
static const uint32_t *s_ic_next;           /* (mem.c's: the next word in sequence) */
/* ... or code after code with no block of its own (an instruction that
 * can't be translated: one across a 64 KB bank; a block's end after a
 * handler): s_failed marks PCs translate() failed at (by their hash), and
 * s_cont says the PC follows such code */
static uint8_t s_failed[HASH_SIZE / 8];
static bool s_cont;
#ifndef ESP_PLATFORM
bool jit_heat_all;                          /* (host/jit_test.c: every case translated at once) */
#endif
#ifndef ESP_PLATFORM
struct { unsigned long translations, flushes, full, drops, blocks_run, code_bytes, insns, distinct, exits, fbs, rv_words, links, kills, fills, resumes; } jit_stats;
uint32_t jit_fb_count[65536];               /* (the handlers called, by opcode: mem.c) */
uint32_t jit_interp_count[65536];           /* (the instructions run outside blocks, by opcode) */
unsigned long jit_interp_total;             /* (AMI_FRAMESTAT: all of them) */
/* (AMI_JITSTAT) lookups, their chain steps; the exits that came to the
 * dispatcher by kind (JIT_EXIT_*), with the slice going on and over */
unsigned long jit_lookups, jit_probes, jit_exits[4][2];
/* (AMI_JITSTAT) the computed exits with the slice going on, by RTS, JSR,
 * JMP: their number, the first at their exit, the target the last one
 * (of 1, 2, 4 kept per exit, the latest first), the last one and a block
 * there */
unsigned long jit_dyn[3][6];
/* (AMI_JITSTAT) the writes looked at, by the CPU (its 2 KB had code) and by
 * DMA and helpers: their number, those with code in their 256-byte page,
 * those over a word of code there (the page's blocks looked at), the blocks
 * looked at, the blocks dropped; the CPU's writes over code by the writing
 * instruction, the dropped blocks by their PC; the banks' tables made */
unsigned long jit_wr[2][5];
unsigned long jit_bank_tables;
/* (AMI_JITSTAT) after a link exit back at the dispatcher: a block there,
 * none yet (not hot), none made (translate() failed), one made now; the
 * PCs translate() failed at */
unsigned long jit_after_link[4];
static int s_wr_cpu;
#define PC_SLOTS        4096
typedef struct { uint32_t pc; unsigned long n, m; } jit_pc_count_t;
jit_pc_count_t jit_wr_pcs[PC_SLOTS], jit_drop_pcs[PC_SLOTS], jit_untrans_pcs[PC_SLOTS], jit_noslot_pcs[PC_SLOTS];
static uint32_t s_last_pc;                  /* (the last exit's PC) */

static jit_pc_count_t *pc_count(jit_pc_count_t *t, uint32_t pc)
{
    uint32_t h = (pc >> 1) & (PC_SLOTS - 1);
    for (int i = 0; i < PC_SLOTS; i++, h = (h + 1) & (PC_SLOTS - 1)) {
        if (t[h].pc == pc + 1 || !t[h].pc) {
            t[h].pc = pc + 1;
            return &t[h];
        }
    }
    static jit_pc_count_t spare;
    return &spare;
}
/* (AMI_JITSTAT) the working set: each PC's last block's bytes in the arena
 * (its tables apart) and the 68k code under it, counted from the stats'
 * start: what an arena would hold to make each block once */
static uint16_t *s_ws_bytes;               /* by PC / 2 */
static uint8_t *s_ws_cover;                 /* the 68k words under them, a bit each */
static uint32_t *s_ws_end;                  /* by PC / 2: its block's end */
static uint8_t *s_ws_why;                   /* by PC / 2: how the dispatcher came to it (s_why) */
/* (AMI_JITSTAT) how the dispatcher came to a PC it made a block at: a
 * slice's first, after a link exit, a computed exit, an instruction
 * interpreted, a handler's or a write's exit */
static int s_why;
unsigned long jit_made_by[5];
/* (AMI_JITSTAT) a slice's first PC: how the slice before ended (a block's
 * exit by its kind, an instruction interpreted, an odd PC) against what the
 * PC had (a block, resumed, a slot not resumed, no slot at the PC, no block
 * or slot but inside the last block's bytes) */
static int s_last_end = 5;
unsigned long jit_first[6][5];

static void ws_note(uint32_t pc, uint32_t end, unsigned long bytes)
{
    if (!s_ws_bytes) {
        s_ws_bytes = calloc(1 << 23, 2);
        s_ws_end = calloc(1 << 23, 4);
        s_ws_cover = calloc(1 << 20, 1);
        s_ws_why = calloc(1 << 23, 1);
    }
    uint32_t k = (pc & 0xFFFFFF) >> 1;
    s_ws_bytes[k] = (uint16_t)(bytes > 0xFFFF ? 0xFFFF : bytes);
    s_ws_end[k] = end;
    s_ws_why[k] = (uint8_t)s_why;
    jit_made_by[s_why]++;
    for (uint32_t a = pc; a < end && a <= 0xFFFFFF; a += 2) {
        s_ws_cover[a >> 4] |= (uint8_t)(1u << ((a >> 1) & 7));
    }
}

/* (host_main.c) the blocks, their bytes, their 68k bytes and the distinct
 * 68k bytes under them; then the blocks and bytes by s_why; clears it */
void jit_ws(unsigned long r[14], bool clear)
{
    memset(r, 0, 14 * sizeof(*r));
    if (!s_ws_bytes) {
        if (clear) {
            memset(jit_made_by, 0, sizeof(jit_made_by));
        }
        return;
    }
    for (uint32_t k = 0; k < (1u << 23); k++) {
        if (s_ws_bytes[k]) {
            r[0]++;
            r[1] += s_ws_bytes[k];
            r[2] += s_ws_end[k] - (k << 1);
            r[4 + 2 * s_ws_why[k]]++;
            r[5 + 2 * s_ws_why[k]] += s_ws_bytes[k];
        }
    }
    for (uint32_t i = 0; i < (1u << 20); i++) {
        r[3] += 2 * (unsigned long)__builtin_popcount(s_ws_cover[i]);
    }
    if (clear) {
        memset(s_ws_bytes, 0, (size_t)2 << 23);
        memset(s_ws_end, 0, (size_t)4 << 23);
        memset(s_ws_cover, 0, 1 << 20);
        memset(s_ws_why, 0, 1 << 23);
        memset(jit_made_by, 0, sizeof(jit_made_by));
    }
}

#define TGT_SLOTS       65536
static struct {
    const jit_exit_t *e;
    uint32_t t[4];
    uint8_t n;
} s_tgt[TGT_SLOTS];

static jit_block_t *lookup(uint32_t pc);

static void dyn_count(const jit_exit_t *e, uint32_t to)
{
    int k = e->ir == 0x4E75 ? 0 : (e->ir & 0xFFC0) == 0x4E80 ? 1 : 2;
    uint32_t h = (uint32_t)((uintptr_t)e >> 4) & (TGT_SLOTS - 1);
    for (int i = 0; i < 64 && s_tgt[h].e && s_tgt[h].e != e; i++) {
        h = (h + 1) & (TGT_SLOTS - 1);
    }
    if (s_tgt[h].e && s_tgt[h].e != e) {
        return;                             /* (the table full here) */
    }
    s_tgt[h].e = e;
    jit_dyn[k][0]++;
    int i = 0, n = s_tgt[h].n;
    while (i < n && s_tgt[h].t[i] != to) {
        i++;
    }
    if (!n) {
        jit_dyn[k][1]++;
    }
    if (i < n) {
        jit_dyn[k][2] += i < 1;
        jit_dyn[k][3] += i < 2;
        jit_dyn[k][4] += i < 4;
        if (i == 0) {
            unsigned long l0 = jit_lookups, p0 = jit_probes;
            jit_dyn[k][5] += lookup(to) != NULL;
            jit_lookups = l0, jit_probes = p0;
        }
    } else {
        i = n < 4 ? n++ : 3;
    }
    for (; i > 0; i--) {
        s_tgt[h].t[i] = s_tgt[h].t[i - 1];
    }
    s_tgt[h].t[0] = to;
    s_tgt[h].n = (uint8_t)n;
}
#define STAT(x) (jit_stats.x)
#else
static struct { unsigned long translations, flushes, full, drops, blocks_run, code_bytes, insns, links, kills, fills, resumes; } jit_stats;
#define STAT(x) (jit_stats.x)
static uint64_t s_us_translate, s_us_sync;

static size_t s_size;

/* (the console's J) */
void jit_report(void)
{
    printf("JIT: %lu translations (%lu instructions, %lu bytes of code), %lu flushes (%lu full), %lu drops, "
           "%lu blocks run, %lu resumed, %lu links, %lu kills, %lu targets kept; %llu us translating, %llu us syncing; the "
           "arena %u KB, %u KB of PSRAM free\n",
           jit_stats.translations, jit_stats.insns, jit_stats.code_bytes, jit_stats.flushes, jit_stats.full,
           jit_stats.drops, jit_stats.blocks_run, jit_stats.resumes, jit_stats.links, jit_stats.kills, jit_stats.fills,
           (unsigned long long)s_us_translate,
           (unsigned long long)s_us_sync,
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
    s_res = NULL;
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
        s_res = (jit_res_t *)(s_arena + BANKS * sizeof(*s_dir));
        memset(s_res, 0, RES_SLOTS * sizeof(*s_res));
        s_start = (BANKS * sizeof(*s_dir) + RES_SLOTS * sizeof(*s_res) + 15) & ~(size_t)15;
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
        memset(s_res, 0, RES_SLOTS * sizeof(*s_res));
    }
    memset(jit_code_pages, 0, sizeof(jit_code_pages));
    memset(s_heat, 0, sizeof(s_heat));
    memset(s_failed, 0, sizeof(s_failed));
    s_used = s_start;
    s_flush_pending = false;
    s_ndead = 0;
#ifndef ESP_PLATFORM
    memset(s_tgt, 0, sizeof(s_tgt));
#endif
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

/* Each page's blocks: a list, and a mask of the words they cover (a bit a
 * 68k word): a write beside code (a game's variables between its routines)
 * looks at the mask, not the blocks (Leading Lap: 31 blocks a write, 450000
 * such writes in 1637 frames and none into code). Blocks are 256 bytes at
 * most (jit_rv.c's MAX_BYTES): each is on the lists of the one or two pages
 * it touches, so a page's list holds every block over it. */
#define MASK_WORDS      (1u << (JIT_PAGE_SHIFT - 6))    /* 32-bit words of a page's mask */
#define PAGE_WORDS      (1u << (JIT_PAGE_SHIFT - 1))

/* page p's list (its bank's table made already) */
static inline jit_block_t **page_list(uint32_t p)
{
    return &s_dir[p / BANK_PAGES][p % BANK_PAGES];
}

/* page p's mask (after its bank's lists) */
static inline uint32_t *page_mask(uint32_t p)
{
    return (uint32_t *)(s_dir[p / BANK_PAGES] + BANK_PAGES) + (p % BANK_PAGES) * MASK_WORDS;
}

/* the words of page p the bytes [lo, hi) touch: [*w0, *w1) */
static inline void page_words(uint32_t p, uint32_t lo, uint32_t hi, unsigned *w0, unsigned *w1)
{
    uint32_t base = p << JIT_PAGE_SHIFT;
    *w0 = lo > base ? (lo - base) >> 1 : 0;
    *w1 = hi < base + (1u << JIT_PAGE_SHIFT) ? (hi - base + 1) >> 1 : PAGE_WORDS;
}

/* mask word i's bits of the words [w0, w1) */
static inline uint32_t span_bits(unsigned i, unsigned w0, unsigned w1)
{
    int a = (int)w0 - 32 * (int)i, z = (int)w1 - 32 * (int)i;
    a = a < 0 ? 0 : a;
    z = z > 32 ? 32 : z;
    if (z <= a) {
        return 0;
    }
    return (z == 32 ? ~0u : (1u << z) - 1) & ~((1u << a) - 1);
}

/* b's words in page p's mask */
static void mask_add(uint32_t p, const jit_block_t *b)
{
    unsigned w0, w1;
    page_words(p, b->pc, b->end, &w0, &w1);
    uint32_t *m = page_mask(p);
    for (unsigned i = w0 >> 5; i <= (w1 - 1) >> 5; i++) {
        m[i] |= span_bits(i, w0, w1);
    }
}

/* the tables of the banks page[0] and page[1] are in, made at *at (up to
 * end) where missing; false: no room */
static bool page_tables(const uint32_t page[2], uint8_t **at, uint8_t *end)
{
    for (int i = 0; i < 2; i++) {
        uint32_t k = page[i] / BANK_PAGES;
        if (!s_dir[k]) {
            size_t n = BANK_PAGES * (sizeof(jit_block_t *) + MASK_WORDS * 4);
            if ((size_t)(end - *at) < n) {
                return false;
            }
            s_dir[k] = (jit_block_t **)*at;
            memset(s_dir[k], 0, n);
            *at += n;
#ifndef ESP_PLATFORM
            jit_bank_tables++;
            jit_bytes[JB_PAGES] += n;
#endif
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
    mask_add(p, b);
    jit_code_pages[p >> 3] |= (uint8_t)(1u << (p & 7));
}

/* b off page p's list, the mask made again from the blocks left */
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
    memset(page_mask(p), 0, MASK_WORDS * 4);
    for (jit_block_t *o = *head; o; o = o->page_next[o->page[0] == p ? 0 : 1]) {
        mask_add(p, o);
    }
    if (!*head) {
        jit_code_pages[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
}

/* A dropped block's entries back to the dispatcher: the links to them
 * come out as their exits (and the dispatcher links them anew) */
static void kill(jit_block_t *b)
{
    for (int i = 0; i < 2; i++) {
        if (i == 0 || b->entry[1] != b->entry[0]) {
            jit_rv_kill(b->entry[i]);
            sync_code(b->entry[i], 4);
        }
    }
    STAT(kills)++;
}

static void drop(jit_block_t *b)
{
    if (b->dead) {
        return;
    }
    b->dead = true;
    STAT(drops)++;
    if (!s_running) {
        kill(b);
    } else if (s_ndead < DEAD_MAX) {
        s_dead[s_ndead++] = b;
    } else {
        s_flush_pending = true;             /* (too many: all of them) */
    }
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
#ifndef ESP_PLATFORM
    int src = s_wr_cpu ? 0 : 1;
    bool paged = false, walked = false;
    if (src) {
        jit_wr[1][0]++;                     /* (the CPU's counted by jit_cpu_wrote()) */
    }
#endif
    for (uint32_t p = lo >> JIT_PAGE_SHIFT; p <= last && p < JIT_PAGES; p++) {
        uint32_t w;
        if (!(p & 31) && (memcpy(&w, &jit_code_pages[p >> 3], 4), !w)) {
            p += 31;                        /* (32 pages without code: a blit's range in a few steps) */
            continue;
        }
        if (!(jit_code_pages[p >> 3] & (1u << (p & 7)))) {
            continue;
        }
#ifndef ESP_PLATFORM
        paged = true;
#endif
        unsigned w0, w1;
        page_words(p, lo, hi, &w0, &w1);
        const uint32_t *m = page_mask(p);
        uint32_t any = 0;
        for (unsigned i = w0 >> 5; i <= (w1 - 1) >> 5; i++) {
            any |= m[i] & span_bits(i, w0, w1);
        }
        if (!any) {
            continue;                       /* (beside the page's code) */
        }
#ifndef ESP_PLATFORM
        walked = true;
#endif
        for (jit_block_t *b = *page_list(p); b; ) {
            jit_block_t *next = b->page_next[b->page[0] == p ? 0 : 1];
#ifndef ESP_PLATFORM
            jit_wr[src][3]++;
#endif
            if (b->pc < hi && lo < b->end) {
#ifndef ESP_PLATFORM
                jit_wr[src][4]++;
                pc_count(jit_drop_pcs, b->pc)->n++;
#endif
                drop(b);
            }
            b = next;
        }
    }
#ifndef ESP_PLATFORM
    if (src) {
        jit_wr[1][1] += paged;
    }
    jit_wr[src][2] += walked;
    if (src == 0 && walked) {
        uint32_t pc = s_running ? m68k_get_reg(NULL, M68K_REG_PC) : m68k_get_reg(NULL, M68K_REG_PPC);
        jit_pc_count_t *c = pc_count(jit_wr_pcs, pc);
        c->n++;
        c->m = lo;
    }
#endif
}

/* The CPU's write of 1-4 bytes at a (jit_check_write(): its 2 KB has
 * code). Most land beside code, in the same page: the page's bit or its
 * mask says so here, without jit_written()'s loop (Leading Lap: ~850 such
 * writes a frame, 1.4% of core 1 through the loop) */
void jit_cpu_wrote(uint32_t a, int bytes)
{
    a &= 0xFFFFFF;
    uint32_t z = a + (uint32_t)bytes - 1, p = a >> JIT_PAGE_SHIFT;
#ifndef ESP_PLATFORM
    jit_wr[0][0]++;
#endif
    if (p == z >> JIT_PAGE_SHIFT) {
        if (!(jit_code_pages[p >> 3] & (1u << (p & 7)))) {
            return;
        }
#ifndef ESP_PLATFORM
        jit_wr[0][1]++;
#endif
        const uint32_t *m = page_mask(p);
        unsigned w = (a & ((1u << JIT_PAGE_SHIFT) - 1)) >> 1, last = (z & ((1u << JIT_PAGE_SHIFT) - 1)) >> 1;
        for (; w <= last; w++) {
            if (m[w >> 5] & (1u << (w & 31))) {
                break;
            }
        }
        if (w > last) {
            return;                         /* (beside the page's code) */
        }
    }
#ifndef ESP_PLATFORM
    s_wr_cpu = 1;
    jit_written(a, a + (uint32_t)bytes);
    s_wr_cpu = 0;
#else
    jit_written(a, a + (uint32_t)bytes);
#endif
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
#ifndef ESP_PLATFORM
        unsigned long pages0 = jit_bytes[JB_PAGES];
#endif
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
        jit_stats.rv_words += (unsigned long)(code_end - (const uint8_t *)b->code) / 4 - 4ul * b->nexits;
        jit_bytes[JB_HEADER] += (unsigned long)(code - (uint8_t *)b);
        jit_bytes[JB_ALIGN] += (unsigned long)((s_arena + s_used) - code_end) -
                               (jit_bytes[JB_PAGES] - pages0);
        ws_note(pc, b->end, (unsigned long)(s_arena + s_used - (uint8_t *)b) - (jit_bytes[JB_PAGES] - pages0));
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
#ifndef ESP_PLATFORM
    jit_lookups++;
#endif
    for (jit_block_t *b = s_hash[hash(pc)]; b; b = b->hash_next) {
#ifndef ESP_PLATFORM
        jit_probes++;
#endif
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

/* The state at an exit (its record) */
static inline void exit_state(const jit_exit_t *e)
{
    if (e->kind == JIT_EXIT_FB) {
        return;
    }
    if (e->kind != JIT_EXIT_DYN) {
        *m68k_loop.pc = e->pc;
    }
    *m68k_loop.ppc = e->ppc & 0xFFFFFF;     /* (above: the way back to its block) */
    *m68k_loop.ir = e->ir;
    mem_fetch_state(e->fe);
}

/* (the record of the exit the code left by) */
static inline const jit_exit_t *run_code(const uint32_t *code)
{
#ifdef ESP_PLATFORM
    return (const jit_exit_t *)((uint32_t (*)(void))code)();
#else
    return (const jit_exit_t *)(uintptr_t)rvsim_call((uint32_t)(uintptr_t)code);
#endif
}

static inline jit_res_t *res_slot(uint32_t pc)
{
    return &s_res[(pc >> 1) & (RES_SLOTS - 1)];
}

/* The block whose exit e is (links may have gone on from the one run): of
 * the blocks from its first longword (s2 as it left: jit_rf[3]) the last
 * made before e. (Any block with an instruction at the PC would do: a
 * resume takes the registers as the exit left them, checked against the
 * flags in memory) */
static const jit_block_t *exit_block(const jit_exit_t *e)
{
    uint32_t base = jit_rf[3] & 0xFFFFFC;
    const jit_block_t *best = NULL;
    for (int k = 0; k < 2; k++) {
        const jit_block_t *c = lookup(base + 2 * (uint32_t)k);
        if (c && (const void *)c < (const void *)e && (!best || c > best)) {
            best = c;
        }
    }
    return best;
}

/* A link exit (its record) made a jump to b's entry: the one for an
 * opcode reached in sequence when the exit's last word fetched ends at it */
static void link(const jit_exit_t *e, const jit_block_t *b)
{
    uint32_t a = b->pc;
    bool seq = a == ((e->fe & 0xFFFF) ? e->fe : 1);
    uint32_t *site = (uint32_t *)(uintptr_t)e - 3;
    jit_rv_link(site, b->entry[seq]);
    sync_code(site, 8);
    STAT(links)++;
}

/* A computed exit (its record) came to b: b kept as its latest target
 * (data the exit's code reads: no caches to sync) */
static void remember(const jit_exit_t *e, const jit_block_t *b)
{
    uint32_t a = b->pc;
    bool seq = a == ((e->fe & 0xFFFF) ? e->fe : 1);
    uint32_t *t = (uint32_t *)(uintptr_t)e + 4;
    if (t[0] != a) {
        t[2] = t[0];
        t[3] = t[1];
        t[0] = a;
    }
    t[1] = (uint32_t)(uintptr_t)b->entry[seq];
    STAT(fills)++;
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
    if (__builtin_expect(!s_ic_next, 0)) {
        jit_env_t env;
        mem_jit_env(&env);
        s_ic_next = env.ic_next;
    }
    unsigned int *pcr = m68k_loop.pc;
    int *remaining = m68k_loop.remaining;
    const unsigned int *tracing = m68k_loop.tracing;
    const jit_exit_t *from = NULL;          /* a link or computed exit the PC came by: on to its block next time */
#ifndef ESP_PLATFORM
    s_why = 0;
    bool first = true;
    static const jit_block_t *last_b;
#endif
    do {
        uint32_t a = *pcr;
        if (a & 1) {
            return 1;                       /* (Musashi's: the address error) */
        }
        jit_block_t *b = a <= 0xFFFFFF ? lookup(a) : NULL;
        const uint32_t *code = b ? b->code : NULL;
        bool resumed = false;
#ifndef ESP_PLATFORM
        int how = b ? 0 : 1;
#endif
        if (!b && a <= 0xFFFFFF) {
            /* inside a block a slice ended in: on from there (a link's or
             * computed exit's target gets a block of its own when hot: one
             * to link to) */
            jit_res_t *r = s_res ? res_slot(a) : NULL;
            if (!from && r && r->b && r->pc == a && (code = jit_rv_resume(r->b, a, r->f, r->j)) != NULL) {
                b = (jit_block_t *)(uintptr_t)r->b;
                resumed = true;
            } else if ((from || a != *s_ic_next || s_cont
#ifndef ESP_PLATFORM
                        || jit_heat_all
#endif
                       ) && ++s_heat[hash(a)] >= jit_hot) {
                s_heat[hash(a)] = 0;
                b = translate(a);
                code = b ? b->code : NULL;
                if (!b) {
                    s_failed[hash(a) >> 3] |= (uint8_t)(1u << (hash(a) & 7));
                }
#ifndef ESP_PLATFORM
                how = b ? 3 : 2;
#endif
            }
            r = s_res ? res_slot(a) : NULL;     /* (translate() may have flushed) */
            if (!b && from && r && r->b && r->pc == a && (code = jit_rv_resume(r->b, a, r->f, r->j)) != NULL) {
                b = (jit_block_t *)(uintptr_t)r->b;
                resumed = true;
            }
        }
#ifndef ESP_PLATFORM
        if (first && a <= 0xFFFFFF) {
            jit_res_t *r = s_res ? res_slot(a) : NULL;
            int k = how == 0 ? 0 : resumed ? 1 : r && r->b && r->pc == a ? 2
                  : last_b && a == last_b->end ? 4 : 3;
            jit_first[s_last_end][k]++;
            if (k == 3 && s_last_end == 0) {
                jit_pc_count_t *c = pc_count(jit_noslot_pcs, a);
                c->n++;
                c->m = s_last_pc;
            }
        }
        first = false;
        if (from && from->kind == JIT_EXIT_LINK) {
            jit_after_link[how]++;
            if (how == 2) {
                pc_count(jit_untrans_pcs, a)->n++;
            }
        }
#endif
        if (!b) {
#ifndef ESP_PLATFORM
            if (a <= 0xFFFFFF) {
                jit_interp_count[amiga_peek16(a)]++;
            }
            jit_interp_total++;
#endif
            s_cont = a <= 0xFFFFFF && (s_failed[hash(a) >> 3] & (1u << (hash(a) & 7)));
            mem_interp_one();
            from = NULL;
#ifndef ESP_PLATFORM
            s_why = 3;
            s_last_end = 4;
#endif
            continue;
        }
        if (from && !resumed) {
            if (from->kind == JIT_EXIT_LINK) {
                link(from, b);
            } else {
                remember(from, b);
            }
        }
        s_running = true;
        jit_stop = false;
        STAT(blocks_run)++;
        if (resumed) {
            STAT(resumes)++;
        }
        const jit_exit_t *e = run_code(code);
        exit_state(e);
        s_running = false;
        s_cont = false;
        if ((e->kind == JIT_EXIT_PC || e->kind == JIT_EXIT_FB) && s_res) {
            /* (ended inside the block that left: the next slice resumes it
             * there; else the code after it may start a block) */
            uint32_t back = JIT_EXIT_BACK(e);
            const jit_block_t *x = back ? (const jit_block_t *)(uintptr_t)((const uint32_t *)e - back) : exit_block(e);
            uint32_t pc = *pcr;
            if (x && pc - x->pc - 1 < x->end - x->pc - 1 && !x->dead) {
                jit_res_t *r = res_slot(pc);
                r->pc = pc;
                r->b = x;
                memcpy(r->f, jit_rf, sizeof(r->f));
                r->j = JIT_EXIT_RES(e);
            } else {
                s_cont = true;
            }
        }
#ifndef ESP_PLATFORM
        jit_exits[e->kind][*remaining <= 0 || *tracing]++;
        if (e->kind == JIT_EXIT_DYN && *remaining > 0 && !*tracing) {
            dyn_count(e, *pcr);
        }
#endif
        for (int i = 0; i < s_ndead; i++) {
            kill(s_dead[i]);
        }
        s_ndead = 0;
        from = e->kind == JIT_EXIT_LINK || e->kind == JIT_EXIT_DYN ? e : NULL;
#ifndef ESP_PLATFORM
        s_why = e->kind == JIT_EXIT_LINK ? 1 : e->kind == JIT_EXIT_DYN ? 2 : 4;
        s_last_end = e->kind;
        last_b = b;
        s_last_pc = *pcr;
#endif
        if (s_flush_pending) {
            flush_now();
            mem_fetch_reset();              /* (the exit's state is the old map's) */
            from = NULL;
        }
    } while (*remaining > 0 && !*tracing);
    return 1;
}
#endif
