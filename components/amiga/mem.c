/*
 * The 68000's memory map and the Musashi bus callbacks.
 *
 *   $000000-$1FFFFF  chip RAM (mirrored by its size); the ROM for reads
 *                    while OVL is set (after reset), writes still to RAM
 *   $200000-$9FFFFF  fast RAM once autoconfigured (autoconf.c)
 *   $A00000-$BFFFFF  the CIAs: CIA-A while A12 is low (odd bytes), CIA-B
 *                    while A13 is low (even bytes), a register every 256
 *   $C00000-$D7FFFF  slow RAM (the A500's trapdoor); above it, the custom
 *                    chips' mirror Kickstart sizes it by (1.2 takes any
 *                    space that isn't the mirror for RAM)
 *   $DA0000-$DAFFFF  Gayle's IDE and its registers, $DE1000 its ID (gayle.c;
 *                    the A600's, the others' with a hard disk in)
 *   $DC0000-$DCFFFF  the battery-backed clock (rtc.c)
 *   $DFF000-$DFFFFF  the custom chips (registers mirrored every $200)
 *   $E80000-$E8FFFF  autoconfig: the next board to configure
 *   $F80000-$FFFFFF  Kickstart (256 KB ones mirrored)
 *
 * No bus errors: an Amiga 500's empty space just reads nothing (0 here).
 *
 * The A1200's 68EC020 waits for the chip bus: each access to chip RAM (32
 * bits at once), the custom chips or a CIA costs it WAIT_* of its clocks
 * (s_m.wait by 64 KB bank, the others in the slow path); the ROM and fast
 * RAM don't, nor instructions from its cache (below). The 68000's waits
 * are counted otherwise (the slots, below).
 */
#include <string.h>
#include "m68k.h"
#include "amiga_internal.h"

#define EVEN_SLOTS      116             /* (a line's 114) */
#define LINE_CCK        (2 * EVEN_SLOTS)

/* The banks and the 68000's bus cycles: one block (one base address on the
 * P4: every access needs several of these)
 */
static struct {
    int after;                          /* the 68000: Musashi's remaining cycles after its last access */
    int n;                              /* its bus cycles in this instruction so far */
    int last_at, last_wait;             /* its last access: when it began, its wait */
    uint8_t no_wait;                    /* the kinds of bank that can't wait now (below) */
    uint8_t slow;                       /* SLOW_BLIT: bus_cycle() works the chip bus's waits out */
    uint8_t cpu020;                     /* ami.cpu_shift */
    uint8_t kind[256];                  /* the 68000's banks that wait (below) */
    uint8_t wait[256];                  /* the 68EC020's waits, by bank (0: none) */
    uint16_t wait_at[LINE_CCK];         /* the clocks an access at colour clock h waits (below) */
    const uint8_t *rmap[256];           /* each bank's host memory less the bank's address */
    uint8_t *wmap[256];                 /* (the byte at a: rmap[a >> 16] + a; NULL: the slow path) */
} s_m = { .after = -1 };

#define WAIT_CHIP       8               /* two chip bus cycles (SysInfo: 1.04 times an A1200) */
#define WAIT_CUSTOM     8
#define WAIT_CIA        20

/* ---- The 68000 on the chip bus, slot by slot ----
 *
 * The 68000 has the even colour clocks of a line the DMA leaves it
 * (refresh, disk, audio, sprites and up to 4 lores planes use odd ones):
 * lores planes 5-6, hires 3-4, super-hires 2 and the copper take even ones
 * (display.c and copper.c mark them as they happen), the blitter what it
 * can. Each access of the 68000's to chip RAM, slow RAM or the chips is at
 * its instruction's start plus 4 clocks a bus cycle before it, and waits
 * for the next even slot free; the wait comes off Musashi's remaining
 * cycles, as the 68EC020's waits do (it keeps its averages: s_m.wait).
 */
static bool s_taken[EVEN_SLOTS + 1];        /* this line's even slots the DMA has (h / 2) */
static bool s_any_taken;
/* s_m.kind: */
#define ON_CHIP_BUS     1                   /* on the chip bus (DMA's slots) */
#define ON_E_CLOCK      2                   /* the CIAs (the E clock) */
/* s_m.slow: */
#define SLOW_BLIT       1                   /* a blit under way in this line (perhaps ended) */
#define BLIT_WAIT       6                   /* a blit leaves the 68000 one slot in four (BLTPRI off) */

/* s_m.no_wait: ON_CHIP_BUS while no slot of this line is taken and no blit
 * under way in it (an access on the chip bus waits for nothing then: no
 * need to know when it is), else 0 (banks off the chip bus). Below it,
 * without SLOW_BLIT, the taken slots alone: wait_at's.
 */
