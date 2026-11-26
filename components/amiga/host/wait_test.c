/* Wait loops passing without the CPU (wait.c), on small programs in chip
 * RAM behind a stub ROM. No ROM needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_internal.h"
#include "m68k.h"

static uint8_t chip[2048 * 1024], kick[256 * 1024], rom[256 * 1024];
static amiga_model_t s_model = AMIGA_A500;  /* start()'s machine: an A500, or an A1200 (2 MB chip RAM) */
static uint32_t s_count[0x100];         /* instructions run at $1000-$11FF, by word */
static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

void m68k_host_instruction(unsigned int pc)
{
    if (pc >= 0x1000 && pc < 0x1200) {
        s_count[(pc - 0x1000) / 2]++;
    }
}

void ami_host_watch_hit(uint32_t addr, int bytes, uint32_t value)
{
    (void)addr;
    (void)bytes;
    (void)value;
}

static uint32_t count(uint32_t pc)
{
    return s_count[(pc - 0x1000) / 2];
}

/* A ROM that sets the stack, turns the overlay off and jumps to $1000 */
static void make_rom(void)
{
    static const uint16_t code[] = {
        0x4FF9, 0x0007, 0xFFF0,                 /* lea $7fff0,a7 */
        0x13FC, 0x0003, 0x00BF, 0xE201,         /* move.b #3,$bfe201 */
        0x13FC, 0x0002, 0x00BF, 0xE001,         /* move.b #2,$bfe001 (overlay off) */
        0x4EF9, 0x0000, 0x1000,                 /* jmp $1000 */
    };
    memset(kick, 0, sizeof(kick));
    kick[0] = kick[1] = 0x11;
    kick[5] = 0xF8;
    kick[7] = 0x10;                             /* reset PC $F80010 */
    kick[12] = 0x00, kick[13] = 0x22;           /* (34.5) */
    kick[15] = 0x05;
    for (unsigned i = 0; i < sizeof(code) / 2; i++) {
        kick[0x10 + 2 * i] = code[i] >> 8;
        kick[0x11 + 2 * i] = code[i] & 0xFF;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < sizeof(kick); i += 4) {
        uint32_t w = ((uint32_t)kick[i] << 24) | (kick[i + 1] << 16) | (kick[i + 2] << 8) | kick[i + 3];
        uint32_t t = sum + w;
        sum = t + (t < sum);
    }
    uint32_t fix = 0xFFFFFFFF - sum;            /* (the checksum's long, as Kickstart's) */
    for (int k = 0; k < 4; k++) {
        kick[sizeof(kick) - 24 + k] = fix >> (24 - 8 * k);
    }
}

/* A machine with the program at $1000 and the VERTB handler (if any) at
 * $1040 */
static void start(const uint16_t *main_code, int main_words, const uint16_t *handler, int handler_words)
{
    static bool tables;
    static uint8_t *opcode_tables;
    if (!tables) {
        opcode_tables = malloc(AMIGA_OPCODE_TABLES_SIZE);
        tables = true;
    }
    memset(chip, 0, sizeof(chip));
    memset(s_count, 0, sizeof(s_count));
    amiga_config_t cfg = {
        .model = s_model, .chip = chip, .chip_size = s_model == AMIGA_A1200 ? 2048 * 1024 : 512 * 1024,
        .kickstart = kick, .kickstart_size = sizeof(kick), .rom = rom, .opcode_tables = opcode_tables,
    };
    if (!amiga_init(&cfg)) {
        fprintf(stderr, "FAIL: the stub ROM\n");
        exit(1);
    }
    for (int i = 0; i < main_words; i++) {
        chip_write16(0x1000 + 2 * i, main_code[i]);
    }
    for (int i = 0; i < handler_words; i++) {
        chip_write16(0x1040 + 2 * i, handler[i]);
    }
    chip_write16(0x6C, 0x0000);                 /* the level 3 vector: $1040 */
    chip_write16(0x6E, 0x1040);
}

