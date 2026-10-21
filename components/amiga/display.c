/*
 * Agnus's bitplane DMA and Denise: the picture.
 *
 * The line's bitplanes are fetched at DDFSTRT's position (an event of the
 * frame loop) with the registers as they are then. At the line's end what
 * Denise needs to draw it goes into the line's record: the fetched words,
 * BPLCON0-2, the window, the colours as the line began and each colour
 * write with the beam's position. Three frames of records (the
 * presenter's, the last one finished, the next); a record that would be
 * the same as the one already there isn't written again.
 *
 * The presenter (core 0) draws the records: the planes turned into pixel
 * indices through a table (odd and even planes apart: their scroll delays
 * differ), the colours changed at the pixels the beam was at, the sprites
 * over them as BPLCON2's priorities say.
 *
 * Sprites: their DMA fetches the control words (SPRxPOS, SPRxCTL) on the
 * first line after vertical blank and at each sprite's VSTOP, the data
 * words on the lines from VSTART; written directly, writing DATA arms a
 * sprite and CTL disarms it. Lores pixels, the display window's
 * horizontal coordinates; an odd sprite with ATT set adds its two bits to
 * its even partner's (15 colours).
 *
 * The picture: lores pixels 108-467 (hires 216-935) of lines 29-312, in
 * hires pixels (lores ones doubled), its lines doubled. Interlaced (LACE
 * in BPLCON0 during the frame, LOF then toggles): each field has its own
 * frames, and the presenter weaves them, the long field's lines even, the
 * short one's odd, once both have been drawn.
 *
 * ECS super-hires (SHRES in BPLCON0, up to 2 planes, 4 pixels a lores
 * one): a frame with such lines is drawn in super-hires pixels (1440
 * wide), the other lines' pixels doubled. Its 4 colours are registers 0,
 * 5, 10 and 15, each gun the mean of its nibble's two halves (the way
 * graphics.library sets them up: register 4a + b holds colour a's upper 2
 * bits in the lower half and b's in the upper one); the sprites' are
 * 16, 21, 26 and 31, at their lores positions.
 */
#include <stdlib.h>
#include <string.h>
#include "amiga_internal.h"

#define FIRST_LORES     108
#define PIC_W           AMIGA_SCREEN_WIDTH
#define PIC_LINES       AMIGA_SCREEN_LINES
#define FIRST_LINE_PAL  29
#define FIRST_LINE_NTSC 21
#define MAX_CHANGES     160
#define MAX_WORDS       200             /* planes x words: 6 x 25 lores, 4 x 50 hires, 2 x 100 super-hires */
#define WIDE_W          (2 * PIC_W)     /* a frame with super-hires lines */
/* The first fetched pixel after DDFSTRT, in lores pixels */
#define DELAY_LORES     17
#define DELAY_HIRES     9
#define DELAY_SHRES     5

#define BPLCON0_HIRES   0x8000
#define BPLCON0_SHRES   0x0040          /* ECS (masked off with OCS) */
#define BPLCON0_HAM     0x0800
#define BPLCON0_DPF     0x0400
#define BPLCON2_PF2PRI  0x0040

/* A line's record: what Denise draws it from */
typedef struct {
    uint32_t key;                   /* a hash of the rest (0: nothing yet) */
    uint16_t con0, con1, con2;
    int16_t first;                  /* the first fetched pixel, lores */
    int16_t x0, x1;                 /* the window, in the picture's pixels */
    uint8_t planes, words;          /* planes 0: no bitplanes on this line */
    uint16_t changes;
    uint8_t sprites;                /* shown on this line */
    uint8_t spr_num[8];             /* the sprite (bit 7: attached to the one before) */
    int16_t spr_x[8];               /* lores pixels from the picture's left edge */
    uint16_t spr_a[8], spr_b[8];    /* DATA, DATB */
    uint16_t color[32];             /* as the line began */
    uint32_t change[MAX_CHANGES];   /* x << 20 | colour << 12 | value */
    uint16_t data[MAX_WORDS];       /* plane after plane */
} row_t;

typedef struct {
    uint32_t pt;
    uint16_t pos, ctl, data, datb;
    bool armed;                     /* its data words show on this line */
    uint8_t dma;                    /* 0 idle, 1 waiting for VSTART, 2 showing */
} sprite_t;

static struct {
    /* registers */
    uint16_t bplcon0, bplcon1, bplcon2, bplcon3;
    sprite_t spr[8];
    uint16_t diwstrt, diwstop, ddfstrt, ddfstop;
    uint16_t diwhigh;               /* ECS: the window's high bits, if written since DIWSTRT/DIWSTOP */
    bool diwhigh_set;
    int16_t mod[2];
    uint32_t pt[6];
    bool vwin;

    /* this line */
    int fetch_at;
    bool fetched;
    uint16_t con0, con1, con2;
    int first, planes, words;
    uint32_t data_key;
    uint16_t data[MAX_WORDS];
    int changes;
    uint32_t change[MAX_CHANGES];
    uint32_t late;                  /* colours changed after the log filled */
    uint16_t late_value[32];
    uint16_t color[32] __attribute__((aligned(4)));     /* 12-bit, as the line began */

    /* collisions */
    uint16_t clxcon, clxdat;
    int clx_frames;                 /* worked out while > 0 (a CLXDAT read sets it) */

    /* the frames: each field's three (an interlaced picture's short
     * frames go to field 1, everything else to field 0)
     */
    row_t *rows;
    uint32_t signature[2][3];
    bool lace[2][3];                /* drawn with BPLCON0's LACE on */
    bool shres[2][3];               /* with super-hires lines */
    uint32_t hash;
    uint32_t state[2];              /* latest | pinned << 2 */
    int writing[2];
    int field;                      /* the one this frame draws */
    bool lace_seen;                 /* LACE on at some point in this frame */
    bool shres_seen;                /* a super-hires line in this frame */
    bool field1;                    /* field 1 drawn since interlace began */
    int first_line;
} s;