void bus_line_start(void)
{
    if (s_any_taken) {
        memset(s_taken, 0, sizeof(s_taken));
        memset(s_m.wait_at, 0, sizeof(s_m.wait_at));
        s_any_taken = false;
    }
    s_m.slow = blit.busy ? SLOW_BLIT : 0;
    s_m.no_wait = blit.busy ? 0 : ON_CHIP_BUS;
}

/* An even slot taken at `at` (68000 clocks into the frame). wait_at[h], the
 * clocks an access at colour clock h waits, is past the taken even slots
 * from it on, 4 clocks each: a slot newly taken adds 4 to its own and to
 * those of the taken ones in a row before it (and the odd clocks between).
 * (Not the 2 clocks a 68000 on an odd colour clock would wait for the even
 * one, which a PAL line's 227 bring about from line to line: with them
 * SysInfo and xSysInfo both gave 1.03, but Hyperion's and Lethal Xcess's
 * loaders failed, their CPU too slow; Musashi knows an instruction's bus
 * cycles, not where in it they fall, and the cycle table's rounding to 4
 * keeps the phase.) The 68EC020 keeps its averages: none of this.
 */
void bus_take(int at)
{
    int e = (at - ami.line_start) >> 2;     /* (h / 2) */
    if (e < 0 || e >= EVEN_SLOTS || s_m.cpu020) {
        return;
    }
    s_any_taken = true;
    s_m.no_wait = 0;
    if (s_taken[e]) {
        return;
    }
    s_taken[e] = true;
    uint16_t wait = s_m.wait_at[2 * e + 1]; /* (the next even slot's) */
    for (;;) {
        wait += 4;
        s_m.wait_at[2 * e] = wait;
        if (e == 0) {
            break;
        }
        s_m.wait_at[2 * e - 1] = wait;
        if (!s_taken[--e]) {
            break;
        }
    }
}

/* A blit started (until the next line the accesses work their waits out) */
void bus_blit_started(void)
{
    s_m.slow |= SLOW_BLIT;
    s_m.no_wait = 0;
}

/* The clocks an access at `at` waits for its slot */
static inline int slot_wait(int at)
{
    int h = (at - ami.line_start) >> 1;
    if (h < 0) {
        return 0;
    }
    if (blit.busy && at < blit.end) {
        return (ami.dmacon & DMA_BLTPRI) ? blit.end - at : BLIT_WAIT;
    }
    if (!s_any_taken || h >= LINE_CCK) {
        return 0;
    }
    return s_m.wait_at[h];
}

/* A bus cycle of the 68000's: its wait on the chip bus */
static void bus_cycle(uint32_t a)
{
    extern int m68ki_initial_cycles, m68ki_remaining_cycles;
    if (m68ki_remaining_cycles != s_m.after) {
        s_m.n = 0;                          /* (Musashi took an instruction's cycles: the next one) */
    }
    s_m.last_wait = 0;
    if (s_m.kind[(a >> 16) & 0xFF] && ami.cpu_running) {
        /* (ami_now() for the 68000: no shift, no remainder) */
        int at = ami.cycle + m68ki_initial_cycles - m68ki_remaining_cycles + 4 * s_m.n;
        s_m.last_at = at;
        /* (an E clock access begins a bus cycle sooner: the 68000 fetched
         * the opcode during the instruction before; the slots keep their
         * tuning) */
        s_m.last_wait = s_m.kind[(a >> 16) & 0xFF] == ON_E_CLOCK ? ami_e_wait(at - 4) : slot_wait(at);
        m68ki_remaining_cycles -= s_m.last_wait;
    }
    s_m.n++;
    s_m.after = m68ki_remaining_cycles;
}

/* bus_cycle() for `cycles` of them (1 or 2) to a bank of `kind`, inline
 * where it's quick: a bank that can't wait now (off the chip bus, or on it
 * with s_m.no_wait) only counts them (last_at isn't needed: last_wait 0);
 * on the chip bus with only taken slots in the way, wait_at has each
 * one's wait. false: the bus_cycle() way (a blit started in this line, the
 * CIAs, the CPU not running; nothing counted yet).
 */
