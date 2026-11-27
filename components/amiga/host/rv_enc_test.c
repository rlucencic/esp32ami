/* rv.h's encodings against the GNU assembler: prints each instruction as
 * assembler text and as rv.h's word (rv_enc_test.py assembles the text
 * and compares). */
#include <stdio.h>
#include <string.h>
#include "rv.h"

static const char *const names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static uint32_t buf[64];
static rv_t r;

static void reset(void)
{
    memset(&r, 0, sizeof(r));
    r.start = r.p = buf;
    r.end = buf + 64;
}

#define ONE(text, ...) do { reset(); __VA_ARGS__; printf("%08x\t%s\n", buf[0], text); } while (0)

int main(void)
{
    char t[100];
    static const int regs[] = { 1, 5, 8, 9, 10, 18, 27, 31 };
    static const int32_t imms[] = { 0, 1, -1, 7, 2047, -2048, 100, -1000 };
    for (int a = 0; a < 8; a++) {
        for (int b = 0; b < 8; b++) {
            int rd = regs[a], rs = regs[b], rt = regs[(a + b + 3) & 7];
            int32_t im = imms[(a * 3 + b) & 7];
#define R3(op) snprintf(t, sizeof(t), #op " %s,%s,%s", names[rd], names[rs], names[rt]); ONE(t, rv_##op(&r, rd, rs, rt))
            R3(add); R3(sub); R3(sll); R3(slt); R3(sltu); R3(xor); R3(srl); R3(sra); R3(or); R3(and);
            R3(mul); R3(mulh); R3(mulhu); R3(div); R3(divu); R3(rem); R3(remu);
#define I2(op) snprintf(t, sizeof(t), #op " %s,%s,%d", names[rd], names[rs], im); ONE(t, rv_##op(&r, rd, rs, im))
            I2(addi); I2(slti); I2(sltiu); I2(xori); I2(ori); I2(andi);
            int sh = (a * 5 + b) & 31;
            snprintf(t, sizeof(t), "slli %s,%s,%d", names[rd], names[rs], sh); ONE(t, rv_slli(&r, rd, rs, sh));
            snprintf(t, sizeof(t), "srli %s,%s,%d", names[rd], names[rs], sh); ONE(t, rv_srli(&r, rd, rs, sh));
            snprintf(t, sizeof(t), "srai %s,%s,%d", names[rd], names[rs], sh); ONE(t, rv_srai(&r, rd, rs, sh));
#define LD(op) snprintf(t, sizeof(t), #op " %s,%d(%s)", names[rd], im, names[rs]); ONE(t, rv_##op(&r, rd, im, rs))
            LD(lb); LD(lh); LD(lw); LD(lbu); LD(lhu);
#define ST(op) snprintf(t, sizeof(t), #op " %s,%d(%s)", names[rd], im, names[rs]); ONE(t, rv_##op(&r, rd, im, rs))
            ST(sb); ST(sh); ST(sw);
            snprintf(t, sizeof(t), "jalr %s,%d(%s)", names[rd], im, names[rs]); ONE(t, rv_jalr(&r, rd, rs, im));
            uint32_t hi = (uint32_t)(a * 0x12345 + b * 0x6789) & 0xFFFFF;
            snprintf(t, sizeof(t), "lui %s,0x%x", names[rd], hi); ONE(t, rv_lui(&r, rd, hi));
        }
    }
    /* branches and jumps: an offset from the instruction (the label's) */
    static const int32_t offs[] = { 8, -8, 4094, -4096, 2048, -2050, 100, -260 };
    static const int32_t joffs[] = { 8, -8, 1048574, -1048576, 4096, -40000, 2048, 300000 };
    static const char *const bn[] = { "beq", "bne", "", "", "blt", "bge", "bltu", "bgeu" };
    for (int k = 0; k < 8; k++) {
        for (int f3 = 0; f3 < 8; f3++) {
            if (!bn[f3][0]) {
                continue;
            }
            int rs = regs[k], rt = regs[(k + f3) & 7];
            snprintf(t, sizeof(t), "%s %s,%s,.%+d", bn[f3], names[rs], names[rt], offs[k]);
            ONE(t, buf[0] = rv_b(offs[k], rt, rs, (uint32_t)f3));
        }
        snprintf(t, sizeof(t), "jal %s,.%+d", names[regs[k]], joffs[k]);
        ONE(t, buf[0] = rv_j(joffs[k], regs[k]));
    }
    return 0;
}