static uint16_t s_rgb565[4096];
static uint64_t s_p2c[256];
static uint8_t s_pf1[64], s_pf2[64];
static uint8_t s_shres_gun[16];     /* super-hires: a nibble's two halves' mean, 4 bits */

#define LATEST(st)      ((st) & 3u)
#define PINNED(st)      (((st) >> 2) & 3u)

size_t amiga_picture_size(void)
{
    return 6 * PIC_LINES * sizeof(row_t);
}

#define BPLCON0_LACE    0x0004

static row_t *buffer(int field, int b)
{
    return s.rows + (field * 3 + b) * PIC_LINES;
}

void display_init(void *picture)
{
    for (int c = 0; c < 4096; c++) {
        int r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
        s_rgb565[c] = (uint16_t)(((r * 31 / 15) << 11) | ((g * 63 / 15) << 5) | (b * 31 / 15));
    }
    for (int b = 0; b < 256; b++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) {
            v |= (uint64_t)((b >> (7 - k)) & 1) << (8 * k);
        }
        s_p2c[b] = v;
    }
    for (int n = 0; n < 16; n++) {
        s_shres_gun[n] = (uint8_t)(((n >> 2) + (n & 3)) * 5 / 2);
    }
    for (int v = 0; v < 64; v++) {
        s_pf1[v] = (uint8_t)((v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4));
        s_pf2[v] = (uint8_t)(((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4));
    }
    if (picture == NULL) {
        picture = calloc(1, amiga_picture_size());
    }
    s.rows = picture;
}

void display_reset(void)
{
    row_t *rows = s.rows;
    memset(&s, 0, sizeof(s));
    s.rows = rows;
    memset(s.rows, 0, amiga_picture_size());
    s.fetch_at = NEVER;
    s.writing[0] = s.writing[1] = 1;
    s.first_line = ami.ntsc ? FIRST_LINE_NTSC : FIRST_LINE_PAL;
    s.hash = 0x811C9DC5u;
}

/* ---- Bitplane DMA ---- */

static bool dma_on(void)
{
    return (ami.dmacon & (DMA_EN | DMA_BPL)) == (DMA_EN | DMA_BPL);
}

static int planes_of(uint16_t con0)
{
    int n = (con0 >> 12) & 7;
    if (n > 6) {
        n = 4;
    }
    if ((con0 & BPLCON0_SHRES) && n > 2) {
        n = 2;
    } else if ((con0 & BPLCON0_HIRES) && n > 4) {
        n = 4;
    }
    return n;
}

static void ddf(int *start, int *stop)
{
    *start = s.ddfstrt < 0x18 ? 0x18 : s.ddfstrt;
    *stop = s.ddfstop > 0xD8 ? 0xD8 : s.ddfstop;
}

/* When this line's fetch happens (none if its start has passed) */
static void plan(int at)
{
    s.fetch_at = NEVER;
    int vblank_end = ami_ntsc_beam() ? VBLANK_END_NTSC : VBLANK_END_PAL;
    if (s.fetched || !dma_on() || !s.vwin || ami.line < vblank_end || planes_of(s.bplcon0) == 0) {
        return;
    }
    int start, stop;
    ddf(&start, &stop);
    if (stop < start) {
        return;
    }
    int t = ami.line_start + start * 2;
    if (t >= at) {
        s.fetch_at = t;
    }
}

int display_fetch_at(void)
{
    return s.fetch_at;
}

void display_dma_changed(int at)
{
    if (!s.fetched) {
        plan(at);
        ami_cpu_wake();
    }
}

/* The display window: OCS's VSTOP has V8 = !V7 and HSTOP H8 set; ECS's
 * DIWHIGH, written after DIWSTRT and DIWSTOP, gives those bits instead
 */
static void diw(int *vstart, int *vstop, int *hstart, int *hstop)
{
    if (s.diwhigh_set) {
        *vstart = (s.diwstrt >> 8) | (s.diwhigh & 7) << 8;
        *vstop = (s.diwstop >> 8) | ((s.diwhigh >> 8) & 7) << 8;
        *hstart = (s.diwstrt & 0xFF) | ((s.diwhigh & 0x0020) ? 0x100 : 0);
        *hstop = (s.diwstop & 0xFF) | ((s.diwhigh & 0x2000) ? 0x100 : 0);
    } else {
        *vstart = s.diwstrt >> 8;
        *vstop = (s.diwstop >> 8) | ((s.diwstop & 0x8000) ? 0 : 0x100);
        *hstart = s.diwstrt & 0xFF;
        *hstop = (s.diwstop & 0xFF) | 0x100;
    }
}

static int spr_vstart(const sprite_t *sp)
{
    return (sp->pos >> 8) | ((sp->ctl & 4) << 6);
}

static int spr_vstop(const sprite_t *sp)
{
    return (sp->ctl >> 8) | ((sp->ctl & 2) << 7);
}

static void spr_fetch_control(sprite_t *sp)
{
    sp->pos = chip_read16(sp->pt);
    sp->ctl = chip_read16(sp->pt + 2);
    sp->pt += 4;
    sp->armed = false;
}

/* The sprites' DMA for this line */
static void sprite_dma(void)
{
    int first = (ami_ntsc_beam() ? VBLANK_END_NTSC : VBLANK_END_PAL) - 1;
    bool dma = (ami.dmacon & (DMA_EN | DMA_SPR)) == (DMA_EN | DMA_SPR);
    for (int i = 0; i < 8; i++) {
        sprite_t *sp = &s.spr[i];
        if (ami.line == 0) {
            sp->dma = 0;
        }
        if (!dma || ami.line < first) {
            continue;
        }
        if (ami.line == first) {
            spr_fetch_control(sp);
            sp->dma = 1;
            continue;
        }
        if (sp->dma == 0) {
            continue;
        }
        sp->armed = false;
        if (sp->dma == 1 && ami.line == spr_vstart(sp)) {
            sp->dma = 2;
        }
        if (sp->dma == 2) {
            if (ami.line == spr_vstop(sp)) {
                spr_fetch_control(sp);
                sp->dma = 1;
            } else {
                sp->data = chip_read16(sp->pt);
                sp->datb = chip_read16(sp->pt + 2);
                sp->pt += 4;
                sp->armed = true;
            }
        }
    }
}