static uint16_t word(uint32_t a)
{
    return amiga_peek16(a);
}

/* The VERTB setup the programs share: interrupts off, VERTB on, mask 0 */
#define VERTB_ON \
    0x33FC, 0x7FFF, 0x00DF, 0xF09A,             /* move.w #$7fff,$dff09a */ \
    0x33FC, 0x7FFF, 0x00DF, 0xF09C,             /* move.w #$7fff,$dff09c */ \
    0x33FC, 0xC020, 0x00DF, 0xF09A,             /* move.w #$c020,$dff09a */ \
    0x46FC, 0x2000                              /* move.w #$2000,sr */

/* A flag the VERTB handler sets, a loop on it, then a branch to itself */
static void flag_set_by_interrupt(void)
{
    static const uint16_t main_code[] = {
        VERTB_ON,
        0x4A38, 0x0100,                         /* $101C: tst.b $100.w */
        0x67FA,                                 /* $1020: beq.s $101c */
        0x31F9, 0x00DF, 0xF006, 0x0104,         /* move.w $dff006,$104.w */
        0x60FE,                                 /* $102A: bra.s * */
    };
    static const uint16_t handler[] = {
        0x5238, 0x0100,                         /* addq.b #1,$100.w */
        0x33FC, 0x0020, 0x00DF, 0xF09C,         /* move.w #$20,$dff09c */
        0x4E73,                                 /* rte */
    };
    start(main_code, sizeof(main_code) / 2, handler, sizeof(handler) / 2);
    amiga_run_frame();
    amiga_run_frame();
    CHECK(word(0x100) >> 8 == 1, "the VERTB handler ran once (flag %04x)", word(0x100));
    CHECK(word(0x104) >> 8 == 0, "the loop ended in line 0 (VHPOSR %04x)", word(0x104));
    uint32_t waited = count(0x101C) + count(0x1020);
    CHECK(waited < 2000, "the wait passed without the CPU (%u instructions, a frame's ~10000)", (unsigned)waited);
    uint32_t idle = count(0x102A);
    for (int f = 0; f < 5; f++) {
        amiga_run_frame();
    }
    CHECK(word(0x100) >> 8 == 6, "VERTB came every frame while the CPU waited (flag %04x)", word(0x100));
    CHECK(count(0x102A) - idle < 500, "bra.s * passed without the CPU (%u)", (unsigned)(count(0x102A) - idle));
}

/* Pango Fun's wait for line 0: V8, then the vertical byte, with a VERTB
 * handler that runs past line 0 */
static void line0_wait(void)
{
    static const uint16_t main_code[] = {
        VERTB_ON,
        0x4A39, 0x00DF, 0xF005,                 /* $101C: tst.b $dff005 */
        0x66F8,                                 /* bne.s $101c */
        0x4A39, 0x00DF, 0xF006,                 /* tst.b $dff006 */
        0x66F0,                                 /* bne.s $101c */
        0x5278, 0x0106,                         /* $102C: addq.w #1,$106.w */
        0x0C39, 0x0064, 0x00DF, 0xF006,         /* $1030: cmpi.b #100,$dff006 */
        0x66F6,                                 /* bne.s $1030 */
        0x60E0,                                 /* bra.s $101c */
    };
    static const uint16_t handler[] = {
        0x303C, 0x07D0,                         /* move.w #2000,d0 */
        0x51C8, 0xFFFE,                         /* dbf d0,* (~44 lines) */
        0x33FC, 0x0020, 0x00DF, 0xF09C,         /* move.w #$20,$dff09c */
        0x4E73,                                 /* rte */
    };
    start(main_code, sizeof(main_code) / 2, handler, sizeof(handler) / 2);
    for (int f = 0; f < 12; f++) {
        amiga_run_frame();
    }
    CHECK(word(0x106) >= 10, "line 0 reached in every frame (%u of 12)", word(0x106));
    CHECK(count(0x1030) < 12 * 200, "the wait for line 100 passed without the CPU (%u)", (unsigned)count(0x1030));
}

