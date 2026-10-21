/*
 * The floppy drives and Paula's disk DMA.
 *
 * The drives: DF0: and DF1:, selected, stepped and their motors switched
 * through CIA-B's port B, read back on CIA-A's port A:
 *   CIA-B PRB: 7 /MTR (latched by a drive when it's selected), 6-3 /SEL3-0,
 *              2 /SIDE, 1 DIR (0: inwards), 0 /STEP
 *   CIA-A PRA: 5 /RDY, 4 /TK0, 3 /WPRO, 2 /CHNG (the selected drives')
 * The disk turns at 300 rpm: a track is TRACK_WORDS MFM words (2 us a bit
 * cell), an index pulse on CIA-B's FLAG each turn.
 *
 * The images: ADF (80 cylinders, 2 sides, 11 sectors of 512), kept in
 * memory, written tracks back to the file at amiga_floppy_flush(). The
 * track under the head is made from the image's sectors in AmigaDOS's own
 * format (sync words, header, checksums, odd/even MFM), so trackdisk and
 * the games' own loaders both read it through the DMA; a track written
 * through the DMA is decoded back into sectors when the write ends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_internal.h"

#define DRIVES          4
#define CYLINDERS       80
#define HEAD_STOPS      84
#define SECTORS         11
#define ADF_SIZE        (CYLINDERS * 2 * SECTORS * 512)
#define TRACK_WORDS     6334            /* a turn: 12668 bytes of MFM */
#define WORD_CYCLES     224             /* 16 bit cells of 2 us, in 68000 clocks */
#define SECTOR_WORDS    544
#define SYNC            0x4489
#define ADK_WORDSYNC    0x0400

typedef struct {
    bool present;
    bool motor;
    bool selected;
    int cylinder;
    bool changed;                   /* /CHNG low: a disk was out since the last step */
    int id_bit;                     /* the drive's ID, read through /RDY with the motor off */

    uint8_t *data;                  /* the image (NULL: no disk) */
    uint16_t *mfm;                  /* the track under the head */
    int mfm_track;                  /* which (cylinder * 2 + side), -1: none */
    char path[256];
    bool protect;
    uint8_t dirty[CYLINDERS * 2 / 8];
    bool any_dirty;

    int pos;                        /* the word under the head */
    int frac;                       /* clocks into it */
    uint32_t reads, writes;
} drive_t;

static drive_t s_drive[DRIVES];
static int s_side;

/* Paula's side */
static struct {
    uint32_t pt;
    uint16_t len;                   /* the last DSKLEN written: DMA starts on the second */
    uint16_t sync;
    bool active, writing, synced;
    int left;
    int drive;                      /* the drive being written */
    bool wordequal;
    bool byte_ready;
} dk;

/* ---- MFM ---- */

/* data's bits at the even positions, the clock bits filled in (prev: the
 * MFM long before, for the first clock bit)
 */
static uint32_t mfm_long(uint32_t data, uint32_t prev)
{
    data &= 0x55555555;
    uint32_t clock = ~((data << 1) | (data >> 1) | (prev << 31)) & 0xAAAAAAAA;
    return data | clock;
}

typedef struct {
    uint16_t *w;
    int n;
    uint32_t last;                  /* the last long written (its bit 0) */
} track_writer_t;

static void put_word(track_writer_t *t, uint16_t v)
{
    t->w[t->n++] = v;
    t->last = (t->last << 16) | v;
}

static void put_long(track_writer_t *t, uint32_t raw)
{
    uint32_t m = mfm_long(raw, t->last);
    put_word(t, (uint16_t)(m >> 16));
    put_word(t, (uint16_t)m);
}