static inline bool bus_cycles_quick(int kind, int cycles)
{
    extern int m68ki_initial_cycles, m68ki_remaining_cycles;
    int r = m68ki_remaining_cycles;
    int n = r == s_m.after ? s_m.n : 0;
    int wait = 0;
    if (__builtin_expect(kind > s_m.no_wait, 0)) {
        if (kind != ON_CHIP_BUS || s_m.slow || !ami.cpu_running) {
            return false;
        }
        /* (slot_wait() without a blit: from the line's start) */
        int at = ami.cycle + m68ki_initial_cycles - r + 4 * n - ami.line_start;
        for (int i = 0; i < cycles; i++) {
            at += i ? 4 + wait : 0;
            unsigned h = (unsigned)(at >> 1);
            wait = h < LINE_CCK ? s_m.wait_at[h] : 0;
            r -= wait;
        }
        if (wait) {
            s_m.last_at = at + ami.line_start;
        }
        m68ki_remaining_cycles = r;
    }
    s_m.n = n + cycles;
    s_m.after = r;
    s_m.last_wait = wait;
    return true;
}

/* The 68000 decides on an interrupt as its instruction's last bus cycle
 * begins (WinUAE: 2 clocks into it): one raised while that cycle waits for
 * the DMA comes after the next instruction. (Musashi doesn't place its
 * prefetch at that bus cycle: the instruction's last access stands for it.) Lionheart polls INTREQR for
 * VERTB from slow RAM with exec's VERTB interrupt on; the copper's list at
 * the top of the frame holds the loop's BEQ, and it never saw the bit.
 */
bool bus_waited_over(int t)
{
    return s_m.last_wait > 0 && s_m.last_at < t && s_m.last_at + s_m.last_wait >= t &&
           s_m.last_at + s_m.last_wait <= ami.cycle;
}

static inline uint16_t rd16(const uint8_t *p, uint32_t off)
{
    return *(const uint16_t *)(p + off);
}

static inline void wr16(uint8_t *p, uint32_t off, uint16_t v)
{
    *(uint16_t *)(p + off) = v;
}

static void ic_map_changed(void);

void mem_update_map(void)
{
    ic_map_changed();
    s_m.cpu020 = ami.cpu_shift != 0;
    memset(s_m.rmap, 0, sizeof(s_m.rmap));
    memset(s_m.wmap, 0, sizeof(s_m.wmap));
    memset(s_m.wait, 0, sizeof(s_m.wait));
    memset(s_m.kind, 0, sizeof(s_m.kind));
    for (uint32_t b = 0x00; b < 0x20; b++) {
        uint8_t *p = ami.chip + ((b << 16) & (ami.chip_size - 1));
        s_m.rmap[b] = p;
        s_m.wmap[b] = p;
        s_m.wait[b] = ami.cpu_shift ? WAIT_CHIP : 0;
        s_m.kind[b] = ami.cpu_shift ? 0 : ON_CHIP_BUS;
    }
    /* (the 68000: slow RAM, the chips and their mirrors on the chip bus too;
     * the CIAs on its own side, the E clock's) */
    for (uint32_t b = 0xA0; b < 0xE0 && !ami.cpu_shift; b++) {
        s_m.kind[b] = b < 0xC0 ? ON_E_CLOCK : (b < 0xD8 || b == 0xDF) ? ON_CHIP_BUS : 0;
    }
    if (ami.ovl) {
        for (uint32_t b = 0x00; b < 0x08; b++) {
            s_m.rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
        }
    }
    for (uint32_t b = 0; b < (ami.slow_size >> 16); b++) {
        s_m.rmap[0xC0 + b] = s_m.wmap[0xC0 + b] = ami.slow + (b << 16);
    }
    uint32_t board, board_size;
    uint8_t *mem = hdboard_memory(&board, &board_size);
    for (uint32_t b = 0; mem && board && b < (board_size >> 16); b++) {
        s_m.rmap[(board >> 16) + b] = s_m.wmap[(board >> 16) + b] = mem + (b << 16);
    }
    for (uint32_t b = 0; ami.fast_base && b < (ami.fast_size >> 16); b++) {
        s_m.rmap[(ami.fast_base >> 16) + b] = s_m.wmap[(ami.fast_base >> 16) + b] = ami.fast + (b << 16);
    }
    for (uint32_t b = 0; ami.ext && b < (EXT_ROM_SIZE >> 16); b++) {
        s_m.rmap[0xE0 + b] = ami.ext + (b << 16);
    }
    for (uint32_t b = 0xF8; b < 0x100; b++) {
        s_m.rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
    }
    /* (each less its bank's address: address a's byte at rmap[a >> 16] + a) */
    for (uint32_t b = 0; b < 0x100; b++) {
        if (s_m.rmap[b]) {
            s_m.rmap[b] -= b << 16;
        }
        if (s_m.wmap[b]) {
            s_m.wmap[b] -= b << 16;
        }
    }
}

