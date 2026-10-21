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
 * differ), the colours changed at the pixels the beam was at.
 *
 * The picture: lores pixels 108-467 (hires 216-935) of lines 29-312, in
 * hires pixels (lores ones doubled), its lines doubled (interlace: the
 * field being drawn, for now).
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
#define MAX_WORDS       200             /* planes x words: 6 x 25 lores, 4 x 50 hires */

#define BPLCON0_HIRES   0x8000
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
    uint16_t color[32];             /* as the line began */
    uint32_t change[MAX_CHANGES];   /* x << 20 | colour << 12 | value */
    uint16_t data[MAX_WORDS];       /* plane after plane */
} row_t;

static struct {
    /* registers */
    uint16_t bplcon0, bplcon1, bplcon2, bplcon3;
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

    /* the frames */
    row_t *rows;
    uint32_t signature[3];
    uint32_t hash;
    uint32_t state;                 /* latest | pinned << 2 */
    int writing;
    int first_line;
} s;

static uint16_t s_rgb565[4096];
static uint64_t s_p2c[256];
static uint8_t s_pf1[64], s_pf2[64];

#define LATEST(st)      ((st) & 3u)
#define PINNED(st)      (((st) >> 2) & 3u)

size_t amiga_picture_size(void)
{
    return 3 * PIC_LINES * sizeof(row_t);
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
    s.writing = 1;
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
    if ((con0 & BPLCON0_HIRES) && n > 4) {
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
    int vblank_end = ami.ntsc ? VBLANK_END_NTSC : VBLANK_END_PAL;
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
}

AMI_HOT void display_fetch(void)
{
    s.fetch_at = NEVER;
    s.fetched = true;
    s.con0 = s.bplcon0;
    s.con1 = s.bplcon1;
    s.con2 = s.bplcon2;
    bool hires = s.con0 & BPLCON0_HIRES;
    int planes = planes_of(s.con0);
    int start, stop;
    ddf(&start, &stop);
    /* whole fetch units of 8 colour clocks, the one DDFSTOP falls in too
     * (Kickstart 1.3's hires screens: $3C-$D0 is 40 words; 3.1's $40-$D0 38)
     */
    int words = ((stop - start + 7) / 8 + 1) * (hires ? 2 : 1);
    s.first = start * 2 + (hires ? 9 : 17);
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
        s.bplcon0 = v;
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
    bool bpl = s.fetched && x0 < x1;
    uint32_t h = 0x811C9DC5u;
    if (bpl) {
        h = mix(h, s.data_key);
        h = mix(h, (uint32_t)s.con0 << 16 | s.con1);
        h = mix(h, (uint32_t)s.con2 << 16 | (uint32_t)(s.first & 0xFFFF));
        h = mix(h, (uint32_t)x0 << 16 | (uint32_t)x1);
        const uint32_t *c = (const uint32_t *)s.color;
        for (int i = 0; i < 16; i++) {
            h = mix(h, c[i]);
        }
    } else {
        h = mix(h, s.color[0]);
    }
    for (int i = 0; i < s.changes; i++) {
        h = mix(h, s.change[i]);
    }
    uint32_t key = h | 1;
    s.hash = mix(s.hash, key);

    row_t *r = &s.rows[s.writing * PIC_LINES + row];
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
        memcpy(r->color, s.color, sizeof(r->color));
        memcpy(r->change, s.change, s.changes * sizeof(uint32_t));
        if (bpl) {
            memcpy(r->data, s.data, s.planes * s.words * sizeof(uint16_t));
        }
    }
    apply_changes();
}

void display_frame_end(void)
{
    s.signature[s.writing] = s.hash;
    s.hash = 0x811C9DC5u;
    uint32_t st = __atomic_load_n(&s.state, __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~3u) | (uint32_t)s.writing;
    } while (!__atomic_compare_exchange_n(&s.state, &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    uint32_t pinned = PINNED(want);
    for (int i = 0; i < 3; i++) {
        if (i != s.writing && (uint32_t)i != pinned) {
            s.writing = i;
            break;
        }
    }
}

/* ---- The presenter's side: drawing the records ---- */

#define MARGIN          768
#define BUF             (MARGIN + 1024 + 64)

static struct {
    uint8_t odd[BUF] __attribute__((aligned(8)));
    uint8_t even[BUF] __attribute__((aligned(8)));
    int filled;
    uint16_t color[32];
    uint16_t rgb[64];               /* 32-63: half bright */
    uint16_t ham;
    const row_t *row;               /* the row in line[] */
    uint32_t key;
    uint16_t line[PIC_W] __attribute__((aligned(4)));
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

static void draw(const row_t *r, uint16_t *out)
{
    if (r->key == 0) {
        memset(out, 0, PIC_W * sizeof(uint16_t));
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
}

uint32_t amiga_screen_signature(void)
{
    uint32_t st = __atomic_load_n(&s.state, __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~(3u << 2)) | LATEST(st) << 2;
    } while (!__atomic_compare_exchange_n(&s.state, &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return s.signature[LATEST(st)];
}

void amiga_render_line_rgb565(int y, uint16_t *dst)
{
    int row = y >> 1;
    if (s.rows == NULL || row < 0 || row >= PIC_LINES) {
        memset(dst, 0, PIC_W * sizeof(uint16_t));
        return;
    }
    uint32_t pinned = PINNED(__atomic_load_n(&s.state, __ATOMIC_ACQUIRE));
    const row_t *r = &s.rows[pinned * PIC_LINES + row];
    if (r != d.row || r->key != d.key) {
        draw(r, d.line);
        d.row = r;
        d.key = r->key;
    }
    memcpy(dst, d.line, sizeof(d.line));
}