/* odd bits then even bits of n longs */
static uint32_t put_odd_even(track_writer_t *t, const uint32_t *v, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) {
        uint32_t odd = (v[i] >> 1) & 0x55555555;
        sum ^= odd;
        put_long(t, odd);
    }
    for (int i = 0; i < n; i++) {
        uint32_t even = v[i] & 0x55555555;
        sum ^= even;
        put_long(t, even);
    }
    return sum;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void make_track(drive_t *d, int track)
{
    track_writer_t t = { .w = d->mfm, .n = 0, .last = 0 };
    for (int s = 0; s < SECTORS; s++) {
        put_word(&t, (t.last & 1) ? 0x2AAA : 0xAAAA);
        put_word(&t, 0xAAAA);
        put_word(&t, SYNC);
        put_word(&t, SYNC);
        uint32_t info = 0xFF000000u | (uint32_t)track << 16 | (uint32_t)s << 8 | (uint32_t)(SECTORS - s);
        uint32_t label[4] = { 0 };
        uint32_t hsum = put_odd_even(&t, &info, 1);
        hsum ^= put_odd_even(&t, label, 4);
        uint32_t data[128];
        const uint8_t *src = d->data + ((size_t)track * SECTORS + s) * 512;
        for (int i = 0; i < 128; i++) {
            data[i] = be32(src + i * 4);
        }
        uint32_t dsum = 0;
        for (int i = 0; i < 128; i++) {
            dsum ^= ((data[i] >> 1) & 0x55555555) ^ (data[i] & 0x55555555);
        }
        put_odd_even(&t, &hsum, 1);
        put_odd_even(&t, &dsum, 1);
        put_odd_even(&t, data, 128);
    }
    while (t.n < TRACK_WORDS) {
        put_word(&t, (t.last & 1) ? 0x2AAA : 0xAAAA);
    }
    d->mfm_track = track;
}

static uint32_t get_long(const uint16_t *w, int i)
{
    return ((uint32_t)w[i % TRACK_WORDS] << 16) | w[(i + 1) % TRACK_WORDS];
}

static uint32_t decode(const uint16_t *w, int odd_at, int even_at)
{
    return ((get_long(w, odd_at) & 0x55555555) << 1) | (get_long(w, even_at) & 0x55555555);
}

/* A written track back into the image's sectors */
static void decode_track(drive_t *d, int track)
{
    const uint16_t *w = d->mfm;
    for (int i = 0; i < TRACK_WORDS; i++) {
        if (w[i] != SYNC || w[(i + 1) % TRACK_WORDS] == SYNC) {
            continue;
        }
        int h = i + 1;
        uint32_t info = decode(w, h, h + 2);
        int sector = (info >> 8) & 0xFF;
        if ((info >> 24) != 0xFF || (int)((info >> 16) & 0xFF) != track || sector >= SECTORS) {
            continue;
        }
        uint8_t *dst = d->data + ((size_t)track * SECTORS + sector) * 512;
        int data_at = h + 4 + 16 + 4 + 4;
        for (int k = 0; k < 128; k++) {
            uint32_t v = decode(w, data_at + 2 * k, data_at + 256 + 2 * k);
            dst[4 * k] = (uint8_t)(v >> 24);
            dst[4 * k + 1] = (uint8_t)(v >> 16);
            dst[4 * k + 2] = (uint8_t)(v >> 8);
            dst[4 * k + 3] = (uint8_t)v;
        }
        d->dirty[track / 8] |= (uint8_t)(1u << (track & 7));
        d->any_dirty = true;
    }
}

/* ---- The drives ---- */

void floppy_reset(void)
{
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        d->present = i < AMIGA_FLOPPIES;
        d->motor = false;
        d->selected = false;
        d->cylinder = 0;
        d->changed = true;
        d->id_bit = 0;
        d->pos = 0;
        d->frac = 0;
    }
    s_side = 0;
    memset(&dk, 0, sizeof(dk));
}

void floppy_control(uint8_t old, uint8_t prb)
{
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        uint8_t sel = 0x08 << i;
        bool now = !(prb & sel);
        if (now && (old & sel)) {
            /* selected: the motor line is latched; with the motor off,
             * each selection shifts the ID out one bit
             */
            bool motor = !(prb & 0x80);
            if (d->motor && !motor) {
                d->id_bit = 0;
            } else if (!motor) {
                d->id_bit = (d->id_bit + 1) & 31;
            }
            d->motor = motor;
        }
        d->selected = now;
    }
    s_side = (prb & 0x04) ? 0 : 1;
    if ((old & 0x01) && !(prb & 0x01)) {
        for (int i = 0; i < DRIVES; i++) {
            drive_t *d = &s_drive[i];
            if (!d->selected || !d->present) {
                continue;
            }
            if (prb & 0x02) {
                if (d->cylinder > 0) {
                    d->cylinder--;
                }
            } else if (d->cylinder < HEAD_STOPS - 1) {
                d->cylinder++;
            }
            if (d->data) {
                d->changed = false;
            }
        }
    }
}