/* ---- The CIAs: CIA-A on the data bus's low byte, CIA-B on the high one.
 * A byte write reaches both halves of the bus (the 68000 puts it on both),
 * so it goes to whichever chip is selected.
 */

static uint8_t cia_read8(uint32_t a)
{
    int reg = (a >> 8) & 0xF;
    if (a & 1) {
        return (a & 0x1000) ? 0xFF : cia_read(&cia[0], reg);
    }
    return (a & 0x2000) ? 0xFF : cia_read(&cia[1], reg);
}

static void cia_write8(uint32_t a, uint8_t v)
{
    int reg = (a >> 8) & 0xF;
    if (!(a & 0x1000)) {
        cia_write(&cia[0], reg, v);
    }
    if (!(a & 0x2000)) {
        cia_write(&cia[1], reg, v);
    }
}

/* (slow RAM's banks never get here) */
static inline bool is_custom(uint32_t a)
{
    return (a >= 0xDFF000 && a < 0xE00000) || (a >= 0xC00000 && a < 0xD80000);
}

static inline bool is_cia(uint32_t a)
{
    return a >= 0xA00000 && a < 0xC00000;
}

static inline bool is_rtc(uint32_t a)
{
    return a >= 0xDC0000 && a < 0xDD0000;
}

static inline bool is_autoconf(uint32_t a)
{
    return a >= 0xE80000 && a < 0xE90000;
}

static inline bool is_gayle(uint32_t a)
{
    return ((a >= 0xDA0000 && a < 0xDB0000) || (a >= 0xDE1000 && a < 0xDE2000)) && gayle_present();
}

/* The 68EC020's wait in the slow path: the custom chips' or a CIA's */
static inline void io_wait(uint32_t a)
{
    if (ami.cpu_shift) {
        ami_cpu_wait(is_cia(a) ? WAIT_CIA : is_custom(a) ? WAIT_CUSTOM : 0);
    }
}

static unsigned int slow_read16(uint32_t a)
{
    io_wait(a);
    if (is_custom(a)) {
        return custom_read(a & 0x1FE);
    }
    if (is_rtc(a)) {
        /* (the clock drives 4 data bits; the others float: xSysInfo takes
         * zeros there for an A1000's custom chips mirrored at $DCF004)
         */
        return 0xFFF0 | rtc_read((a >> 2) & 15);
    }
    if (is_cia(a)) {
        int reg = (a >> 8) & 0xF;
        uint16_t hi = (a & 0x2000) ? 0xFF : cia_read(&cia[1], reg);
        uint16_t lo = (a & 0x1000) ? 0xFF : cia_read(&cia[0], reg);
        return (hi << 8) | lo;
    }
    if (is_autoconf(a)) {
        return autoconf_read16(a);
    }
    if (is_gayle(a)) {
        return gayle_read16(a);
    }
    return 0;
}