/* CIA-B's timer A as a one-shot, its interrupt off, polled through ICR:
 * the loop ends at the underflow, not a line later */
static void cia_timer_polled(void)
{
    static const uint16_t main_code[] = {
        0x33FC, 0x7FFF, 0x00DF, 0xF09A,         /* move.w #$7fff,$dff09a */
        0x46FC, 0x2700,                         /* move.w #$2700,sr */
        0x13FC, 0x007F, 0x00BF, 0xDD00,         /* move.b #$7f,$bfdd00 (ICR: all off) */
        0x13FC, 0x0008, 0x00BF, 0xDE00,         /* move.b #$08,$bfde00 (CRA: one-shot) */
        0x13FC, 0x0000, 0x00BF, 0xD400,         /* move.b #0,$bfd400 */
        0x4A39, 0x00BF, 0xDD00,                 /* tst.b $bfdd00 */
        0x0C39, 0x0064, 0x00DF, 0xF006,         /* $102A: cmpi.b #100,$dff006 */
        0x66F6,                                 /* bne.s $102a */
        0x13FC, 0x0002, 0x00BF, 0xD500,         /* move.b #2,$bfd500 (512 ticks, started) */
        0x31F9, 0x00DF, 0xF006, 0x0110,         /* move.w $dff006,$110.w */
        0x0839, 0x0000, 0x00BF, 0xDD00,         /* $1044: btst #0,$bfdd00 */
        0x67F6,                                 /* beq.s $1044 */
        0x31F9, 0x00DF, 0xF006, 0x0112,         /* move.w $dff006,$112.w */
        0x60FE,                                 /* bra.s * */
    };
    start(main_code, sizeof(main_code) / 2, NULL, 0);
    for (int f = 0; f < 3; f++) {
        amiga_run_frame();
    }
    uint16_t a = word(0x110), b = word(0x112);
    int cck = ((b >> 8) - (a >> 8)) * 227 + (b & 0xFF) - (a & 0xFF);
    CHECK(b != 0 && cck >= 2560 && cck < 2560 + 60,
          "the ICR poll ended at the underflow: %d colour clocks for 512 E ticks (2560)", cck);
    CHECK(count(0x1044) < 60, "the ICR poll passed without the CPU (%u)", (unsigned)count(0x1044));
}

/* The loop's code changed under a kept loop while the CPU waits: a blit
 * the copper starts at line 150 writes a NOP over the branch */
