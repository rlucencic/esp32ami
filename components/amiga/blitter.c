/*
 * The blitter: area mode (all minterms, the A and B shifts, first and last
 * word masks, descending, inclusive and exclusive fill) and line mode.
 *
 * A blit is done the moment it starts; BBUSY stays set and the interrupt
 * waits for as long as the real one would take: its words times the
 * colour clocks a word needs with those channels, each a bus slot it gets
 * only where the line's other DMA leaves one (refresh, disk, audio,
 * sprites, the bitplanes: 4 hires planes leave none in the fetch window).
 * The copper's few slots aren't counted.
 */
#include <string.h>
#include "amiga_internal.h"
#include "jit.h"

blitter_t blit;

enum { A, B, C, D };

#define USEA    0x0800
#define USEB    0x0400
#define USEC    0x0200
#define USED    0x0100

#define LINE    0x0001
#define DESC    0x0002
#define FCI     0x0004
#define IFE     0x0008
#define EFE     0x0010

/* Colour clocks a word takes, by BLTCON0's USE bits (D, C, B, A) */
static const uint8_t s_word_cck[16] = { 2, 2, 2, 3, 2, 3, 2, 3, 2, 2, 2, 3, 2, 3, 3, 4 };

/* Fill: [exclusive][carry in][byte] -> the filled byte, bit 8 the carry out */
static uint16_t s_fill[2][2][256];

static void init_fill(void)
{
    for (int ex = 0; ex < 2; ex++) {
        for (int c = 0; c < 2; c++) {
            for (int b = 0; b < 256; b++) {
                int carry = c, out = 0;
                for (int i = 0; i < 8; i++) {
                    int bit = (b >> i) & 1;
                    carry ^= bit;
                    out |= (ex ? carry : (bit | carry)) << i;
                }
                s_fill[ex][c][b] = (uint16_t)(out | (carry << 8));
            }
        }
    }
}

static inline uint16_t minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c)
{
    switch (lf) {
    case 0x00: return 0;
    case 0xF0: return a;
    case 0xCC: return b;
    case 0xAA: return c;
    case 0xCA: return (a & b) | (~a & c);       /* cookie cut */
    case 0x0A: return ~a & c;
    case 0xFA: return a | c;
    case 0xEA: return (a & b) | c;
    case 0x5A: return a ^ c;
    case 0xFF: return 0xFFFF;
    default: {
        uint16_t d = 0;
        if (lf & 0x80) d |= a & b & c;
        if (lf & 0x40) d |= a & b & ~c;
        if (lf & 0x20) d |= a & ~b & c;
        if (lf & 0x10) d |= a & ~b & ~c;
        if (lf & 0x08) d |= ~a & b & c;
        if (lf & 0x04) d |= ~a & b & ~c;
        if (lf & 0x02) d |= ~a & ~b & c;
        if (lf & 0x01) d |= ~a & ~b & ~c;
        return d;
    }
    }
}

/* A-only, unshifted copy. Keep each read before its write: overlapping
 * regions follow the blitter's direction, including wrapped chip addresses. */
static int blit_copy(void)
{
    bool desc = blit.con1 & DESC;
    int inc = desc ? -2 : 2;
    int32_t amod = desc ? -blit.mod[A] : blit.mod[A];
    int32_t dmod = desc ? -blit.mod[D] : blit.mod[D];
    uint32_t apt = blit.pt[A], dpt = blit.pt[D];
    uint16_t adat = blit.dat[A], any = 0;
    uint32_t w = blit.width, h = blit.height;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            adat = chip_read16(apt);
            apt += inc;
            uint16_t d = adat;
            if (x == 0) {
                d &= blit.afwm;
            }
            if (x == w - 1) {
                d &= blit.alwm;
            }
            any |= d;
            chip_write16(dpt, d);
            dpt += inc;
        }
        apt += amod;
        dpt += dmod;
    }
    blit.pt[A] = apt;
    blit.pt[D] = dpt;
    blit.dat[A] = adat;
    blit.zero = any == 0;
    return (int)(w * h) * s_word_cck[9];
}

/* D alone with minterm $00 or $FF (no fill): every word the same, a row
 * at a time where it doesn't wrap chip RAM (Deluxe Galaga's clears: 42% of
 * its words in play) */
