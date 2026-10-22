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
 * (s_wait by 64 KB bank, the others in the slow path); the ROM and fast
 * RAM don't. The 68000's waits are counted otherwise (amiga.c).
 */
#include <string.h>
#include "m68k.h"
#include "amiga_internal.h"

static const uint8_t *s_rmap[256];
static uint8_t *s_wmap[256];
static uint8_t s_wait[256];           /* the 68EC020's waits, by bank (0: none) */

#define WAIT_CHIP       5
#define WAIT_CUSTOM     8
#define WAIT_CIA        20

static inline uint16_t rd16(const uint8_t *p, uint32_t off)
{
    return *(const uint16_t *)(p + off);
}

static inline void wr16(uint8_t *p, uint32_t off, uint16_t v)
{
    *(uint16_t *)(p + off) = v;
}

void mem_update_map(void)
{
    memset(s_rmap, 0, sizeof(s_rmap));
    memset(s_wmap, 0, sizeof(s_wmap));
    memset(s_wait, 0, sizeof(s_wait));
    for (uint32_t b = 0x00; b < 0x20; b++) {
        uint8_t *p = ami.chip + ((b << 16) & (ami.chip_size - 1));
        s_rmap[b] = p;
        s_wmap[b] = p;
        s_wait[b] = ami.cpu_shift ? WAIT_CHIP : 0;
    }
    if (ami.ovl) {
        for (uint32_t b = 0x00; b < 0x08; b++) {
            s_rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
        }
    }
    for (uint32_t b = 0; b < (ami.slow_size >> 16); b++) {
        s_rmap[0xC0 + b] = s_wmap[0xC0 + b] = ami.slow + (b << 16);
    }
    uint32_t board, board_size;
    uint8_t *mem = hdboard_memory(&board, &board_size);
    for (uint32_t b = 0; mem && board && b < (board_size >> 16); b++) {
        s_rmap[(board >> 16) + b] = s_wmap[(board >> 16) + b] = mem + (b << 16);
    }
    for (uint32_t b = 0; ami.fast_base && b < (ami.fast_size >> 16); b++) {
        s_rmap[(ami.fast_base >> 16) + b] = s_wmap[(ami.fast_base >> 16) + b] = ami.fast + (b << 16);
    }
    for (uint32_t b = 0; ami.ext && b < (EXT_ROM_SIZE >> 16); b++) {
        s_rmap[0xE0 + b] = ami.ext + (b << 16);
    }
    for (uint32_t b = 0xF8; b < 0x100; b++) {
        s_rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
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

static uint16_t slow_read16(uint32_t a)
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

static uint8_t slow_read8(uint32_t a)
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

unsigned int AMI_HOT m68k_read_memory_8(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        return p[(a & 0xFFFF) ^ 1];
    }
    return slow_read8(a);
}

unsigned int AMI_HOT m68k_read_memory_16(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        return rd16(p, a & 0xFFFF);
    }
    return slow_read16(a);
}

unsigned int AMI_HOT m68k_read_memory_32(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        if (s_wait[a >> 16]) {
            ami_cpu_wait(s_wait[a >> 16]);
        }
        return ((uint32_t)rd16(p, a & 0xFFFF) << 16) | rd16(p, (a & 0xFFFF) + 2);
    }
    uint32_t hi = m68k_read_memory_16(a);
    return (hi << 16) | m68k_read_memory_16((a + 2) & 0xFFFFFF);
}

void AMI_HOT m68k_write_memory_8(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
    if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        p[(a & 0xFFFF) ^ 1] = v;
        return;
    }
    slow_write8(a, v);
}

void AMI_HOT m68k_write_memory_16(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
    if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        wr16(p, a & 0xFFFF, v);
        return;
    }
    slow_write16(a, v);
}

void AMI_HOT m68k_write_memory_32(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        if (s_wait[a >> 16]) {
            ami_cpu_wait(s_wait[a >> 16]);
        }
        wr16(p, a & 0xFFFF, v >> 16);
        wr16(p, (a & 0xFFFF) + 2, v & 0xFFFF);
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
        uint8_t *p = s_wmap[a >> 16];
        if (p) {
            p[(a & 0xFFFF) ^ 1] = src[i];
        } else {
            m68k_write_memory_8(a, src[i]);
        }
    }
}

void mem_copy_from(uint8_t *dst, uint32_t a, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++, a++) {
        a &= 0xFFFFFF;
        const uint8_t *p = s_rmap[a >> 16];
        dst[i] = p ? p[(a & 0xFFFF) ^ 1] : (uint8_t)m68k_read_memory_8(a);
    }
}

/* ---- Without side effects: the debugger, the disassembler ---- */

uint16_t amiga_peek16(uint32_t a)
{
    a &= 0xFFFFFE;
    const uint8_t *p = s_rmap[a >> 16];
    return p ? rd16(p, a & 0xFFFF) : 0;
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
    uint8_t *p = s_wmap[a >> 16];
    if (p) {
        p[(a & 0xFFFF) ^ 1] = v;
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
