/* The blitter's special loops against blit_area()'s own: random blits over
 * a small chip RAM (so rows wrap round its end), each done both ways from
 * the same memory and registers; the memory, the pointers, the data
 * registers, BZERO and the colour clocks compared.
 *   blit_test [cases] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_internal.h"

#define CHIP    (256 * 1024)

static uint8_t chip[CHIP], before[CHIP], after[CHIP];
static uint32_t s_rng;

void m68k_host_instruction(unsigned int pc) { (void)pc; }
void ami_host_watch_hit(uint32_t addr, int bytes, uint32_t value) { (void)addr; (void)bytes; (void)value; }

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t rn(uint32_t n)
{
    return rnd() % n;
}

extern bool blit_generic;
int blitter_test_run(void);

int main(int argc, char **argv)
{
    int cases = argc > 1 ? atoi(argv[1]) : 20000;
    s_rng = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 12345;
    ami.chip = chip;
    ami.chip_mask = CHIP - 1;
    blitter_reset();
    int failures = 0, kinds[3] = { 0 };
    for (int k = 0; k < cases && failures < 10; k++) {
        for (uint32_t i = 0; i < CHIP; i += 4) {
            uint32_t v = rn(4) ? rnd() : 0;
            memcpy(chip + i, &v, 4);
        }
        blitter_t b = { 0 };
        switch (rn(4)) {
        case 0:                             /* the clears: D alone, $00 or $FF */
            b.con0 = (uint16_t)(0x0100 | (rn(2) ? 0xFF : 0x00) | rn(16) << 12);
            kinds[0]++;
            break;
        case 1: case 2:                     /* the cookie cut */
            b.con0 = (uint16_t)(0x0FCA | rn(16) << 12);
            kinds[1]++;
            break;
        default:                            /* anything (the copy, the rest) */
            b.con0 = (uint16_t)(rn(3) ? rnd() : (0x09F0 | rn(16) << 12));
            kinds[2]++;
            break;
        }
        b.con1 = (uint16_t)((rn(16) << 12) | (rn(3) ? 0 : 0x0002) | (rn(4) ? 0 : (rnd() & 0x001C)));
        b.afwm = (uint16_t)(rn(3) ? 0xFFFF : rnd());
        b.alwm = (uint16_t)(rn(3) ? 0xFFFF : rnd());
        for (int i = 0; i < 4; i++) {
            b.pt[i] = rnd() & 0x1FFFFE;
            b.mod[i] = (int16_t)(rn(2) ? 2 * (int)rn(40) - 40 : (int16_t)(rnd() & 0xFFFE));
        }
        for (int i = 0; i < 3; i++) {
            b.dat[i] = (uint16_t)rnd();
        }
        if (!rn(4)) {
            b.pt[3] = b.pt[2] + 2 * (rn(3) - 1);    /* (D over C, or a word either side: in place) */
            b.mod[3] = b.mod[2];
        }
        b.width = rn(8) ? 1 + rn(8) : 1 + rn(rn(4) ? 64 : 2048);
        b.height = rn(8) ? 1 + rn(64) : 1 + rn(1024);
        memcpy(before, chip, CHIP);
        blit = b;
        blit_generic = false;
        int c1 = blitter_test_run();
        blitter_t r1 = blit;
        memcpy(after, chip, CHIP);
        memcpy(chip, before, CHIP);
        blit = b;
        blit_generic = true;
        int c2 = blitter_test_run();
        blitter_t r2 = blit;
        bool same = c1 == c2 && !memcmp(r1.pt, r2.pt, sizeof(r1.pt)) && !memcmp(r1.dat, r2.dat, sizeof(r1.dat)) &&
                    r1.zero == r2.zero && !memcmp(after, chip, CHIP);
        if (!same) {
            failures++;
            fprintf(stderr, "FAIL case %d: con0 %04x con1 %04x %ux%u masks %04x %04x: cycles %d %d, D %06x %06x, zero %d %d",
                    k, b.con0, b.con1, b.width, b.height, b.afwm, b.alwm, c1, c2, r1.pt[3], r2.pt[3], r1.zero, r2.zero);
            for (uint32_t i = 0; i < CHIP; i += 2) {
                if (memcmp(after + i, chip + i, 2)) {
                    fprintf(stderr, ", first word apart at %05x", i);
                    break;
                }
            }
            fprintf(stderr, "\n");
        }
    }
    printf("blit_test: %d cases (%d clears, %d cookie cuts, %d others), %d failed\n", cases, kinds[0], kinds[1], kinds[2],
           failures);
    return failures != 0;
}