static unsigned int slow_read8(uint32_t a)
{
    io_wait(a);
    if (is_custom(a)) {
        uint16_t w = custom_read(a & 0x1FE);
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    if (is_cia(a)) {
        return cia_read8(a);
    }
    if (is_rtc(a)) {
        return (a & 1) ? 0xF0 | rtc_read((a >> 2) & 15) : 0xFF;
    }
    if (is_autoconf(a)) {
        return autoconf_read8(a);
    }
    if (is_gayle(a)) {
        return gayle_read8(a);
    }
    return 0;
}

static void slow_write16(uint32_t a, uint16_t v)
{
    io_wait(a);
    if (is_custom(a)) {
        custom_write(a & 0x1FE, v, ami_now(), false);
    } else if (is_cia(a)) {
        int reg = (a >> 8) & 0xF;
        if (!(a & 0x2000)) {
            cia_write(&cia[1], reg, v >> 8);
        }
        if (!(a & 0x1000)) {
            cia_write(&cia[0], reg, v & 0xFF);
        }
    } else if (is_rtc(a)) {
        rtc_write((a >> 2) & 15, v & 0xFF);
    } else if (is_autoconf(a)) {
        autoconf_write16(a, v);
    } else if (is_gayle(a)) {
        gayle_write16(a, v);
    }
}

static void slow_write8(uint32_t a, uint8_t v)
{
    io_wait(a);
    if (is_custom(a)) {
        /* the byte on both halves of the bus: a word to the chips */
        custom_write(a & 0x1FE, (v << 8) | v, ami_now(), false);
    } else if (is_cia(a)) {
        cia_write8(a, v);
    } else if (is_rtc(a)) {
        rtc_write((a >> 2) & 15, v);
    } else if (is_autoconf(a)) {
        autoconf_write8(a, v);
    } else if (is_gayle(a)) {
        gayle_write8(a, v);
    }
}

/* ---- Musashi's callbacks ---- */

/* A read on the 68000's chip bus drives the word too, including opcode
 * fetches from chip/slow RAM. Write-only custom registers then read this
 * word, not just the last custom-register write. Otherwise a byte RAM
 * probe can mistake their mirrors for RAM (Steel Empire's stack at D80000).
 */
static inline uint16_t cpu_bus_word(uint32_t a, uint16_t v)
{
    if (s_m.kind[a >> 16] == ON_CHIP_BUS) {
        ami.bus = v;
    }
    return v;
}

/* The 68000's other accesses (a blit started in this line, the CIAs, the
 * chips' registers): bus_cycle() first, as it always was. Out of line: the
 * callbacks' quick path (bus_cycles_quick()) needs no stack frame.
 */
static unsigned int AMI_HOT __attribute__((noinline)) read8_000(uint32_t a)
{
    bus_cycle(a);
    const uint8_t *p = s_m.rmap[a >> 16];
    if (p) {
        cpu_bus_word(a, rd16(p, a & ~1u));
        return p[a ^ 1];
    }
    return slow_read8(a);
}

static unsigned int AMI_HOT __attribute__((noinline)) read16_000(uint32_t a)
{
    bus_cycle(a);
    const uint8_t *p = s_m.rmap[a >> 16];
    if (p) {
        return cpu_bus_word(a, rd16(p, a));
    }
    return slow_read16(a);
}

static unsigned int AMI_HOT __attribute__((noinline)) read32_000(uint32_t a)
{
    const uint8_t *p = s_m.rmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        bus_cycle(a);
        bus_cycle(a);
        return ((uint32_t)rd16(p, a) << 16) | cpu_bus_word(a, rd16(p, a + 2));
    }
    uint32_t hi = m68k_read_memory_16(a);
    return (hi << 16) | m68k_read_memory_16((a + 2) & 0xFFFFFF);
}

static void AMI_HOT __attribute__((noinline)) write8_000(uint32_t a, unsigned int v)
{
    bus_cycle(a);
    uint8_t *p = s_m.wmap[a >> 16];
    if (p) {
        cpu_bus_word(a, (uint8_t)v * 0x0101u);
        p[a ^ 1] = v;
        return;
    }
    slow_write8(a, v);
}

static void AMI_HOT __attribute__((noinline)) write16_000(uint32_t a, unsigned int v)
{
    bus_cycle(a);
    uint8_t *p = s_m.wmap[a >> 16];
    if (p) {
        cpu_bus_word(a, v);
        wr16(p, a, v);
        return;
    }
    slow_write16(a, v);
}

static void AMI_HOT __attribute__((noinline)) write32_000(uint32_t a, unsigned int v)
{
    uint8_t *p = s_m.wmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        bus_cycle(a);
        bus_cycle(a);
        cpu_bus_word(a, v);
        wr16(p, a, v >> 16);
        wr16(p, a + 2, v & 0xFFFF);
        return;
    }
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16((a + 2) & 0xFFFFFF, v & 0xFFFF);
}

/* Each callback: the 68000's access to plain memory inline (off the chip
 * bus, or on it while nothing or only this line's taken slots are in the
 * way), the rest as above; then the 68EC020's.
 */
unsigned int AMI_HOT m68k_read_memory_8(unsigned int a)
{
    uint32_t b = a >> 16;
    const uint8_t *p = s_m.rmap[b];
    if (!s_m.cpu020) {
        int kind = s_m.kind[b];
        if (p && bus_cycles_quick(kind, 1)) {
            if (kind) {
                ami.bus = rd16(p, a & ~1u);
            }
            return p[a ^ 1];
        }
        return read8_000(a);
    }
    if (s_m.wait[b]) {
        ami_cpu_wait(s_m.wait[b]);
    }
    return p ? p[a ^ 1] : slow_read8(a);
}

unsigned int AMI_HOT m68k_read_memory_16(unsigned int a)
{
    uint32_t b = a >> 16;
    const uint8_t *p = s_m.rmap[b];
    if (!s_m.cpu020) {
        int kind = s_m.kind[b];
        if (p && bus_cycles_quick(kind, 1)) {
            uint16_t v = rd16(p, a);
            if (kind) {
                ami.bus = v;
            }
            return v;
        }
        return read16_000(a);
    }
    if (s_m.wait[b]) {
        ami_cpu_wait(s_m.wait[b]);
    }
    return p ? rd16(p, a) : slow_read16(a);
}