uint8_t floppy_status(void)
{
    uint8_t v = 0x3C;
    for (int i = 0; i < DRIVES; i++) {
        const drive_t *d = &s_drive[i];
        if (!d->selected || !d->present) {
            continue;
        }
        /* a 3.5" double density drive: ID all ones (/RDY low while the
         * motor is off); ready with it on
         */
        v &= ~0x20;
        if (d->cylinder == 0) {
            v &= ~0x10;
        }
        if (!d->data || d->protect) {
            v &= ~0x08;
        }
        if (d->changed) {
            v &= ~0x04;
        }
    }
    return v;
}

/* The drive whose data reaches Paula: the first selected one turning */
static drive_t *reading(void)
{
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        if (d->selected && d->present && d->motor && d->data) {
            return d;
        }
    }
    return NULL;
}

static bool dma_on(void)
{
    return (ami.dmacon & (DMA_EN | DMA_DSK)) == (DMA_EN | DMA_DSK);
}

static void end_dma(void)
{
    if (dk.writing) {
        drive_t *d = &s_drive[dk.drive];
        if (d->data && d->mfm_track >= 0 && d->mfm_track < CYLINDERS * 2 && !d->protect) {
            decode_track(d, d->mfm_track);
        }
        d->writes++;
    }
    dk.active = false;
    ami_set_intreq(INT_DSKBLK);
}

/* A word passes under the head of the drive Paula reads */
static void word_passes(drive_t *d)
{
    int track = d->cylinder * 2 + s_side;
    if (d->mfm_track != track) {
        if (d->cylinder < CYLINDERS) {
            make_track(d, track);
        } else {
            for (int i = 0; i < TRACK_WORDS; i++) {
                d->mfm[i] = 0xAAAA;
            }
            d->mfm_track = track;
        }
    }
    uint16_t w = d->mfm[d->pos];
    dk.wordequal = w == dk.sync;
    dk.byte_ready = true;
    if (dk.wordequal) {
        ami_set_intreq(INT_DSKSYN);
    }
    if (!dk.active || !dma_on()) {
        return;
    }
    if (dk.writing) {
        d->mfm[d->pos] = chip_read16(dk.pt);
        dk.pt += 2;
        dk.drive = (int)(d - s_drive);
    } else if (!(ami.adkcon & ADK_WORDSYNC) || dk.synced) {
        chip_write16(dk.pt, w);
        dk.pt += 2;
        if (dk.left == 1) {
            d->reads++;
        }
    } else {
        dk.synced = dk.wordequal;   /* (the sync word itself isn't transferred) */
        return;
    }
    if (--dk.left == 0) {
        end_dma();
    }
}

void floppy_line(int cycles)
{
    drive_t *r = reading();
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        if (!d->motor || !d->data) {
            continue;
        }
        d->frac += cycles;
        while (d->frac >= WORD_CYCLES) {
            d->frac -= WORD_CYCLES;
            if (d == r) {
                word_passes(d);
            }
            if (++d->pos == TRACK_WORDS) {
                d->pos = 0;
                if (d->selected) {
                    cia_flag(&cia[1]);      /* the index pulse */
                }
            }
        }
    }
}

/* ---- Paula's disk registers ---- */