void display_line_start(void)
{
    s.fetched = false;
    s.changes = 0;
    int vstart, vstop, hstart, hstop;
    diw(&vstart, &vstop, &hstart, &hstop);
    if (ami.line == 0) {
        s.vwin = false;
    }
    if (ami.line == vstart) {
        s.vwin = true;
    }
    if (ami.line == vstop) {
        s.vwin = false;
    }
    plan(ami.line_start);
    sprite_dma();
}

AMI_HOT void display_fetch(void)
{
    s.fetch_at = NEVER;
    s.fetched = true;
    s.con0 = s.bplcon0;
    s.con1 = s.bplcon1;
    s.con2 = s.bplcon2;
    bool shres = s.con0 & BPLCON0_SHRES;
    bool hires = !shres && (s.con0 & BPLCON0_HIRES);
    int planes = planes_of(s.con0);
    int start, stop;
    ddf(&start, &stop);
    /* whole fetch units of 8 colour clocks, the one DDFSTOP falls in too
     * (Kickstart 1.3's hires screens: $3C-$D0 is 40 words; 3.1's $40-$D0 38)
     */
    int words = ((stop - start + 7) / 8 + 1) * (shres ? 4 : hires ? 2 : 1);
    s.first = start * 2 + (shres ? DELAY_SHRES : hires ? DELAY_HIRES : DELAY_LORES);
    s.planes = planes;
    s.words = words;

    uint32_t key = 0x811C9DC5u;
    uint16_t *d = s.data;
    for (int p = 0; p < planes; p++) {
        uint32_t ptr = s.pt[p];
        for (int w = 0; w < words; w++) {
            uint16_t v = chip_read16(ptr);
            ptr += 2;
            key = (key ^ v) * 0x01000193u;
            *d++ = v;
        }
        s.pt[p] = ptr + s.mod[p & 1];
    }
    s.data_key = key;
    /* lores planes 5-6, hires planes 3-4 and super-hires plane 2 fetch in
     * the 68000's (even) slots: a slot a word
     */
    int even = planes - (shres ? 1 : hires ? 2 : 4);
    if (even > 0) {
        ami.stall += even * words * SLOT_WAIT;
    }
}

/* ---- Registers ---- */

static void log_color(int reg, uint16_t v, int at)
{
    int h = at / 2 - ami.line * CCK_LINE;
    int x = 4 * h + 6 - 2 * FIRST_LORES;
    x = x < 0 ? 0 : x > PIC_W ? PIC_W : x;
    v &= 0xFFF;
    if (s.changes == MAX_CHANGES) {
        s.late |= 1u << reg;
        s.late_value[reg] = v;
        return;
    }
    /* where it goes (the copper's writes can be ahead of the CPU's) */
    int i = s.changes;
    while (i > 0 && (s.change[i - 1] >> 20) > (uint32_t)x) {
        i--;
    }
    /* the same colour again changes nothing */
    uint16_t now = s.color[reg];
    for (int j = i - 1; j >= 0; j--) {
        if (((s.change[j] >> 12) & 31) == (uint32_t)reg) {
            now = s.change[j] & 0xFFF;
            break;
        }
    }
    if (now == v) {
        return;
    }
    memmove(&s.change[i + 1], &s.change[i], (s.changes - i) * sizeof(uint32_t));
    s.change[i] = (uint32_t)x << 20 | (uint32_t)reg << 12 | v;
    s.changes++;
}