unsigned int AMI_HOT m68k_read_memory_32(unsigned int a)
{
    uint32_t b = a >> 16;
    const uint8_t *p = s_m.rmap[b];
    if (!s_m.cpu020) {
        int kind = s_m.kind[b];
        if (p && (a & 0xFFFF) <= 0xFFFC && bus_cycles_quick(kind, 2)) {
            uint16_t lo = rd16(p, a + 2);
            if (kind) {
                ami.bus = lo;
            }
            return ((uint32_t)rd16(p, a) << 16) | lo;
        }
        return read32_000(a);
    }
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        if (s_m.wait[b]) {
            ami_cpu_wait(s_m.wait[b]);
        }
        return ((uint32_t)rd16(p, a) << 16) | rd16(p, a + 2);
    }
    uint32_t hi = m68k_read_memory_16(a);
    return (hi << 16) | m68k_read_memory_16((a + 2) & 0xFFFFFF);
}

/* ---- The 68EC020's instruction cache ----
 *
 * 64 longwords, direct-mapped by address bits 7-2, tagged with the rest,
 * filled while CACR's E bit is set (F freezes it; a write with C set
 * empties it). An instruction word from a longword in it costs no bus
 * time; otherwise the longword is read (the chip bus's wait), once for
 * both its words (the 68020 fetches longwords). Only the time: the words
 * themselves always come from memory (no stale ones after code changes).
 */
#define IC_LINES        64
static struct {
    uint32_t tag[IC_LINES];                 /* its longword's address | 1 (0: empty); line n
                                             * at byte n * 4: the address's bits 7-2 */
    uint32_t next;                          /* the last instruction word fetched + 2 (odd: none) */
    const uint16_t *next_word;              /* that word in host memory, while it's in the same
                                             * bank of plain memory (NULL: the general path) */
    uint32_t next_wait;                     /* (its bank's s_m.wait, while next_word is set) */
    uint32_t cacr;
    uint32_t cpu020;                        /* ami.cpu_shift (the 68000: m68k_read_memory_16()) */
} s_ic = { .next = 1 };

/* (the next word's host address may be another's now: the general path) */
static void ic_map_changed(void)
{
    s_ic.next_word = NULL;
    s_ic.cpu020 = ami.cpu_shift;
}

void mem_cacr(uint32_t cacr)
{
    if (cacr != s_ic.cacr && (cacr & 8)) {
        memset(s_ic.tag, 0, sizeof(s_ic.tag));
    }
    s_ic.cacr = cacr;
}

/* A longword's first instruction word: from the cache, else its wait (its
 * bank's) */
static inline void ic_longword(uint32_t a, const uint32_t *wait_at)
{
    uint32_t cacr = s_ic.cacr;
    if (cacr & 1) {
        uint32_t lw = (a & ~3u) | 1, *t = (uint32_t *)((uint8_t *)s_ic.tag + (a & (IC_LINES - 1) * 4));
        if (*t == lw) {
            return;
        }
        if (!(cacr & 2)) {
            *t = lw;
        }
    }
    uint32_t wait = *wait_at;
    if (wait) {
        ami_cpu_wait(wait);
    }
}

static inline void ic_word(uint32_t a)
{
    uint32_t next = s_ic.next;
    s_ic.next = a + 2;
    s_ic.next_word = NULL;
    if (a == next && (a & 2)) {
        return;                     /* the longword's second word: read with the first */
    }
    uint32_t wait = s_m.wait[a >> 16];
    ic_longword(a, &wait);
}

/* An instruction word (its wait already taken) */
static inline uint16_t program16(uint32_t a)
{
    const uint8_t *p = s_m.rmap[a >> 16];
    return p ? rd16(p, a) : slow_read16(a);
}

/* The 68EC020 runs on through the words after the last one: those come
 * through next_word (no bank, no mask), the cache and the waits as in
 * ic_word(). Any other word (a branch, a new bank, the slow path's) the
 * general way, which sets next_word up again for plain memory;
 * mem_update_map() clears it. (The 68000 first: its path as it was.)
 */