void floppy_write(uint32_t reg, uint16_t v)
{
    switch (reg) {
    case 0x020:                     /* DSKPTH, DSKPTL */
        dk.pt = (dk.pt & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        break;
    case 0x022:
        dk.pt = (dk.pt & 0xFFFF0000) | (v & 0xFFFE);
        break;
    case 0x024:                     /* DSKLEN: written twice with bit 15 starts it */
        if (!(v & 0x8000)) {
            dk.active = false;
        } else if (dk.len & 0x8000) {
            dk.active = (v & 0x3FFF) != 0;
            dk.writing = (v & 0x4000) != 0;
            dk.left = v & 0x3FFF;
            dk.synced = false;
            dk.drive = 0;
            drive_t *d = reading();
            if (d) {
                dk.drive = (int)(d - s_drive);
            }
        }
        dk.len = v;
        break;
    case 0x07E:                     /* DSKSYNC */
        dk.sync = v;
        break;
    default:
        break;
    }
}

uint16_t floppy_dskbytr(void)
{
    uint16_t v = 0;
    drive_t *d = reading();
    if (d && d->mfm_track >= 0) {
        uint16_t w = d->mfm[d->pos];
        v = d->frac < WORD_CYCLES / 2 ? (w >> 8) : (w & 0xFF);
    }
    if (dk.byte_ready) {
        v |= 0x8000;
        dk.byte_ready = false;
    }
    if (dk.active) {
        v |= 0x4000;
    }
    if (dk.active && dk.writing) {
        v |= 0x2000;
    }
    if (dk.wordequal) {
        v |= 0x1000;
    }
    return v;
}

/* ---- The images ---- */

static void flush_drive(drive_t *d)
{
    if (!d->data || !d->any_dirty || d->protect) {
        return;
    }
    FILE *f = fopen(d->path, "r+b");
    if (f) {
        for (int t = 0; t < CYLINDERS * 2; t++) {
            if (d->dirty[t / 8] & (1u << (t & 7))) {
                long off = (long)t * SECTORS * 512;
                fseek(f, off, SEEK_SET);
                fwrite(d->data + off, 1, SECTORS * 512, f);
            }
        }
        fclose(f);
    }
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
}

bool amiga_floppy_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= AMIGA_FLOPPIES) {
        return false;
    }
    drive_t *d = &s_drive[drive];
    amiga_floppy_eject(drive);
    bool ro = false;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
        ro = true;
    }
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size != ADF_SIZE) {
        AMI_LOGW("DF%d: %s: %ld bytes is not an ADF (%d)", drive, path, size, ADF_SIZE);
        fclose(f);
        return false;
    }
    /* kept for the next disk (big blocks fragment PSRAM) */
    static uint8_t *images[AMIGA_FLOPPIES];
    static uint16_t *tracks[AMIGA_FLOPPIES];
    if (!images[drive]) {
        images[drive] = ami_malloc(ADF_SIZE);
        tracks[drive] = ami_malloc(TRACK_WORDS * sizeof(uint16_t));
    }
    if (!images[drive] || !tracks[drive] || fread(images[drive], 1, ADF_SIZE, f) != ADF_SIZE) {
        fclose(f);
        return false;
    }
    fclose(f);
    d->data = images[drive];
    d->mfm = tracks[drive];
    d->mfm_track = -1;
    d->protect = ro;
    d->changed = true;
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
    snprintf(d->path, sizeof(d->path), "%s", path);
    AMI_LOGI("DF%d: %s%s", drive, path, ro ? " (write protected)" : "");
    return true;
}

void amiga_floppy_eject(int drive)
{
    if (drive < 0 || drive >= AMIGA_FLOPPIES) {
        return;
    }
    drive_t *d = &s_drive[drive];
    flush_drive(d);
    d->data = NULL;
    d->mfm_track = -1;
    d->path[0] = 0;
    d->changed = true;
    if (dk.active && dk.drive == drive) {
        dk.active = false;
    }
}

const char *amiga_floppy_path(int drive)
{
    return drive >= 0 && drive < AMIGA_FLOPPIES && s_drive[drive].data ? s_drive[drive].path : NULL;
}

bool amiga_floppy_write_protected(int drive)
{
    return drive >= 0 && drive < AMIGA_FLOPPIES && s_drive[drive].protect;
}

bool amiga_floppy_create(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    static const uint8_t zero[512];
    bool ok = true;
    for (int i = 0; i < ADF_SIZE / 512 && ok; i++) {
        ok = fwrite(zero, 1, sizeof(zero), f) == sizeof(zero);
    }
    return fclose(f) == 0 && ok;
}

bool amiga_floppy_dirty(void)
{
    for (int i = 0; i < AMIGA_FLOPPIES; i++) {
        if (s_drive[i].any_dirty) {
            return true;
        }
    }
    return false;
}

void amiga_floppy_flush(void)
{
    for (int i = 0; i < AMIGA_FLOPPIES; i++) {
        flush_drive(&s_drive[i]);
    }
}

void amiga_floppy_status(amiga_floppy_status_t *st)
{
    st->light = -1;
    for (int i = 0; i < AMIGA_FLOPPIES; i++) {
        const drive_t *d = &s_drive[i];
        st->reads[i] = d->reads;
        st->writes[i] = d->writes;
        st->cylinder[i] = (uint8_t)d->cylinder;
        if (d->motor && st->light < 0) {
            st->light = i;
        }
    }
}