void display_write(uint32_t reg, uint16_t v, int at)
{
    if (reg >= 0x180 && reg <= 0x1BE) {
        log_color((reg - 0x180) >> 1, v, at);
        return;
    }
    if (reg >= 0x120 && reg <= 0x13E) {
        sprite_t *sp = &s.spr[(reg - 0x120) >> 2];
        if (reg & 2) {
            sp->pt = (sp->pt & 0xFFFF0000) | (v & 0xFFFE);
        } else {
            sp->pt = (sp->pt & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        }
        return;
    }
    if (reg >= 0x140 && reg <= 0x17E) {
        sprite_t *sp = &s.spr[(reg - 0x140) >> 3];
        switch (reg & 6) {
        case 0: sp->pos = v; break;
        case 2: sp->ctl = v; sp->armed = false; break;
        case 4: sp->data = v; sp->armed = true; break;
        default: sp->datb = v; break;
        }
        return;
    }
    if (reg >= 0x0E0 && reg <= 0x0F6) {
        int p = (reg - 0x0E0) >> 2;
        if (reg & 2) {
            s.pt[p] = (s.pt[p] & 0xFFFF0000) | (v & 0xFFFE);
        } else {
            s.pt[p] = (s.pt[p] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        }
        return;
    }
    switch (reg) {
    case 0x08E:
        s.diwstrt = v;
        s.diwhigh_set = false;
        break;
    case 0x090:
        s.diwstop = v;
        s.diwhigh_set = false;
        break;
    case 0x1E4:
        if (ami.ecs) {
            s.diwhigh = v;
            s.diwhigh_set = true;
        }
        break;
    case 0x092:
        s.ddfstrt = v & 0xFC;
        display_dma_changed(at);
        break;
    case 0x094:
        s.ddfstop = v & 0xFC;
        break;
    case 0x100:
        s.bplcon0 = ami.ecs ? v : v & ~BPLCON0_SHRES;
        s.lace_seen |= (v & BPLCON0_LACE) != 0;
        display_dma_changed(at);
        break;
    case 0x102:
        s.bplcon1 = v;
        break;
    case 0x104:
        s.bplcon2 = v;
        break;
    case 0x106:
        s.bplcon3 = v;
        break;
    case 0x108:
        s.mod[0] = (int16_t)(v & 0xFFFE);
        break;
    case 0x10A:
        s.mod[1] = (int16_t)(v & 0xFFFE);
        break;
    default:
        break;
    }
}

/* ---- Collisions (CLXDAT) ----
 *
 * Worked out at each line's end while a program reads CLXDAT (for a few
 * frames after each read: it costs time, and most never look): the armed
 * sprites against each other and against the playfields, the odd planes
 * against the even ones, inside the display window. A playfield matches
 * where its planes that CLXCON enables (ENBP) have the values it asks for
 * (MVBP: a plane not fetched is 0); a sprite pair is its even sprite, the
 * odd one too with ENSP. Hires pixels count for the sprites' lores pixel
 * they're in.
 */
#define CLX_FRAMES      4

uint16_t display_clxdat(void)
{
    uint16_t v = s.clxdat | 0x8000;
    s.clxdat = 0;
    s.clx_frames = CLX_FRAMES;
    return v;
}

void display_clxcon(uint16_t v)
{
    s.clxcon = v;
}

/* 16 pixels of plane p's line from pixel idx on (outside it, or a plane
 * not fetched: 0)
 */
static uint16_t plane16(int p, int planes, int idx)
{
    int n = s.words * 16;
    if (p >= planes || idx <= -16 || idx >= n) {
        return 0;
    }
    const uint16_t *d = s.data + p * s.words;
    int w = idx >= 0 ? idx >> 4 : -1, sh = idx - w * 16;
    uint32_t a = w >= 0 ? d[w] : 0, b = (w + 1) * 16 < n ? d[w + 1] : 0;
    return (uint16_t)(((a << 16) | b) >> (16 - sh));
}

/* Where 16 pixels match the odd (*odd) and even (*even) playfield; idx_o
 * and idx_e: their first pixel in the odd and the even planes
 */
static void matches16(int planes, int idx_o, int idx_e, uint16_t *odd, uint16_t *even)
{
    int enbp = (s.clxcon >> 6) & 63, mvbp = s.clxcon & 63;
    uint16_t mo = 0xFFFF, me = 0xFFFF;
    for (int p = 0; p < 6; p++) {
        if (!(enbp & (1 << p))) {
            continue;
        }
        uint16_t w = plane16(p, planes, (p & 1) ? idx_e : idx_o);
        w = (mvbp & (1 << p)) ? w : (uint16_t)~w;
        if (p & 1) {
            me &= w;
        } else {
            mo &= w;
        }
    }
    *odd = mo;
    *even = me;
}

/* 32 hires pixels to their 16 lores ones (either of the two) */
static uint16_t lores16(uint32_t v)
{
    v = (v | (v >> 1)) & 0x55555555u;
    v = (v | (v >> 1)) & 0x33333333u;
    v = (v | (v >> 2)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4)) & 0x00FF00FFu;
    return (uint16_t)(v | (v >> 8));
}

/* Lores pixels L..L+15 inside the window (bit 15: L) */
static uint16_t win16(int L, int hstart, int hstop)
{
    uint16_t m = 0;
    for (int i = 0; i < 16; i++) {
        if (L + i >= hstart && L + i < hstop) {
            m |= (uint16_t)(0x8000 >> i);
        }
    }
    return m;
}

/* The playfields' matches at lores pixels L..L+15 */
static void pf_matches(int L, uint16_t *odd, uint16_t *even)
{
    int planes = s.fetched ? s.planes : 0;
    int d1 = s.con1 & 15, d2 = (s.con1 >> 4) & 15;
    if (planes && (s.con0 & BPLCON0_SHRES)) {
        uint16_t o[4], e[4];
        int at = 4 * (L - s.first);
        for (int k = 0; k < 4; k++) {
            matches16(planes, at - 4 * d1 + 16 * k, at - 4 * d2 + 16 * k, &o[k], &e[k]);
        }
        *odd = lores16((uint32_t)lores16((uint32_t)o[0] << 16 | o[1]) << 16 | lores16((uint32_t)o[2] << 16 | o[3]));
        *even = lores16((uint32_t)lores16((uint32_t)e[0] << 16 | e[1]) << 16 | lores16((uint32_t)e[2] << 16 | e[3]));
    } else if (planes && (s.con0 & BPLCON0_HIRES)) {
        uint16_t o1, e1, o2, e2;
        int at = 2 * (L - s.first);
        matches16(planes, at - 2 * d1, at - 2 * d2, &o1, &e1);
        matches16(planes, at - 2 * d1 + 16, at - 2 * d2 + 16, &o2, &e2);
        *odd = lores16((uint32_t)o1 << 16 | o2);
        *even = lores16((uint32_t)e1 << 16 | e2);
    } else {
        matches16(planes, L - s.first - d1, L - s.first - d2, odd, even);
    }
}

static void collide(void)
{
    int vstart, vstop, hstart, hstop;
    diw(&vstart, &vstop, &hstart, &hstop);
    if (hstart >= hstop) {
        return;
    }
    uint16_t clx = s.clxdat;
    /* odd planes against even ones */
    for (int L = hstart; L < hstop && !(clx & 1); L += 16) {
        uint16_t o, e;
        pf_matches(L, &o, &e);
        if (o & e & win16(L, hstart, hstop)) {
            clx |= 1;
        }
    }
    /* the sprite pairs (up to two 16-pixel spans each) against the
     * playfields: bits 1-4 the odd one, 5-8 the even one
     */
    int sx[4][2], spans[4] = { 0 };
    uint16_t sm[4][2];
    for (int i = 0; i < 8; i++) {
        const sprite_t *sp = &s.spr[i];
        if (!sp->armed || ((i & 1) && !(s.clxcon & (0x1000 << (i >> 1))))) {
            continue;
        }
        int g = i >> 1, L = ((sp->pos & 0xFF) << 1) | (sp->ctl & 1);
        uint16_t m = (sp->data | sp->datb) & win16(L, hstart, hstop);
        if (m == 0) {
            continue;
        }
        sx[g][spans[g]] = L;
        sm[g][spans[g]] = m;
        spans[g]++;
        uint16_t o, e;
        pf_matches(L, &o, &e);
        clx |= (uint16_t)((m & o) ? 2 << g : 0);
        clx |= (uint16_t)((m & e) ? 0x20 << g : 0);
    }
    /* pairs against pairs: bits 9-14 (0-1, 0-2, 0-3, 1-2, 1-3, 2-3) */
    int bit = 9;
    for (int a = 0; a < 4; a++) {
        for (int b = a + 1; b < 4; b++, bit++) {
            for (int i = 0; i < spans[a]; i++) {
                for (int j = 0; j < spans[b]; j++) {
                    int dx = sx[b][j] - sx[a][i];
                    if (dx > -16 && dx < 16) {
                        uint32_t mb = dx >= 0 ? (uint32_t)sm[b][j] >> dx : (uint32_t)sm[b][j] << -dx;
                        clx |= (uint16_t)((sm[a][i] & mb & 0xFFFF) ? 1u << bit : 0);
                    }
                }
            }
        }
    }
    s.clxdat = clx;
}

/* ---- The line's record ---- */

static inline uint32_t mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 0x01000193u;
}