unsigned int AMI_HOT m68k_read_immediate_16(unsigned int a)
{
    if (!s_ic.cpu020) {
        return m68k_read_memory_16(a);
    }
    const uint16_t *w = s_ic.next_word;
    if (__builtin_expect(a == s_ic.next && w, 1)) {
        uint32_t next = a + 2;
        s_ic.next = next;
        s_ic.next_word = __builtin_expect(next << 16 != 0, 1) ? w + 1 : NULL;  /* (a bank's end) */
        if (!(a & 2)) {             /* (a longword's second word: read with the first) */
            ic_longword(a, &s_ic.next_wait);
        }
        return *w;
    }
    ic_word(a);
    const uint8_t *p = s_m.rmap[a >> 16];
    if (p) {
        if ((a + 2) & 0xFFFF) {
            s_ic.next_word = (const uint16_t *)(p + a + 2);
            s_ic.next_wait = s_m.wait[a >> 16];
        }
        return rd16(p, a);
    }
    return slow_read16(a);
}

/* (a long across a 64 KB bank: each word from its own; Darkage's Under
 * Pressure crashed when they both came from the slow path's zeros)
 */
unsigned int AMI_HOT m68k_read_immediate_32(unsigned int a)
{
    if (!ami.cpu_shift) {
        return m68k_read_memory_32(a);
    }
    ic_word(a);
    ic_word((a + 2) & 0xFFFFFF);
    const uint8_t *p = s_m.rmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        return ((uint32_t)rd16(p, a) << 16) | rd16(p, a + 2);
    }
    return ((uint32_t)program16(a) << 16) | program16((a + 2) & 0xFFFFFF);
}

/* The instruction at pc, then a conditional branch back to it: a loop of
 * the two (custom.c's polls)
 */
bool mem_poll_loop(uint32_t pc)
{
    char text[100];
    uint32_t b = pc + m68k_disassemble(text, pc, ami.cpu_shift ? M68K_CPU_TYPE_68EC020 : M68K_CPU_TYPE_68000);
    uint16_t op = (uint16_t)m68k_read_disassembler_16(b);
    if ((op & 0xF000) != 0x6000 || (op & 0x0F00) < 0x0200) {
        return false;               /* not a Bcc (BRA and BSR aren't) */
    }
    int8_t d8 = (int8_t)(op & 0xFF);
    if (d8 == -1) {
        return false;               /* (68020: a 32-bit displacement) */
    }
    uint32_t target = d8 ? b + 2 + d8 : b + 2 + (int16_t)m68k_read_disassembler_16(b + 2);
    return (target & 0xFFFFFF) == pc;
}

/* PC-relative operands: data (the cache holds instructions only). Musashi
 * hands these over without its 24-bit mask (its data path masks inside):
 * an index going round the top read past s_m.rmap (Desert Dream crashed)
 */
unsigned int m68k_read_pcrelative_8(unsigned int a)
{
    return m68k_read_memory_8(a & 0xFFFFFF);
}

unsigned int m68k_read_pcrelative_16(unsigned int a)
{
    return m68k_read_memory_16(a & 0xFFFFFF);
}

unsigned int m68k_read_pcrelative_32(unsigned int a)
{
    return m68k_read_memory_32(a & 0xFFFFFF);
}

void AMI_HOT m68k_write_memory_8(unsigned int a, unsigned int v)
{
    uint32_t b = a >> 16;
    uint8_t *p = s_m.wmap[b];
    if (!s_m.cpu020) {
        int kind = s_m.kind[b];
        if (p && bus_cycles_quick(kind, 1)) {
            if (kind) {
                ami.bus = (uint8_t)v * 0x0101u;
            }
            p[a ^ 1] = v;
            return;
        }
        write8_000(a, v);
        return;
    }
    if (s_m.wait[b]) {
        ami_cpu_wait(s_m.wait[b]);
    }
    if (p) {
        p[a ^ 1] = v;
        return;
    }
    slow_write8(a, v);
}

void AMI_HOT m68k_write_memory_16(unsigned int a, unsigned int v)
{
    uint32_t b = a >> 16;
    uint8_t *p = s_m.wmap[b];
    if (!s_m.cpu020) {
        int kind = s_m.kind[b];
        if (p && bus_cycles_quick(kind, 1)) {
            if (kind) {
                ami.bus = v;
            }
            wr16(p, a, v);
            return;
        }
        write16_000(a, v);
        return;
    }
    if (s_m.wait[b]) {
        ami_cpu_wait(s_m.wait[b]);
    }
    if (p) {
        wr16(p, a, v);
        return;
    }
    slow_write16(a, v);
}