static int blit_const(void)
{
    bool desc = blit.con1 & DESC;
    uint16_t v = (blit.con0 & 0xFF) ? 0xFFFF : 0;
    int32_t inc = desc ? -2 : 2, dmod = desc ? -blit.mod[D] : blit.mod[D];
    uint32_t dpt = blit.pt[D], w = blit.width, h = blit.height, m = ami.chip_mask;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t a = dpt & m & ~1u, lo = desc ? a - 2 * (w - 1) : a;
        if (desc ? a >= 2 * (w - 1) : a + 2 * w <= m + 1) {
            memset(ami.chip + lo, v & 0xFF, 2 * w);         /* ($0000 or $FFFF: either byte order) */
            dpt += (uint32_t)inc * w;
        } else {
            for (uint32_t x = 0; x < w; x++) {
                chip_write16(dpt, v);
                dpt += (uint32_t)inc;
            }
        }
        dpt += (uint32_t)dmod;
    }
    blit.pt[D] = dpt;
    blit.zero = v == 0;
    return (int)(w * h) * s_word_cck[1];
}

/* A, B, C and D with minterm $CA (the cookie cut: A's mask picks B over
 * C), ascending, no fill: blit_area()'s steps without its tests (the
 * reads A, B, C, then D's write, word by word; the shifts' words carried
 * across rows as there) */
static int blit_cookie(void)
{
    int ash = blit.con0 >> 12, bsh = blit.con1 >> 12;
    uint32_t apt = blit.pt[A], bpt = blit.pt[B], cpt = blit.pt[C], dpt = blit.pt[D];
    int32_t amod = blit.mod[A], bmod = blit.mod[B], cmod = blit.mod[C], dmod = blit.mod[D];
    uint16_t adat = blit.dat[A], bdat = blit.dat[B], cdat = blit.dat[C], aold = 0, bold = 0, any = 0;
    uint16_t fw = blit.afwm, lw = blit.alwm;
    uint32_t w = blit.width, h = blit.height;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            adat = chip_read16(apt);
            bdat = chip_read16(bpt);
            cdat = chip_read16(cpt);
            apt += 2, bpt += 2, cpt += 2;
            uint16_t am = adat & (x == 0 ? fw : 0xFFFF) & (x == w - 1 ? lw : 0xFFFF);
            uint16_t a = (uint16_t)((((uint32_t)aold << 16) | am) >> ash);
            uint16_t b = (uint16_t)((((uint32_t)bold << 16) | bdat) >> bsh);
            aold = am;
            bold = bdat;
            uint16_t d = (uint16_t)((a & b) | (~a & cdat));
            any |= d;
            chip_write16(dpt, d);
            dpt += 2;
        }
        apt += (uint32_t)amod, bpt += (uint32_t)bmod, cpt += (uint32_t)cmod, dpt += (uint32_t)dmod;
    }
    blit.pt[A] = apt, blit.pt[B] = bpt, blit.pt[C] = cpt, blit.pt[D] = dpt;
    blit.dat[A] = adat, blit.dat[B] = bdat, blit.dat[C] = cdat;
    blit.zero = any == 0;
    return (int)(w * h) * s_word_cck[15];
}

#ifndef ESP_PLATFORM
bool blit_generic;                          /* (host/blit_test.c: blit_area()'s own loop alone) */
#endif