static void code_changed(void)
{
    static const uint16_t main_code[] = {
        0x33FC, 0x7FFF, 0x00DF, 0xF09A,         /* move.w #$7fff,$dff09a */
        0x46FC, 0x2700,                         /* move.w #$2700,sr */
        0x33FC, 0x01F0, 0x00DF, 0xF040,         /* BLTCON0 $01f0: D = A (A off: BLTADAT) */
        0x33FC, 0x0000, 0x00DF, 0xF042,         /* BLTCON1 0 */
        0x23FC, 0xFFFF, 0xFFFF, 0x00DF, 0xF044, /* BLTAFWM, BLTALWM */
        0x33FC, 0x4E71, 0x00DF, 0xF074,         /* BLTADAT $4e71 (NOP) */
        0x23FC, 0x0000, 0x1060, 0x00DF, 0xF054, /* BLTDPT $1060 */
        0x23FC, 0x0000, 0x2000, 0x00DF, 0xF080, /* COP1LC $2000 */
        0x33FC, 0x0002, 0x00DF, 0xF02E,         /* COPCON: CDANG */
        0x33FC, 0x82C0, 0x00DF, 0xF096,         /* DMACON: DMAEN, COPEN, BLTEN */
        0x4EF8, 0x105C,                         /* jmp $105c.w */
        0x4E71, 0x4E71, 0x4E71,
        0x4A38, 0x0100,                         /* $105C: tst.b $100.w (stays 0) */
        0x67FA,                                 /* $1060: beq.s $105c */
        0x31F9, 0x00DF, 0xF006, 0x0104,         /* move.w $dff006,$104.w */
        0x60FE,                                 /* bra.s * */
    };
    static const uint16_t copper[] = {
        0x9601, 0xFFFE,                         /* wait for line 150 */
        0x0058, 0x0041,                         /* BLTSIZE: one word */
        0xFFFF, 0xFFFE,
    };
    start(main_code, sizeof(main_code) / 2, NULL, 0);
    for (unsigned i = 0; i < sizeof(copper) / 2; i++) {
        chip_write16(0x2000 + 2 * i, copper[i]);
    }
    chip_write16(0x104, 0xFFFF);
    for (int f = 0; f < 3; f++) {
        amiga_run_frame();
    }
    CHECK(word(0x1060) == 0x4E71, "the blit wrote the NOP (%04x)", word(0x1060));
    CHECK(word(0x104) >> 8 == 150, "the changed loop ended at line 150 (VHPOSR %04x)", word(0x104));
}

/* The beam wait of many games: VPOSR and VHPOSR as a long, the horizontal
 * byte masked off, compared with a line */
static void beam_wait_long(void)
{
    static const uint16_t main_code[] = {
        0x33FC, 0x7FFF, 0x00DF, 0xF09A,         /* move.w #$7fff,$dff09a */
        0x46FC, 0x2700,                         /* move.w #$2700,sr */
        0x2039, 0x00DF, 0xF004,                 /* $100C: move.l $dff004,d0 */
        0x0280, 0x0001, 0xFF00,                 /* andi.l #$1ff00,d0 */
        0xB0BC, 0x0001, 0x2C00,                 /* cmp.l #$12c00,d0 (line 300) */
        0x66EC,                                 /* bne.s $100c */
        0x31F9, 0x00DF, 0xF006, 0x0104,         /* move.w $dff006,$104.w */
        0x60FE,                                 /* bra.s * */
    };
    start(main_code, sizeof(main_code) / 2, NULL, 0);
    for (int f = 0; f < 2; f++) {
        amiga_run_frame();
    }
    CHECK(word(0x104) >> 8 == (300 & 0xFF), "the long beam wait ended at line 300 (VHPOSR %04x)", word(0x104));
    CHECK(count(0x100C) < 600, "it passed without the CPU (%u passes)", (unsigned)count(0x100C));
}

/* A compiled wait on a flag the VERTB handler sets, through the stack
 * (AmiBee's twelve instructions) */
static void compiled_flag_wait(void)
{
    static const uint16_t main_code[] = {
        VERTB_ON,
        0x4BF8, 0x0200,                         /* lea $200.w,a5 */
        0x102D, 0xFF00,                         /* $1020: move.b -$100(a5),d0 ($100) */
        0x4880,                                 /* ext.w d0 */
        0x48E7, 0x8000,                         /* movem.l d0,-(a7) */
        0x7000,                                 /* moveq #0,d0 */
        0x2200,                                 /* move.l d0,d1 */
        0x4CDF, 0x0001,                         /* movem.l (a7)+,d0 */
        0xB041,                                 /* cmp.w d1,d0 */
        0x57C0,                                 /* seq d0 */
        0x4880,                                 /* ext.w d0 */
        0x4A40,                                 /* tst.w d0 */
        0x6702,                                 /* beq.s $103e */
        0x60E2,                                 /* bra.s $1020 */
        0x31F9, 0x00DF, 0xF006, 0x0104,         /* $103E: move.w $dff006,$104.w */
        0x60FE,                                 /* bra.s * */
    };
    static const uint16_t handler[] = {
        0x5238, 0x0100,                         /* addq.b #1,$100.w */
        0x33FC, 0x0020, 0x00DF, 0xF09C,         /* move.w #$20,$dff09c */
        0x4E73,                                 /* rte */
    };
    /* (the handler at $1040 would be inside this program: moved to $1080) */
    start(main_code, sizeof(main_code) / 2, NULL, 0);
    for (unsigned i = 0; i < sizeof(handler) / 2; i++) {
        chip_write16(0x1080 + 2 * i, handler[i]);
    }
    chip_write16(0x6E, 0x1080);
    amiga_run_frame();
    amiga_run_frame();
    CHECK(word(0x100) >> 8 >= 1, "the VERTB handler ran (flag %04x)", word(0x100));
    CHECK(word(0x104) >> 8 == 0, "the compiled wait ended in line 0 (VHPOSR %04x)", word(0x104));
    CHECK(count(0x1020) < 300, "it passed without the CPU (%u passes, a frame's ~800)", (unsigned)count(0x1020));
}

