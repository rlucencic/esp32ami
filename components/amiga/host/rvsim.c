/* The host's runner of the translated blocks: an RV32IM interpreter for
 * the code jit_rv.c writes, so the host checks the very code the board
 * runs. A jump out of the code arena is a call of the host function at
 * that address (a0-a3 in, a0 out; the caller-saved registers come back
 * as garbage, as a callee may leave them). Every address the code uses
 * must fit in 32 bits: the build is not position independent (code and
 * globals low) and the heap stays in brk's low memory (low_heap()).
 */
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((constructor)) static void low_heap(void)
{
    mallopt(M_MMAP_MAX, 0);
}

static uint32_t s_lo, s_hi;                 /* the code arena */
static uint32_t s_stack[4096];

void rvsim_arena(const void *base, size_t size)
{
    s_lo = (uint32_t)(uintptr_t)base;
    s_hi = s_lo + (uint32_t)size;
}

static inline uint32_t rd32(uint32_t a)
{
    uint32_t v;
    memcpy(&v, (const void *)(uintptr_t)a, 4);
    return v;
}

static __attribute__((noreturn)) void bad(uint32_t pc, uint32_t w)
{
    fprintf(stderr, "rvsim: bad instruction %08x at %08x (arena %08x-%08x, +%x)\n", w, pc, s_lo, s_hi, pc - s_lo);
    abort();
}

typedef uint64_t (*host_fn)(uint64_t, uint64_t, uint64_t, uint64_t);

/* Runs the function at entry; its a0 */
uint32_t rvsim_call(uint32_t entry)
{
    uint32_t x[32] = { 0 };
    x[2] = (uint32_t)(uintptr_t)&s_stack[4096];
    x[1] = 0;                               /* (a return to 0: back here) */
    uint32_t pc = entry;
    for (;;) {
        if (pc < s_lo || pc >= s_hi) {
            if (pc == 0) {
                return x[10];
            }
            /* a host function, then back to the return address */
            uint32_t r = (uint32_t)((host_fn)(uintptr_t)pc)(x[10], x[11], x[12], x[13]);
            static const int clobber[] = { 5, 6, 7, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31 };
            for (unsigned i = 0; i < sizeof(clobber) / sizeof(clobber[0]); i++) {
                x[clobber[i]] = 0xDEADBEEFu ^ (uint32_t)i;
            }
            x[10] = r;
            pc = x[1];
            continue;
        }
        uint32_t w = rd32(pc), op = w & 0x7F, rd = (w >> 7) & 31, f3 = (w >> 12) & 7;
        uint32_t rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31, f7 = w >> 25;
        uint32_t a = x[rs1], b = x[rs2];
        int32_t ii = (int32_t)w >> 20;
        uint32_t next = pc + 4, v = 0;
        int wr = 1;
        switch (op) {
        case 0x37:                          /* LUI */
            v = w & 0xFFFFF000;
            break;
        case 0x17:                          /* AUIPC */
            v = pc + (w & 0xFFFFF000);
            break;
        case 0x6F: {                        /* JAL */
            int32_t off = (int32_t)(((w >> 31) & 1) << 20 | ((w >> 12) & 0xFF) << 12 | ((w >> 20) & 1) << 11 |
                                    ((w >> 21) & 0x3FF) << 1);
            off = off << 11 >> 11;
            v = pc + 4;
            next = pc + (uint32_t)off;
            break;
        }
        case 0x67:                          /* JALR */
            v = pc + 4;
            next = (a + (uint32_t)ii) & ~1u;
            break;
        case 0x63: {                        /* branches */
            int32_t off = (int32_t)(((w >> 31) & 1) << 12 | ((w >> 7) & 1) << 11 | ((w >> 25) & 0x3F) << 5 |
                                    ((w >> 8) & 0xF) << 1);
            off = off << 19 >> 19;
            bool t;
            switch (f3) {
            case 0: t = a == b; break;
            case 1: t = a != b; break;
            case 4: t = (int32_t)a < (int32_t)b; break;
            case 5: t = (int32_t)a >= (int32_t)b; break;
            case 6: t = a < b; break;
            case 7: t = a >= b; break;
            default: bad(pc, w);
            }
            if (t) {
                next = pc + (uint32_t)off;
            }
            wr = 0;
            break;
        }
        case 0x03: {                        /* loads */
            const void *m = (const void *)(uintptr_t)(a + (uint32_t)ii);
            switch (f3) {
            case 0: v = (uint32_t)(int32_t)*(const int8_t *)m; break;
            case 1: { int16_t h; memcpy(&h, m, 2); v = (uint32_t)(int32_t)h; break; }
            case 2: memcpy(&v, m, 4); break;
            case 4: v = *(const uint8_t *)m; break;
            case 5: { uint16_t h; memcpy(&h, m, 2); v = h; break; }
            default: bad(pc, w);
            }
            break;
        }
        case 0x23: {                        /* stores */
            int32_t off = (int32_t)(((w >> 25) << 5) | ((w >> 7) & 31)) << 20 >> 20;
            void *m = (void *)(uintptr_t)(a + (uint32_t)off);
            switch (f3) {
            case 0: *(uint8_t *)m = (uint8_t)b; break;
            case 1: { uint16_t h = (uint16_t)b; memcpy(m, &h, 2); break; }
            case 2: memcpy(m, &b, 4); break;
            default: bad(pc, w);
            }
            wr = 0;
            break;
        }
        case 0x13:                          /* immediate arithmetic */
            switch (f3) {
            case 0: v = a + (uint32_t)ii; break;
            case 1: v = a << (rs2 & 31); break;
            case 2: v = (int32_t)a < ii; break;
            case 3: v = a < (uint32_t)ii; break;
            case 4: v = a ^ (uint32_t)ii; break;
            case 5: v = (w >> 30) & 1 ? (uint32_t)((int32_t)a >> (rs2 & 31)) : a >> (rs2 & 31); break;
            case 6: v = a | (uint32_t)ii; break;
            case 7: v = a & (uint32_t)ii; break;
            }
            break;
        case 0x33:                          /* register arithmetic */
            if (f7 == 1) {
                switch (f3) {
                case 0: v = a * b; break;
                case 1: v = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
                case 2: v = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); break;
                case 3: v = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
                case 4: v = b == 0 ? 0xFFFFFFFFu : (a == 0x80000000u && b == 0xFFFFFFFFu) ? a
                          : (uint32_t)((int32_t)a / (int32_t)b); break;
                case 5: v = b == 0 ? 0xFFFFFFFFu : a / b; break;
                case 6: v = b == 0 ? a : (a == 0x80000000u && b == 0xFFFFFFFFu) ? 0
                          : (uint32_t)((int32_t)a % (int32_t)b); break;
                case 7: v = b == 0 ? a : a % b; break;
                }
            } else {
                switch (f3) {
                case 0: v = f7 ? a - b : a + b; break;
                case 1: v = a << (b & 31); break;
                case 2: v = (int32_t)a < (int32_t)b; break;
                case 3: v = a < b; break;
                case 4: v = a ^ b; break;
                case 5: v = f7 ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
                case 6: v = a | b; break;
                case 7: v = a & b; break;
                }
            }
            break;
        default:
            bad(pc, w);
        }
        if (wr && rd) {
            x[rd] = v;
        }
        pc = next;
    }
}