static int blit_area(void)
{
    uint16_t con0 = blit.con0, con1 = blit.con1;
#ifndef ESP_PLATFORM
    if (!blit_generic)
#endif
    {
        if (con0 == (USEA | USED | 0xF0) && !(con1 & (LINE | IFE | EFE))) {
            return blit_copy();
        }
        if ((con0 & 0x0F00) == USED && ((con0 & 0xFF) == 0x00 || (con0 & 0xFF) == 0xFF) && !(con1 & (IFE | EFE))) {
            return blit_const();
        }
        if ((con0 & 0x0FFF) == (USEA | USEB | USEC | USED | 0xCA) && !(con1 & (DESC | IFE | EFE))) {
            return blit_cookie();
        }
    }
    bool desc = con1 & DESC;
    int ash = con0 >> 12, bsh = con1 >> 12;
    uint8_t lf = con0 & 0xFF;
    bool fill = (con1 & (IFE | EFE)) != 0;
    int ex = (con1 & EFE) ? 1 : 0;
    int inc = desc ? -2 : 2;
    int32_t mod[4];
    for (int i = 0; i < 4; i++) {
        mod[i] = desc ? -blit.mod[i] : blit.mod[i];
    }
    uint32_t apt = blit.pt[A], bpt = blit.pt[B], cpt = blit.pt[C], dpt = blit.pt[D];
    uint16_t adat = blit.dat[A], bdat = blit.dat[B], cdat = blit.dat[C];
    uint16_t aold = 0, bold = 0;
    uint32_t w = blit.width, h = blit.height;
    bool zero = true;

    for (uint32_t y = 0; y < h; y++) {
        int carry = (con1 & FCI) ? 1 : 0;
        for (uint32_t x = 0; x < w; x++) {
            if (con0 & USEA) {
                adat = chip_read16(apt);
                apt += inc;
            }
            if (con0 & USEB) {
                bdat = chip_read16(bpt);
                bpt += inc;
            }
            if (con0 & USEC) {
                cdat = chip_read16(cpt);
                cpt += inc;
            }
            uint16_t am = adat;
            if (x == 0) {
                am &= blit.afwm;
            }
            if (x == w - 1) {
                am &= blit.alwm;
            }
            uint16_t a, b;
            if (!desc) {
                a = (uint16_t)((((uint32_t)aold << 16) | am) >> ash);
                b = (uint16_t)((((uint32_t)bold << 16) | bdat) >> bsh);
            } else {
                a = (uint16_t)((((uint32_t)am << 16) | aold) >> (16 - ash));
                b = (uint16_t)((((uint32_t)bdat << 16) | bold) >> (16 - bsh));
            }
            aold = am;
            bold = bdat;
            uint16_t d = minterm(lf, a, b, cdat);
            if (fill) {
                uint16_t lo = s_fill[ex][carry][d & 0xFF];
                uint16_t hi = s_fill[ex][lo >> 8][d >> 8];
                carry = hi >> 8;
                d = (uint16_t)((lo & 0xFF) | ((hi & 0xFF) << 8));
            }
            if (d) {
                zero = false;
            }
            if (con0 & USED) {
                chip_write16(dpt, d);
                dpt += inc;
            }
        }
        if (con0 & USEA) {
            apt += mod[A];
        }
        if (con0 & USEB) {
            bpt += mod[B];
        }
        if (con0 & USEC) {
            cpt += mod[C];
        }
        if (con0 & USED) {
            dpt += mod[D];
        }
    }
    blit.pt[A] = apt;
    blit.pt[B] = bpt;
    blit.pt[C] = cpt;
    blit.pt[D] = dpt;
    blit.dat[A] = adat;
    blit.dat[B] = bdat;
    blit.dat[C] = cdat;
    blit.zero = zero;
    return (int)(w * h) * s_word_cck[(con0 >> 8) & 0xF];
}

/*
 * Line mode: a pixel a step along the major axis, a step along the minor
 * one too while the error term (BLTAPTL) isn't negative. BLTCON1's octant
 * bits: SUD (bit 4) the minor steps are vertical (the major axis is x),
 * SUL (bit 3) they go up or left, AUL (bit 2) the major ones do.
 */
static int blit_line(void)
{
    uint16_t con0 = blit.con0, con1 = blit.con1;
    uint8_t lf = con0 & 0xFF;
    int ash = con0 >> 12, bsh = con1 >> 12;
    bool sud = con1 & 0x10, sul = con1 & 0x08, aul = con1 & 0x04;
    bool sing = con1 & 0x02;
    bool sign = con1 & 0x40;
    int16_t err = (int16_t)(blit.pt[A] & 0xFFFF);
    uint32_t cpt = blit.pt[C], dpt = blit.pt[D];
    int32_t row = blit.mod[C];
    bool dotted = false, zero = true;

    for (uint32_t i = 0; i < blit.height; i++) {
        uint16_t a = (uint16_t)(blit.dat[A] >> ash);
        uint16_t b = (blit.dat[B] & (0x8000 >> bsh)) ? 0xFFFF : 0;
        uint16_t c = chip_read16(cpt);
        uint16_t d = minterm(lf, a, b, c);
        if (d) {
            zero = false;
        }
        if (!sing || !dotted) {
            chip_write16(dpt, d);
            dotted = true;
        }
        bsh = (bsh + 1) & 15;

        bool minor = !sign;
        err = (int16_t)(err + (sign ? blit.mod[B] : blit.mod[A]));
        sign = err < 0;
        int dx = 0, dy = 0;
        if (sud) {
            dx = aul ? -1 : 1;
            if (minor) {
                dy = sul ? -1 : 1;
            }
        } else {
            dy = aul ? -1 : 1;
            if (minor) {
                dx = sul ? -1 : 1;
            }
        }
        if (dx > 0 && ++ash == 16) {
            ash = 0;
            cpt += 2;
        }
        if (dx < 0 && --ash < 0) {
            ash = 15;
            cpt -= 2;
        }
        if (dy) {
            cpt += dy < 0 ? -row : row;
            dotted = false;
        }
        dpt = cpt;                  /* (the first pixel goes to BLTDPT, the rest follow C) */
    }
    blit.pt[A] = (blit.pt[A] & 0xFFFF0000) | (uint16_t)err;
    blit.pt[C] = blit.pt[D] = cpt;
    blit.con0 = (uint16_t)((con0 & 0x0FFF) | (ash << 12));
    blit.con1 = (uint16_t)((con1 & 0x0FBF) | (bsh << 12) | (sign ? 0x40 : 0));
    blit.zero = zero;
    return (int)blit.height * 4;
}