/* A loop whose registers change each pass isn't a wait: it runs */
static void not_a_fixed_point(void)
{
    static const uint16_t main_code[] = {
        0x33FC, 0x7FFF, 0x00DF, 0xF09A,         /* move.w #$7fff,$dff09a */
        0x46FC, 0x2700,                         /* move.w #$2700,sr */
        0x0C39, 0x0064, 0x00DF, 0xF006,         /* $100C: cmpi.b #100,$dff006 */
        0x66F6,                                 /* bne.s $100c */
        0x72FF,                                 /* moveq #-1,d1 */
        0xE289,                                 /* $1018: lsr.l #1,d1 (32 passes: ~1.4 lines) */
        0x66FC,                                 /* bne.s $1018 */
        0x31F9, 0x00DF, 0xF006, 0x0104,         /* move.w $dff006,$104.w */
        0x60FE,                                 /* bra.s * */
    };
    start(main_code, sizeof(main_code) / 2, NULL, 0);
    chip_write16(0x104, 0xFFFF);
    amiga_run_frame();
    CHECK(count(0x1018) == 32, "the shifting loop ran its 32 passes (%u)", (unsigned)count(0x1018));
    CHECK(word(0x104) >> 8 == 101 || word(0x104) >> 8 == 102, "and went on after it (VHPOSR %04x)", word(0x104));
}

/* A wait whose pass leaves a register the code after it reads: a skip
 * sets the CPU to the pass it would run next (the line it read), not
 * leaves the line from before the skips */
static void register_after_wait(void)
{
    static const uint16_t main_code[] = {
        VERTB_ON,
        0x4A38, 0x0100,                         /* $101C: tst.b $100.w */
        0x660A,                                 /* bne.s $102c */
        0x3039, 0x00DF, 0xF006,                 /* move.w $dff006,d0 */
        0xE048,                                 /* lsr.w #8,d0 */
        0x60F0,                                 /* bra.s $101c */
        0x31C0, 0x0104,                         /* $102C: move.w d0,$104.w */
        0x60FE,                                 /* bra.s * */
    };
    static const uint16_t handler[] = {
        0x5238, 0x0100,                         /* addq.b #1,$100.w */
        0x33FC, 0x0020, 0x00DF, 0xF09C,         /* move.w #$20,$dff09c */
        0x4E73,                                 /* rte */
    };
    start(main_code, sizeof(main_code) / 2, handler, sizeof(handler) / 2);
    chip_write16(0x104, 0xFFFF);
    amiga_run_frame();
    amiga_run_frame();
    uint16_t line = word(0x104);
    CHECK(line == 0 || line == (312 & 0xFF), "the line the wait read last: 312 or 0 (%u)", line);
    CHECK(count(0x101C) < 300, "it passed without the CPU (%u passes)", (unsigned)count(0x101C));
}

