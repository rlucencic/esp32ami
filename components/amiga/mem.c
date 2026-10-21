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
 *   $DC0000-$DCFFFF  the battery-backed clock (rtc.c)
 *   $DFF000-$DFFFFF  the custom chips (registers mirrored every $200)
 *   $E80000-$E8FFFF  autoconfig: the next board to configure
 *   $F80000-$FFFFFF  Kickstart (256 KB ones mirrored)
 *
 * No bus errors: an Amiga 500's empty space just reads nothing (0 here).
 */
#include <string.h>
#include "m68k.h"
#include "amiga_internal.h"

static const uint8_t *s_rmap[256];
static uint8_t *s_wmap[256];

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
    for (uint32_t b = 0x00; b < 0x20; b++) {
        uint8_t *p = ami.chip + ((b << 16) & (ami.chip_size - 1));
        s_rmap[b] = p;
        s_wmap[b] = p;
    }
    if (ami.ovl) {
        for (uint32_t b = 0x00; b < 0x08; b++) {
            s_rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
        }
    }
    for (uint32_t b = 0; b < (ami.slow_size >> 16); b++) {
        s_rmap[0xC0 + b] = s_wmap[0xC0 + b] = ami.slow + (b << 16);
    }
    for (uint32_t b = 0; ami.fast_base && b < (ami.fast_size >> 16); b++) {
        s_rmap[(ami.fast_base >> 16) + b] = s_wmap[(ami.fast_base >> 16) + b] = ami.fast + (b << 16);
    }
    for (uint32_t b = 0xF8; b < 0x100; b++) {
        s_rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
    }
}

uint16_t chip_read16(uint32_t a)
{
    return rd16(ami.chip, a & ami.chip_mask & ~1u);
}

void chip_write16(uint32_t a, uint16_t v)
{
    wr16(ami.chip, a & ami.chip_mask & ~1u, v);
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

static uint16_t slow_read16(uint32_t a)
{
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
    return 0;
}

static uint8_t slow_read8(uint32_t a)
{
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
    return 0;
}

static void slow_write16(uint32_t a, uint16_t v)
{
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
    }
}

static void slow_write8(uint32_t a, uint8_t v)
{
    if (is_custom(a)) {
        /* the byte on both halves of the bus: a word to the chips */
        custom_write(a & 0x1FE, (v << 8) | v, ami_now(), false);
    } else if (is_cia(a)) {
        cia_write8(a, v);
    } else if (is_rtc(a)) {
        rtc_write((a >> 2) & 15, v);
    } else if (is_autoconf(a)) {
        autoconf_write8(a, v);
    }
}

/* ---- Musashi's callbacks ---- */

unsigned int AMI_HOT m68k_read_memory_8(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (p) {
        return p[(a & 0xFFFF) ^ 1];
    }
    return slow_read8(a);
}

unsigned int AMI_HOT m68k_read_memory_16(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (p) {
        return rd16(p, a & 0xFFFF);
    }
    return slow_read16(a);
}

unsigned int AMI_HOT m68k_read_memory_32(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        return ((uint32_t)rd16(p, a & 0xFFFF) << 16) | rd16(p, (a & 0xFFFF) + 2);
    }
    uint32_t hi = m68k_read_memory_16(a);
    return (hi << 16) | m68k_read_memory_16((a + 2) & 0xFFFFFF);
}

void AMI_HOT m68k_write_memory_8(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
    if (p) {
        p[(a & 0xFFFF) ^ 1] = v;
        return;
    }
    slow_write8(a, v);
}

void AMI_HOT m68k_write_memory_16(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
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
        wr16(p, a & 0xFFFF, v >> 16);
        wr16(p, (a & 0xFFFF) + 2, v & 0xFFFF);
        return;
    }
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16((a + 2) & 0xFFFFFF, v & 0xFFFF);
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