/* The slots of this line the blitter may have: its colour clocks less
 * refresh (4), disk (3), audio (one a channel) and the display's
 */
static int line_free(void)
{
    uint16_t dma = (ami.dmacon & DMA_EN) ? ami.dmacon : 0;
    int free = ami_line_cck(ami.line) - 4 - ((dma & DMA_DSK) ? 3 : 0) - __builtin_popcount(dma & DMA_AUD) -
               display_dma_slots();
    return free > 0 ? free : 0;
}

/* Its slots from `from` on, the rest of this line's free ones spread over
 * the line: done in it (the end), or the rest in the next lines
 */
static void plan_end(int from)
{
    int len = 2 * ami_line_cck(ami.line), line_end = ami.line_start + len;
    int span = line_end - from;
    int avail = span > 0 ? (int)((int64_t)line_free() * span / len) : 0;
    if (blit.left <= avail) {
        blit.end = from + (avail > 0 ? (int)((int64_t)blit.left * span / avail) : 0);
        blit.left = 0;
    } else {
        blit.left -= avail;
        blit.end = line_end + 2 * blit.left;    /* (at the earliest: the next lines say) */
    }
}

#ifndef ESP_PLATFORM
/* (host/blit_test.c) the blit set up in `blit` done, its colour clocks */
int blitter_test_run(void)
{
    return (blit.con1 & LINE) ? blit_line() : blit_area();
}
#endif

void blitter_line_start(void)
{
    if (blit.busy && blit.left > 0) {
        plan_end(ami.line_start);
    }
}

#ifndef ESP_PLATFORM
/* (AMI_BLITSTAT) the blits by kind: BLTCON0's channels and minterm, a shift
 * of A or B, descending, fill, line; their number and words */
struct blit_stat { uint32_t key; unsigned long n, words; } blit_stat[256];
static void blit_count(void)
{
    uint16_t c0 = blit.con0, c1 = blit.con1;
    uint32_t key = (c1 & LINE) ? 0x10000u | (c0 & 0xFFF) :
                   (uint32_t)(c0 & 0xFFF) | ((c0 >> 12) ? 0x1000u : 0) | ((c1 >> 12) ? 0x2000u : 0) |
                   ((c1 & DESC) ? 0x4000u : 0) | ((c1 & (IFE | EFE)) ? 0x8000u : 0);
    uint32_t h = (key * 2654435761u) >> 24;
    for (int i = 0; i < 256; i++, h = (h + 1) & 255) {
        if (!blit_stat[h].n || blit_stat[h].key == key) {
            blit_stat[h].key = key;
            blit_stat[h].n++;
            blit_stat[h].words += (c1 & LINE) ? blit.height : blit.width * blit.height;
            return;
        }
    }
}
#endif

static void start(int at)
{
    blit.pending = false;
#ifndef ESP_PLATFORM
    blit_count();
#endif
    uint32_t d0 = blit.pt[D];
    int cck = (blit.con1 & LINE) ? blit_line() : blit_area();
#ifdef AMI_JIT
    {
        /* the 68EC020's translated code in what D wrote goes: between the
         * pointer's start and end, a row's width and modulo either side */
        uint32_t d1 = blit.pt[D], m = ami.chip_mask;
        int32_t mod = blit.mod[D] < 0 ? -blit.mod[D] : blit.mod[D];
        int32_t margin = 2 * (int32_t)blit.width + mod + 4;
        int32_t lo = (int32_t)((d0 < d1 ? d0 : d1) & m) - margin, hi = (int32_t)((d0 < d1 ? d1 : d0) & m) + margin;
        if ((d0 & m) > (d1 & m) ? (d0 & m) - (d1 & m) > m / 2 : (d1 & m) - (d0 & m) > m / 2) {
            lo = 0;                     /* (round chip RAM's end) */
            hi = (int32_t)m + 1;
        }
        JIT_WRITTEN(lo < 0 ? 0 : (uint32_t)lo, (uint32_t)hi);
    }
#endif
    blit.busy = true;
    bus_blit_started();
    blit.left = cck;
    plan_end(at + 8);
    /* The 68EC020's slots it takes on average: those its channels use (a
     * line: C and D), all of them with BLTPRI ("nasty"), else it gets one
     * in four it waits for (the 68000 waits access by access: mem.c)
     */
    int used = (blit.con1 & LINE) ? 2 * (int)blit.height
                                  : (int)(blit.width * blit.height) * __builtin_popcount((blit.con0 >> 8) & 0xF);
    int steal = used * SLOT_WAIT;
    if (!(ami.dmacon & DMA_BLTPRI)) {
        steal = steal * 3 / 4;
    }
    blit.steal = ami.cpu_shift ? steal : 0;
    blit.steal_line = cck > 0 ? (int)((int64_t)steal * CYCLES_LINE / (cck * 2)) : steal;
    ami_cpu_wake();
}