static void window(int *x0, int *x1)
{
    int vstart, vstop, hstart, hstop;
    diw(&vstart, &vstop, &hstart, &hstop);
    int a = 2 * (hstart - FIRST_LORES), b = 2 * (hstop - FIRST_LORES);
    *x0 = a < 0 ? 0 : a > PIC_W ? PIC_W : a;
    *x1 = b < 0 ? 0 : b > PIC_W ? PIC_W : b;
}

/* The colours as the next line begins: this line's changes applied */
static void apply_changes(void)
{
    for (int i = 0; i < s.changes; i++) {
        s.color[(s.change[i] >> 12) & 31] = s.change[i] & 0xFFF;
    }
    for (int i = 0; s.late && i < 32; i++) {
        if (s.late & (1u << i)) {
            s.color[i] = s.late_value[i];
            s.late &= ~(1u << i);
        }
    }
    s.changes = 0;
}

AMI_HOT void display_line_end(void)
{
    int row = ami.line - s.first_line;
    if (row < 0 || row >= PIC_LINES) {
        apply_changes();
        return;
    }
    int x0, x1;
    window(&x0, &x1);
    if (s.clx_frames) {
        collide();
    }
    bool bpl = s.fetched && x0 < x1;
    /* the sprites showing their data on this line */
    int nspr = 0;
    uint8_t num[8];
    for (int i = 0; i < 8 && x0 < x1; i++) {
        const sprite_t *sp = &s.spr[i];
        if (sp->armed) {
            num[nspr++] = (uint8_t)(i | ((i & 1) && (sp->ctl & 0x80) ? 0x80 : 0));
        }
    }
    uint32_t h = 0x811C9DC5u;
    if (bpl) {
        h = mix(h, s.data_key);
        h = mix(h, (uint32_t)s.con0 << 16 | s.con1);
        h = mix(h, (uint32_t)(s.first & 0xFFFF));
    }
    if (bpl || nspr) {
        h = mix(h, (uint32_t)s.con2 << 16 | s.bplcon2);
        h = mix(h, (uint32_t)x0 << 16 | (uint32_t)x1);
        const uint32_t *c = (const uint32_t *)s.color;
        for (int i = 0; i < 16; i++) {
            h = mix(h, c[i]);
        }
    } else {
        h = mix(h, s.color[0]);
    }
    for (int i = 0; i < nspr; i++) {
        const sprite_t *sp = &s.spr[num[i] & 7];
        h = mix(h, (uint32_t)num[i] << 24 | (uint32_t)(sp->pos & 0xFF) << 8 | (sp->ctl & 1));
        h = mix(h, (uint32_t)sp->data << 16 | sp->datb);
    }
    for (int i = 0; i < s.changes; i++) {
        h = mix(h, s.change[i]);
    }
    uint32_t key = h | 1;
    s.hash = mix(s.hash, key);
    s.shres_seen |= bpl && (s.con0 & BPLCON0_SHRES);

    row_t *r = &buffer(s.field, s.writing[s.field])[row];
    if (r->key != key) {
        r->key = key;
        r->con0 = s.con0;
        r->con1 = s.con1;
        r->con2 = s.con2;
        r->first = (int16_t)s.first;
        r->x0 = (int16_t)x0;
        r->x1 = (int16_t)x1;
        r->planes = bpl ? (uint8_t)s.planes : 0;
        r->words = (uint8_t)s.words;
        r->changes = (uint16_t)s.changes;
        r->sprites = (uint8_t)nspr;
        for (int i = 0; i < nspr; i++) {
            const sprite_t *sp = &s.spr[num[i] & 7];
            r->spr_num[i] = num[i];
            r->spr_x[i] = (int16_t)((((sp->pos & 0xFF) << 1) | (sp->ctl & 1)) - FIRST_LORES);
            r->spr_a[i] = sp->data;
            r->spr_b[i] = sp->datb;
        }
        if (!bpl) {
            r->con2 = s.bplcon2;
        }
        memcpy(r->color, s.color, sizeof(r->color));
        memcpy(r->change, s.change, s.changes * sizeof(uint32_t));
        if (bpl) {
            memcpy(r->data, s.data, s.planes * s.words * sizeof(uint16_t));
        }
    }
    apply_changes();
}

/* The frame's records become its field's latest; true when it was
 * interlaced (the long frames then alternate with short ones)
 */