void AMI_HOT m68k_write_memory_32(unsigned int a, unsigned int v)
{
    uint32_t b = a >> 16;
    uint8_t *p = s_m.wmap[b];
    if (!s_m.cpu020) {
        int kind = s_m.kind[b];
        if (p && (a & 0xFFFF) <= 0xFFFC && bus_cycles_quick(kind, 2)) {
            if (kind) {
                ami.bus = v;
            }
            wr16(p, a, v >> 16);
            wr16(p, a + 2, v & 0xFFFF);
            return;
        }
        write32_000(a, v);
        return;
    }
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        if (s_m.wait[b]) {
            ami_cpu_wait(s_m.wait[b]);
        }
        wr16(p, a, v >> 16);
        wr16(p, a + 2, v & 0xFFFF);
        return;
    }
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16((a + 2) & 0xFFFFFF, v & 0xFFFF);
}

/* ---- Bytes to and from the 68000's memory (byte n of a word at n ^ 1) ---- */

void mem_copy_to(uint32_t a, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++, a++) {
        a &= 0xFFFFFF;
        uint8_t *p = s_m.wmap[a >> 16];
        if (p) {
            p[a ^ 1] = src[i];
        } else {
            m68k_write_memory_8(a, src[i]);
        }
    }
}

void mem_copy_from(uint8_t *dst, uint32_t a, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++, a++) {
        a &= 0xFFFFFF;
        const uint8_t *p = s_m.rmap[a >> 16];
        dst[i] = p ? p[a ^ 1] : (uint8_t)m68k_read_memory_8(a);
    }
}

/* ---- Without side effects: the debugger, the disassembler ---- */

uint16_t amiga_peek16(uint32_t a)
{
    a &= 0xFFFFFE;
    const uint8_t *p = s_m.rmap[a >> 16];
    return p ? rd16(p, a) : 0;
}

uint8_t amiga_peek8(uint32_t a)
{
    uint16_t w = amiga_peek16(a);
    return (a & 1) ? (w & 0xFF) : (w >> 8);
}

/* n bytes of RAM at a (chip, slow or fast; not ROM or I/O) */
static bool ram_range(uint32_t a, uint32_t n)
{
    if ((a & 1) || n > 0x1000000 - a) {
        return false;
    }
    return a + n <= ami.chip_size || (ami.slow_size && a >= 0xC00000 && a + n <= 0xC00000 + ami.slow_size) ||
           (ami.fast_base && a >= ami.fast_base && a + n <= ami.fast_base + ami.fast_size);
}

static uint32_t peek32(uint32_t a)
{
    return (uint32_t)amiga_peek16(a) << 16 | amiga_peek16(a + 2);
}

/* exec's MemList (ExecBase + $142): MemHeaders (a Node, then mh_First,
 * mh_Lower, mh_Upper, mh_Free at 16, 20, 24, 28), each with its MemChunks
 * (mc_Next, mc_Bytes); the kind by where the memory is
 */
bool amiga_mem_free(amiga_mem_free_t *chip, amiga_mem_free_t *slow, amiga_mem_free_t *fast)
{
    amiga_mem_free_t *kinds[3] = { chip, slow, fast };
    bool known[3] = { true, true, true };
    for (int k = 0; k < 3; k++) {
        memset(kinds[k], 0, sizeof(*kinds[k]));
    }
    uint32_t exec = peek32(4);
    if (!ram_range(exec, 0x150) || peek32(exec + 38) != ~exec) {
        return false;
    }
    bool any = false;
    uint32_t mh = peek32(exec + 0x142);
    for (int n = 0; n < 16 && ram_range(mh, 32) && peek32(mh) != 0; n++, mh = peek32(mh)) {
        uint32_t lower = peek32(mh + 20), upper = peek32(mh + 24);
        int k = lower < 0x200000 ? 0 : lower >= 0xC00000 && lower < 0xD80000 ? 1 : 2;
        amiga_mem_free_t *m = kinds[k];
        m->found = any = true;
        m->free += peek32(mh + 28);
        uint32_t mc = peek32(mh + 16);
        for (int c = 0; mc != 0 && c < 4096; c++, mc = peek32(mc)) {
            if (!ram_range(mc, 8) || mc < lower || mc >= upper) {
                break;
            }
            uint32_t bytes = peek32(mc + 4);
            m->largest = bytes > m->largest ? bytes : m->largest;
        }
        if (mc != 0) {
            known[k] = false;               /* (a list being changed) */
        }
    }
    for (int k = 0; k < 3; k++) {
        if (!known[k]) {
            kinds[k]->largest = 0;
        }
    }
    return any;
}

void amiga_poke8(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFF;
    uint8_t *p = s_m.wmap[a >> 16];
    if (p) {
        p[a ^ 1] = v;
    }
}

unsigned int m68k_read_disassembler_16(unsigned int a)
{
    return amiga_peek16(a);
}

unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return ((uint32_t)amiga_peek16(a) << 16) | amiga_peek16(a + 2);
}