int blitter_steal_line(void)
{
    if (blit.steal <= 0) {
        return 0;
    }
    int n = blit.steal < blit.steal_line ? blit.steal : blit.steal_line;
    n = n > 0 ? n : blit.steal;
    blit.steal -= n;
    return n;
}

static bool dma_on(void)
{
    return (ami.dmacon & (DMA_EN | DMA_BLT)) == (DMA_EN | DMA_BLT);
}

static void begin(int at)
{
    if (dma_on()) {
        start(at);
    } else {
        blit.pending = true;
    }
}

void blitter_dma_changed(int at)
{
    if (blit.pending && dma_on()) {
        start(at);
    }
}

void blitter_finish(void)
{
    blit.busy = false;
    ami_set_intreq(INT_BLIT);
}

bool blitter_busy(int at)
{
    return blit.pending || (blit.busy && at < blit.end);
}

static uint32_t set_hi(uint32_t pt, uint16_t v)
{
    return (pt & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
}

static uint32_t set_lo(uint32_t pt, uint16_t v)
{
    return (pt & 0xFFFF0000) | (v & 0xFFFE);
}

void blitter_write(uint32_t reg, uint16_t v, int at)
{
    switch (reg) {
    case 0x040: blit.con0 = v; break;
    case 0x042: blit.con1 = v; break;
    case 0x044: blit.afwm = v; break;
    case 0x046: blit.alwm = v; break;
    case 0x048: blit.pt[C] = set_hi(blit.pt[C], v); break;
    case 0x04A: blit.pt[C] = set_lo(blit.pt[C], v); break;
    case 0x04C: blit.pt[B] = set_hi(blit.pt[B], v); break;
    case 0x04E: blit.pt[B] = set_lo(blit.pt[B], v); break;
    case 0x050: blit.pt[A] = set_hi(blit.pt[A], v); break;
    case 0x052: blit.pt[A] = (blit.pt[A] & 0xFFFF0000) | v; break;   /* (line mode's error term: all 16 bits) */
    case 0x054: blit.pt[D] = set_hi(blit.pt[D], v); break;
    case 0x056: blit.pt[D] = set_lo(blit.pt[D], v); break;
    case 0x058:                     /* BLTSIZE: height (10 bits), width in words (6) */
        blit.height = (v >> 6) ? (v >> 6) : 1024;
        blit.width = (v & 0x3F) ? (v & 0x3F) : 64;
        begin(at);
        break;
    case 0x05A:                     /* BLTCON0L (ECS) */
        if (ami.ecs) {
            blit.con0 = (blit.con0 & 0xFF00) | (v & 0xFF);
        }
        break;
    case 0x05C:                     /* BLTSIZV (ECS) */
        if (ami.ecs) {
            blit.height = (v & 0x7FFF) ? (v & 0x7FFF) : 0x8000;
        }
        break;
    case 0x05E:                     /* BLTSIZH (ECS): starts it */
        if (ami.ecs) {
            blit.width = (v & 0x7FF) ? (v & 0x7FF) : 0x800;
            begin(at);
        }
        break;
    case 0x060: blit.mod[C] = (int16_t)(v & 0xFFFE); break;
    case 0x062: blit.mod[B] = (int16_t)(v & 0xFFFE); break;
    case 0x064: blit.mod[A] = (int16_t)(v & 0xFFFE); break;
    case 0x066: blit.mod[D] = (int16_t)(v & 0xFFFE); break;
    case 0x070: blit.dat[C] = v; break;
    case 0x072: blit.dat[B] = v; break;
    case 0x074: blit.dat[A] = v; break;
    default: break;
    }
}

void blitter_reset(void)
{
    static bool tables;
    if (!tables) {
        init_fill();
        tables = true;
    }
    blit = (blitter_t){ .afwm = 0xFFFF, .alwm = 0xFFFF, .end = NEVER };
}