bool display_frame_end(void)
{
    if (s.clx_frames) {
        s.clx_frames--;
    }
    int f = s.field, w = s.writing[f];
    bool lace = s.lace_seen;
    s.signature[f][w] = s.hash;
    s.lace[f][w] = lace;
    s.shres[f][w] = s.shres_seen;
    s.shres_seen = false;
    s.hash = 0x811C9DC5u;
    if (f == 1) {
        s.field1 = true;
    } else if (!lace) {
        s.field1 = false;
    }
    uint32_t st = __atomic_load_n(&s.state[f], __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~3u) | (uint32_t)w;
    } while (!__atomic_compare_exchange_n(&s.state[f], &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    uint32_t pinned = PINNED(want);
    for (int i = 0; i < 3; i++) {
        if (i != w && (uint32_t)i != pinned) {
            s.writing[f] = i;
            break;
        }
    }
    s.lace_seen = (s.bplcon0 & BPLCON0_LACE) != 0;
    return lace;
}

/* The next frame's field: its LOF (a short frame of an interlaced picture
 * draws the odd lines)
 */
void display_next_field(bool lace)
{
    s.field = lace && !ami.lof ? 1 : 0;
    s.first_line = ami_ntsc_beam() ? FIRST_LINE_NTSC : FIRST_LINE_PAL;
}

/* ---- The presenter's side: drawing the records ---- */

/* The pixels of a line's planes from MARGIN on; before it room for the
 * scroll (a super-hires fetch from DDFSTRT $D8: 4 x (108 - 437 - 15))
 */
#define MARGIN          1408
#define BUF             (MARGIN + 1664 + 64)

static struct {
    uint8_t odd[BUF] __attribute__((aligned(8)));
    uint8_t even[BUF] __attribute__((aligned(8)));
    int filled;
    uint16_t color[32];
    uint16_t rgb[64];               /* 32-63: half bright */
    uint16_t srgb[8];               /* super-hires: the 4 colours, the sprites' 4 */
    uint16_t ham;
    const row_t *row;               /* the row in line[] */
    uint32_t key;
    bool wide;                      /* line[] drawn WIDE_W wide */
    uint16_t line[WIDE_W] __attribute__((aligned(4)));
    uint16_t tmp[WIDE_W] __attribute__((aligned(4)));
} d;

static inline void set_color(int reg, uint16_t v)
{
    d.color[reg] = v;
    d.rgb[reg] = s_rgb565[v];
    d.rgb[reg + 32] = s_rgb565[(v >> 1) & 0x777];
}

/* (a and b even) */
static void fill(uint16_t *out, int a, int b, uint16_t c)
{
    uint32_t *o32 = (uint32_t *)out, c2 = c * 0x10001u;
    for (int i = a / 2; i < b / 2; i++) {
        o32[i] = c2;
    }
}

static void compose(const row_t *r, uint16_t *out, int xa, int xb)
{
    uint16_t con0 = r->con0;
    int d1 = r->con1 & 15, d2 = (r->con1 >> 4) & 15;
    if (con0 & BPLCON0_HIRES) {
        const uint8_t *po = d.odd + MARGIN + 2 * (FIRST_LORES - r->first - d1);
        const uint8_t *pe = d.even + MARGIN + 2 * (FIRST_LORES - r->first - d2);
        if (con0 & BPLCON0_DPF) {
            bool pri2 = r->con2 & BPLCON2_PF2PRI;
            for (int x = xa; x < xb; x++) {
                int v = po[x] | pe[x];
                int p1 = s_pf1[v], p2 = s_pf2[v];
                int i = pri2 ? (p2 ? 8 + p2 : p1) : (p1 ? p1 : p2 ? 8 + p2 : 0);
                out[x] = d.rgb[i];
            }
            return;
        }
        for (int x = xa; x < xb; x++) {
            out[x] = d.rgb[po[x] | pe[x]];
        }
        return;
    }
    const uint8_t *po = d.odd + MARGIN + FIRST_LORES - r->first - d1;
    const uint8_t *pe = d.even + MARGIN + FIRST_LORES - r->first - d2;
    uint32_t *o32 = (uint32_t *)out;
    int pa = xa / 2, pb = xb / 2;
    if (con0 & BPLCON0_DPF) {
        bool pri2 = r->con2 & BPLCON2_PF2PRI;
        for (int p = pa; p < pb; p++) {
            int v = po[p] | pe[p];
            int p1 = s_pf1[v], p2 = s_pf2[v];
            int i = pri2 ? (p2 ? 8 + p2 : p1) : (p1 ? p1 : p2 ? 8 + p2 : 0);
            o32[p] = d.rgb[i] * 0x10001u;
        }
        return;
    }
    if ((con0 & BPLCON0_HAM) && r->planes >= 5) {
        uint16_t ham = d.ham;
        for (int p = pa; p < pb; p++) {
            int v = po[p] | pe[p], val = v & 15;
            switch (v >> 4) {
            case 0: ham = d.color[val]; break;
            case 1: ham = (uint16_t)((ham & 0xFF0) | val); break;
            case 2: ham = (uint16_t)((ham & 0x0FF) | (val << 8)); break;
            default: ham = (uint16_t)((ham & 0xF0F) | (val << 4)); break;
            }
            o32[p] = s_rgb565[ham] * 0x10001u;
        }
        d.ham = ham;
        return;
    }
    for (int p = pa; p < pb; p++) {
        o32[p] = d.rgb[po[p] | pe[p]] * 0x10001u;
    }
}

/* Super-hires colours from the registers (see the top) */
static void shres_colors(void)
{
    for (int v = 0; v < 8; v++) {
        uint16_t c = d.color[(v & 4) * 4 + 5 * (v & 3)];
        d.srgb[v] = s_rgb565[s_shres_gun[c >> 8] << 8 | s_shres_gun[(c >> 4) & 15] << 4 | s_shres_gun[c & 15]];
    }
}

static void span(const row_t *r, uint16_t *out, int a, int b)
{
    int ws = a > r->x0 ? a : r->x0, we = b < r->x1 ? b : r->x1;
    if (r->planes == 0 || ws >= we) {
        fill(out, a, b, d.rgb[0]);
        return;
    }
    fill(out, a, ws, d.rgb[0]);
    compose(r, out, ws, we);
    fill(out, we, b, d.rgb[0]);
}

/* The playfield's priority code at a picture pixel (BPLCON2: a sprite pair
 * shows in front of a playfield whose code is above the pair's number), or
 * 8 where the playfields are transparent
 */
static int pf_code(const row_t *r, int x)
{
    if (r->planes == 0 || x < r->x0 || x >= r->x1) {
        return 8;
    }
    uint16_t con0 = r->con0;
    int d1 = r->con1 & 15, d2 = (r->con1 >> 4) & 15;
    int v;
    if (con0 & BPLCON0_HIRES) {
        v = d.odd[MARGIN + 2 * (FIRST_LORES - r->first - d1) + x] | d.even[MARGIN + 2 * (FIRST_LORES - r->first - d2) + x];
    } else {
        int p = x / 2;
        v = d.odd[MARGIN + FIRST_LORES - r->first - d1 + p] | d.even[MARGIN + FIRST_LORES - r->first - d2 + p];
    }
    int pf1p = r->con2 & 7, pf2p = (r->con2 >> 3) & 7;
    if (con0 & BPLCON0_DPF) {
        int p1 = s_pf1[v], p2 = s_pf2[v];
        bool pri2 = r->con2 & BPLCON2_PF2PRI;
        if (pri2 ? p2 : !p1 && p2) {
            return pf2p;
        }
        return p1 ? pf1p : 8;
    }
    return v ? pf2p : 8;
}

/* A super-hires line's playfield priority code at a pixel (as pf_code()) */
static int pf_code_shres(const row_t *r, int x)
{
    if (r->planes == 0 || x < 2 * r->x0 || x >= 2 * r->x1) {
        return 8;
    }
    int d1 = r->con1 & 15, d2 = (r->con1 >> 4) & 15;
    int v = d.odd[MARGIN + 4 * (FIRST_LORES - r->first - d1) + x] | d.even[MARGIN + 4 * (FIRST_LORES - r->first - d2) + x];
    return v ? (r->con2 >> 3) & 7 : 8;
}

/* The sprites over the line: lower numbers in front, then the playfields'
 * priorities; shres: a super-hires line (4 pixels a lores one)
 */
static void draw_sprites(const row_t *r, uint16_t *out, bool shres)
{
    static uint8_t col[360];            /* colour (16-31) or 0, per lores pixel */
    static uint8_t pair[360];
    int lo = 360, hi = 0;
    for (int k = r->sprites - 1; k >= 0; k--) {
        int n = r->spr_num[k] & 7;
        bool attached = r->spr_num[k] & 0x80;
        int x = r->spr_x[k];
        uint16_t a = r->spr_a[k], b = r->spr_b[k];
        /* an attached odd sprite: its bits over its partner's (k - 1) */
        uint16_t pa = 0, pb = 0;
        if (attached && k > 0 && (r->spr_num[k - 1] & 7) == n - 1) {
            pa = r->spr_a[k - 1];
            pb = r->spr_b[k - 1];
        }
        for (int i = 0; i < 16; i++) {
            int p = x + i;
            if (p < 0 || p >= 360) {
                continue;
            }
            int bit = 15 - i;
            int v = ((a >> bit) & 1) | (((b >> bit) & 1) << 1);
            int c;
            if (attached) {
                v = (v << 2) | ((pa >> bit) & 1) | (((pb >> bit) & 1) << 1);
                c = 16 + v;
            } else {
                c = 16 + (n >> 1) * 4 + v;
            }
            if (v) {
                col[p] = (uint8_t)c;
                pair[p] = (uint8_t)(n >> 1);
                lo = p < lo ? p : lo;
                hi = p > hi ? p : hi;
            }
        }
        if (attached && k > 0 && (r->spr_num[k - 1] & 7) == n - 1) {
            k--;                        /* (its partner drawn with it) */
        }
    }
    for (int p = lo; p <= hi; p++) {
        if (!col[p]) {
            continue;
        }
        if (shres) {
            for (int x = 4 * p; x < 4 * p + 4; x++) {
                if (x >= 2 * r->x0 && x < 2 * r->x1 && pair[p] < pf_code_shres(r, x)) {
                    out[x] = d.srgb[4 + ((col[p] - 16) & 3)];
                }
            }
        } else {
            for (int x = 2 * p; x < 2 * p + 2; x++) {
                if (x >= r->x0 && x < r->x1 && pair[p] < pf_code(r, x)) {
                    out[x] = d.rgb[col[p]];
                }
            }
        }
        col[p] = 0;
    }
}

/* A super-hires line, WIDE_W pixels (the planes already in odd[], even[]) */
static void draw_shres(const row_t *r, uint16_t *out)
{
    int d1 = r->con1 & 15, d2 = (r->con1 >> 4) & 15;
    const uint8_t *po = d.odd + MARGIN + 4 * (FIRST_LORES - r->first - d1);
    const uint8_t *pe = d.even + MARGIN + 4 * (FIRST_LORES - r->first - d2);
    int x0 = 2 * r->x0, x1 = 2 * r->x1;
    shres_colors();
    int ci = 0, x = 0, n = r->changes;
    while (x < WIDE_W) {
        bool changed = false;
        while (ci < n && 2 * (int)(r->change[ci] >> 20) <= x) {
            set_color((r->change[ci] >> 12) & 31, r->change[ci] & 0xFFF);
            ci++;
            changed = true;
        }
        if (changed) {
            shres_colors();
        }
        int end = ci < n ? 2 * (int)(r->change[ci] >> 20) : WIDE_W;
        int ws = x > x0 ? x : x0, we = end < x1 ? end : x1;
        if (ws >= we) {
            fill(out, x, end, d.srgb[0]);
        } else {
            fill(out, x, ws, d.srgb[0]);
            for (int i = ws; i < we; i++) {
                out[i] = d.srgb[po[i] | pe[i]];
            }
            fill(out, we, end, d.srgb[0]);
        }
        x = end;
    }
    if (r->sprites) {
        draw_sprites(r, out, true);
    }
}

/* A line, PIC_W pixels or (wide) WIDE_W */
static void draw(const row_t *r, uint16_t *out, bool wide)
{
    if (r->key == 0) {
        memset(out, 0, (wide ? WIDE_W : PIC_W) * sizeof(uint16_t));
        return;
    }
    for (int i = 0; i < 32; i++) {
        set_color(i, r->color[i]);
    }
    if (r->planes) {
        memset(d.odd + MARGIN, 0, d.filled);
        memset(d.even + MARGIN, 0, d.filled);
        int words = r->words;
        d.filled = words * 16;
        const uint16_t *src = r->data;
        for (int p = 0; p < r->planes; p++) {
            uint64_t *dst = (uint64_t *)(((p & 1) ? d.even : d.odd) + MARGIN);
            for (int w = 0; w < words; w++) {
                uint16_t v = *src++;
                dst[2 * w] |= s_p2c[v >> 8] << p;
                dst[2 * w + 1] |= s_p2c[v & 0xFF] << p;
            }
        }
    }
    if (r->planes && (r->con0 & BPLCON0_SHRES)) {
        if (wide) {
            draw_shres(r, out);
            return;
        }
        /* (not the presenter's frame's width: every other pixel) */
        draw_shres(r, d.tmp);
        for (int i = 0; i < PIC_W; i++) {
            out[i] = d.tmp[2 * i];
        }
        return;
    }
    d.ham = d.color[0];
    int ci = 0, x = 0, n = r->changes;
    while (x < PIC_W) {
        while (ci < n && (int)(r->change[ci] >> 20) <= x) {
            set_color((r->change[ci] >> 12) & 31, r->change[ci] & 0xFFF);
            ci++;
        }
        int end = ci < n ? (int)(r->change[ci] >> 20) : PIC_W;
        span(r, out, x, end);
        x = end;
    }
    if (r->sprites) {
        draw_sprites(r, out, false);
    }
    if (wide) {
        for (int i = PIC_W - 1; i >= 0; i--) {
            out[2 * i] = out[2 * i + 1] = out[i];
        }
    }
}

/* The presenter's frame of a field: its latest, pinned (the writer then
 * leaves it alone)
 */
static uint32_t pin(int f)
{
    uint32_t st = __atomic_load_n(&s.state[f], __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~(3u << 2)) | LATEST(st) << 2;
    } while (!__atomic_compare_exchange_n(&s.state[f], &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return LATEST(st);
}

static bool s_weave;                /* (the presenter's) both fields, line by line */
static bool s_wide;                 /* the presenter's frame: WIDE_W wide */
static bool s_dbg_wide;             /* the debugger's (told when it asked the size) */
static uint32_t s_pinned_frame;     /* when the presenter last pinned */
static volatile uint32_t s_draws;   /* lines drawn (the debugger's check) */

static uint32_t pin_latest(bool *wide)
{
    uint32_t b0 = pin(0);
    s_weave = s.lace[0][b0] && s.field1;
    *wide = s.shres[0][b0];
    if (!s_weave) {
        return s.signature[0][b0];
    }
    uint32_t b1 = pin(1);
    *wide |= s.shres[1][b1];
    return s.signature[0][b0] ^ (s.signature[1][b1] * 0x9E3779B1u);
}

uint32_t amiga_screen_signature(void)
{
    s_pinned_frame = ami.frames;
    bool wide;
    uint32_t sig = pin_latest(&wide);
    s_wide = wide;
    return sig ^ (wide ? 0x5A5A5A5Au : 0);
}

int amiga_screen_width(void)
{
    return s_wide ? WIDE_W : PIC_W;
}

static void render(int y, uint16_t *dst, bool wide)
{
    s_draws++;
    int row = y >> 1;
    if (s.rows == NULL || row < 0 || row >= PIC_LINES) {
        memset(dst, 0, (wide ? WIDE_W : PIC_W) * sizeof(uint16_t));
        return;
    }
    int f = s_weave ? y & 1 : 0;
    uint32_t pinned = PINNED(__atomic_load_n(&s.state[f], __ATOMIC_ACQUIRE));
    const row_t *r = &buffer(f, (int)pinned)[row];
    if (r != d.row || r->key != d.key || wide != d.wide) {
        d.row = NULL;               /* (half drawn: see display_dbg_line()) */
        draw(r, d.line, wide);
        d.row = r;
        d.key = r->key;
        d.wide = wide;
    }
    memcpy(dst, d.line, (wide ? WIDE_W : PIC_W) * sizeof(uint16_t));
}

void amiga_render_line_rgb565(int y, uint16_t *dst)
{
    render(y, dst, s_wide);
}

/* The debugger's picture, from its task (on the board below the presenter,
 * on the same core: the presenter may draw in the middle of the debugger's
 * line, never the other way round). The presenter's frame, or the latest
 * when nothing pins (the host build, no presenter). Its width.
 */
int display_dbg_frame(void)
{
    bool wide = s_wide;
    if (ami.frames - s_pinned_frame > 25) {
        pin_latest(&wide);
    }
    s_dbg_wide = wide;
    return wide ? WIDE_W : PIC_W;
}

/* A line for the debugger: drawn again if the presenter drew meanwhile
 * (they share the drawing's state)
 */
void display_dbg_line(int y, uint16_t *dst)
{
    for (int tries = 0; tries < 8; tries++) {
        uint32_t before = s_draws;
        render(y, dst, s_dbg_wide);
        if (s_draws == before + 1) {
            return;
        }
    }
}