/* A compare that takes the horizontal byte in: no skip, the loop ends at
 * its position */
static void horizontal_compare(void)
{
    static const uint16_t main_code[] = {
        0x33FC, 0x7FFF, 0x00DF, 0xF09A,         /* move.w #$7fff,$dff09a */
        0x46FC, 0x2700,                         /* move.w #$2700,sr */
        0x3039, 0x00DF, 0xF006,                 /* $100C: move.w $dff006,d0 */
        0x0C40, 0x6480,                         /* cmpi.w #$6480,d0 (line 100, h $80) */
        0x65F4,                                 /* bcs.s $100c */
        0x31F9, 0x00DF, 0xF006, 0x0104,         /* move.w $dff006,$104.w */
        0x60FE,                                 /* bra.s * */
    };
    start(main_code, sizeof(main_code) / 2, NULL, 0);
    amiga_run_frame();
    uint16_t v = word(0x104);
    CHECK(v >> 8 == 100 && (v & 0xFF) >= 0x80 && (v & 0xFF) < 0x80 + 24,
          "the horizontal compare ended at line 100, h $80 (VHPOSR %04x)", v);
}

/* The 68000's prefetch (mem.c's, for the 68000 only): an instruction that
 * writes over the next one runs the old word, also when the write follows
 * an extension word; a taken branch fetches the new one. The 68EC020 runs
 * the new word (Voyage's trace decoder needs the 68000's way) */
static void prefetch(amiga_model_t model)
{
    static const uint16_t main_code[] = {
        0x33FC, 0x7FFF, 0x00DF, 0xF09A,         /* move.w #$7fff,$dff09a */
        0x46FC, 0x2700,                         /* move.w #$2700,sr */
        0x43F8, 0x0100,                         /* lea $100.w,a1 */
        0x41F8, 0x101A,                         /* lea $101a.w,a0 */
        0x303C, 0x7202,                         /* move.w #$7202,d0 */
        0x3080,                                 /* $1018: move.w d0,(a0) */
        0x7201,                                 /* $101A: moveq #1,d1 (becomes moveq #2,d1) */
        0x32C1,                                 /* move.w d1,(a1)+ */
        0xB2FC, 0x0104,                         /* cmpa.w #$104,a1 */
        0x66F6,                                 /* bne.s $101a */
        0x41F8, 0x102C,                         /* lea $102c.w,a0 */
        0x30BC, 0x7403,                         /* $1028: move.w #$7403,(a0) */
        0x7401,                                 /* $102C: moveq #1,d2 (becomes moveq #3,d2) */
        0x31C2, 0x0104,                         /* move.w d2,$104.w */
        0x60FE,                                 /* bra.s * */
    };
    s_model = model;
    start(main_code, sizeof(main_code) / 2, NULL, 0);
    s_model = AMIGA_A500;
    amiga_run_frame();
    const char *cpu = model == AMIGA_A1200 ? "68EC020" : "68000";
    uint16_t old = model == AMIGA_A1200 ? 2 : 1;
    CHECK(word(0x100) == old, "%s: the word after a write over it (%u, expected %u)", cpu, word(0x100), old);
    CHECK(word(0x102) == 2, "%s: after a branch back to it (%u, expected 2)", cpu, word(0x102));
    old = model == AMIGA_A1200 ? 3 : 1;
    CHECK(word(0x104) == old, "%s: written after an extension word (%u, expected %u)", cpu, word(0x104), old);
}

int main(void)
{
    make_rom();
    prefetch(AMIGA_A500);
    prefetch(AMIGA_A1200);
    flag_set_by_interrupt();
    line0_wait();
    cia_timer_polled();
    code_changed();
    beam_wait_long();
    compiled_flag_wait();
    not_a_fixed_point();
    register_after_wait();
    horizontal_compare();
    puts(failures ? "wait loop checks failed" : "all wait loop checks passed");
    return failures != 0;
}
