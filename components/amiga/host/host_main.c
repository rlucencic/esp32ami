/*
 * Run the Amiga on the host: boot a Kickstart for a number of frames and
 * write the last picture as a PPM image.
 *
 *   ./ami_host <kickstart.rom | folder of them> [frames] [out.ppm]
 *
 * An RP9 in AMI_DF0 or AMI_HD0 without AMI_MODEL: the machine its manifest
 * names (model, memory; given a folder, its Kickstart by FS-UAE's names,
 * amiga-os-130.rom ...).
 *
 * Environment: AMI_CHIP_KB (512), AMI_SLOW_KB (512; 0 for none), AMI_FAST_KB
 * (0; 1024-8192: an autoconfig board), AMI_ECS,
 * AMI_NTSC, AMI_SERIAL (print what's sent on the serial port), AMI_EXT=<rom>
 * (AROS's extension ROM at $E00000),
 * AMI_SHOTS=<n> (a picture every n frames, <out>-<frame>.ppm; AMI_SHOTFROM=<frame>:
 * from there on), AMI_LOG=<n>
 * (the machine's state every n frames), AMI_LOGFROM=<frame> (every frame
 * from there), AMI_RING=<n> (the last n instructions at the end),
 * AMI_STOP=<pc>[:<frame>], AMI_TRACE=<pc>:<n> (+ AMI_TRACE_FRAME),
 * AMI_WATCH=<addr hex>[:<frame>] (stop on a CPU write that covers the
 * address, as AMI_STOP), AMI_NEGY=<pc> (log each visit where D1.w < 0),
 * AMI_PROJ=<pc> (log projected x,y outside 0..319/0..255),
 * AMI_PROJENTRY=<pc> (log D5/D6/A2 once a frame at projection entry),
 * AMI_ANG=<addr> (with PROJENTRY: peek angle/Z words there), AMI_PROJFROM/TO,
 * AMI_DIS=<addr hex>:<n> (disassemble and stop),
 * AMI_EXC (exceptions other than interrupts; "all" with them),
 * AMI_EXCSTOP=<vector>[:<frame>] (stop at that exception, as AMI_STOP), AMI_LOGREGS
 * (AMI_LOG's lines with the display's registers),
 * AMI_DEBUG=<port> (serve the debugger, in real time), AMI_MODEL (0 A500, 1
 * A2000, 2 A600), AMI_HD0, AMI_HD1 (HDF images, written: use copies),
 * AMI_NEWHD=<path>:<MB> (a blank hard disk made first), AMI_DF0, AMI_DF1
 * (ADF images in the drives: written to!), AMI_INSERT=<frame>:<drive>:<adf>
 * [;...] (later; an empty name ejects), AMI_INPUT=<frame>:m:<dx>:<dy>:<buttons>
 * | <frame>:k:<code hex>:<1|0> | <frame>:j:<bits hex> | <frame>:t:<text>
 * [;...] (the mouse, a key, the joystick at that frame, text typed from
 * then on, "\n" for Return; a mouse move of more than 100 is spread over
 * the frames after), AMI_WAV=<file> (the sound), AMI_DISKLOG
 * (each disk DMA's start, its end with a checksum of its words, each
 * change of the drives' lines on CIA-B; with AMI_DISKDUMP=<folder> each
 * read's words as read-<n>.bin), AMI_DISKSEED=<n> (another sequence of the
 * drives' speeds from turn to turn, as another drive would have),
 * AMI_DUMP=<addr hex>:<bytes>:<file> (memory at the end).
 * Where two runs part: AMI_WATCHLOG (with AMI_WATCH: every covering write
 * a line, the run goes on), AMI_STOPDIS=<addr hex>:<n> (disassemble memory
 * as it is where AMI_STOP or AMI_WATCH stops), AMI_PCLOG=<file>:<frame>
 * [:<lo>-<hi>] (each instruction's PC and frame: tools/pcdiff.py),
 * AMI_PCCOVER=<file> (each address executed, at the end, in the order
 * first run: tools/smoke.py's replays).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "amiga.h"
#include "m68k.h"
#include "emudbg.h"

static uint32_t s_frame;
static uint64_t s_insns;
/* AMI_PCCOVER: per even address, the instruction count and frame of its
 * first execution (0: never); calloc'd, untouched pages cost nothing */
static uint32_t *s_cover_first, *s_cover_frame;

static void disassemble(uint32_t pc, int count)
{
    char buf[100];
    for (int i = 0; i < count; i++) {
        unsigned int len = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("  $%06lx  %s\n", (unsigned long)pc, buf);
        pc += len;
    }
}

#define RING_SIZE 4096
static uint32_t s_ring[RING_SIZE];
static uint32_t s_ring_pos;

static void show_ring(int n)
{
    if (n > RING_SIZE) {
        n = RING_SIZE;
    }
    for (uint32_t i = s_ring_pos - n; i != s_ring_pos; i++) {
        disassemble(s_ring[i % RING_SIZE], 1);
    }
}

/* The last instructions and the registers, then the end.
 * Re-entry (disassemble reading memory that faults into AMI_EXCSTOP) just exits.
 */
static void stop_here(void)
{
    static int stopping;
    if (stopping) {
        exit(0);
    }
    stopping = 1;
    show_ring(60);
    printf("D0-7:");
    for (int r = M68K_REG_D0; r <= M68K_REG_D7; r++) {
        printf(" %08x", m68k_get_reg(NULL, r));
    }
    printf("\nA0-7:");
    for (int r = M68K_REG_A0; r <= M68K_REG_A7; r++) {
        printf(" %08x", m68k_get_reg(NULL, r));
    }
    printf("\n");
    /* AMI_STOPDIS=<addr hex>:<n>: memory as it is now, disassembled (code
     * a game loaded: AMI_DIS sees memory at power-on) */
    if (getenv("AMI_STOPDIS")) {
        unsigned pc = 0, count = 20;
        sscanf(getenv("AMI_STOPDIS"), "%x:%u", &pc, &count);
        printf("-- $%06x now:\n", pc);
        disassemble(pc, count);
    }
    exit(0);
}

/* mem.c: AMI_WATCH hit (a CPU write covering the address); with
 * AMI_WATCHLOG set each hit is a line (and A7) and the run goes on */
void ami_host_watch_hit(uint32_t addr, int bytes, uint32_t value)
{
    int keep_going = getenv("AMI_WATCHLOG") != NULL;
    printf("-- write %d bytes %0*x to $%06x at PC $%06x (frame %lu)%s",
           bytes, bytes <= 1 ? 2 : bytes <= 2 ? 4 : 8, (unsigned)value,
           (unsigned)addr,
           m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF,
           (unsigned long)s_frame, keep_going ? "" : ", after:\n");
    if (keep_going) {
        printf(", A7 $%06x\n", m68k_get_reg(NULL, M68K_REG_A7) & 0xFFFFFF);
        return;
    }
    stop_here();
}

static uint64_t s_opstat[65536];
static uint32_t s_opstat_pc[65536];

/* At exit: per opcode its count, share and an instruction with it (the
 * last one seen), most first */
static int opstat_order(const void *a, const void *b)
{
    uint64_t x = s_opstat[*(const uint16_t *)a], y = s_opstat[*(const uint16_t *)b];
    return x > y ? -1 : x < y;
}

static void opstat_write(void)
{
    static uint16_t ops[65536];
    uint64_t total = 0;
    int n = 0;
    for (int op = 0; op < 65536; op++) {
        if (s_opstat[op]) {
            ops[n++] = (uint16_t)op;
            total += s_opstat[op];
        }
    }
    if (!total) {
        return;
    }
    qsort(ops, n, sizeof(ops[0]), opstat_order);
    printf("opstat: %llu instructions, %d opcodes\n", (unsigned long long)total, n);
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        char text[100];
        m68k_disassemble(text, s_opstat_pc[ops[i]], M68K_CPU_TYPE_68EC020);
        sum += s_opstat[ops[i]];
        printf("opstat %04x %10llu %5.2f%% %6.2f%%  %s\n", ops[i], (unsigned long long)s_opstat[ops[i]],
               100.0 * s_opstat[ops[i]] / total, 100.0 * sum / total, text);
    }
}

void m68k_host_instruction(unsigned int pc)
{
    s_insns++;
    s_ring[s_ring_pos++ % RING_SIZE] = pc;

    static uint32_t stop_pc = 1;
    static uint32_t stop_after;
    if (stop_pc == 1) {
        const char *e = getenv("AMI_STOP");
        stop_pc = e ? strtoul(e, NULL, 16) : 0;
        stop_after = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
    }
    if (stop_pc && pc == stop_pc && s_frame >= stop_after) {
        printf("-- reached $%06x at frame %lu after:\n", pc, (unsigned long)s_frame);
        stop_here();
    }

    /* AMI_NEGY=<pc>: log each visit where D1.w is negative (Under Pressure
     * line drawer's mulu.w D5,D1 treats Y unsigned). */
    static uint32_t negy_pc = 1;
    if (negy_pc == 1) {
        const char *e = getenv("AMI_NEGY");
        negy_pc = e ? strtoul(e, NULL, 16) : 0;
    }
    if (negy_pc && pc == negy_pc) {
        uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
        if ((int16_t)(d1 & 0xFFFF) < 0) {
            static int negy_n;
            uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF;
            uint32_t a5 = m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF;
            printf("-- neg Y at $%06x frame %lu: D0=%d D1=%d D2=%d D3=%d "
                   "A0 $%06x A1 $%06x A2 $%06x A5 $%06x center %d,%d\n",
                   pc, (unsigned long)s_frame,
                   (int16_t)(m68k_get_reg(NULL, M68K_REG_D0) & 0xFFFF),
                   (int16_t)(d1 & 0xFFFF),
                   (int16_t)(m68k_get_reg(NULL, M68K_REG_D2) & 0xFFFF),
                   (int16_t)(m68k_get_reg(NULL, M68K_REG_D3) & 0xFFFF),
                   m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF,
                   a1,
                   m68k_get_reg(NULL, M68K_REG_A2) & 0xFFFFFF,
                   a5,
                   (int16_t)amiga_peek16(a5),
                   (int16_t)amiga_peek16(a5 + 2));
            if (negy_n++ == 0) {
                unsigned int x = 0x224a2e;
                printf("-- draw entry disasm:\n");
                for (int i = 0; i < 36; i++) {
                    char buf[128];
                    unsigned int n = m68k_disassemble(buf, x, m68k_get_reg(NULL, M68K_REG_CPU_TYPE));
                    printf("  $%06x  %s\n", x, buf);
                    x += n;
                }
                printf("-- edges @ $2f0438:");
                for (int i = 0; i < 16; i++) {
                    printf(" %04x", amiga_peek16(0x2f0438 + i * 2));
                }
                printf("\n-- center words @ $224b54: %d,%d\n",
                       (int16_t)amiga_peek16(0x224b54),
                       (int16_t)amiga_peek16(0x224b56));
                printf("-- A3 verts @ $5e0094 (x,y):");
                for (int i = 0; i < 12; i++) {
                    uint32_t v = 0x5e0094 + i * 6;
                    printf(" (%d,%d)",
                           (int16_t)amiga_peek16(v),
                           (int16_t)amiga_peek16(v + 2));
                }
                printf("\n");
            }
            fflush(stdout);
        }
    }

    /* AMI_PROJ=<pc>: log screen-center A5 and projected x,y after add (Under Pressure).
     * Use the store PC ($224b3e). AMI_PROJENTRY=<pc> logs matrix/Z once a frame
     * at the projection loop entry ($224ae2). */
    static uint32_t proj_pc = 1, proj_entry = 1;
    if (proj_pc == 1) {
        const char *e = getenv("AMI_PROJ");
        proj_pc = e ? strtoul(e, NULL, 16) : 0;
        e = getenv("AMI_PROJENTRY");
        proj_entry = e ? strtoul(e, NULL, 16) : 0;
    }
    if (proj_entry && pc == proj_entry) {
        static uint32_t last_frame = ~0u;
        static uint32_t ang_base = 1;
        static int from_frame, to_frame;
        if (ang_base == 1) {
            const char *e = getenv("AMI_ANG");
            ang_base = e ? strtoul(e, NULL, 16) : 0;
            e = getenv("AMI_PROJFROM");
            from_frame = e ? atoi(e) : 0;
            e = getenv("AMI_PROJTO");
            to_frame = e ? atoi(e) : 0;
        }
        if (s_frame != last_frame &&
            (!from_frame || (int)s_frame >= from_frame) &&
            (!to_frame || (int)s_frame <= to_frame)) {
            last_frame = (uint32_t)s_frame;
            printf("-- proj entry frame %lu: D5 %08x D6 %08x A2 %06x",
                   (unsigned long)s_frame,
                   m68k_get_reg(NULL, M68K_REG_D5),
                   m68k_get_reg(NULL, M68K_REG_D6),
                   m68k_get_reg(NULL, M68K_REG_A2) & 0xFFFFFF);
            if (ang_base) {
                printf(" ang %04x %04x Z %04x center %d,%d",
                       amiga_peek16(ang_base), amiga_peek16(ang_base + 2),
                       amiga_peek16(ang_base + 4),
                       (int16_t)amiga_peek16(0x224b54),
                       (int16_t)amiga_peek16(0x224b56));
            }
            printf("\n");
            fflush(stdout);
        }
    }
    if (proj_pc && pc == proj_pc) {
        int16_t x = (int16_t)(m68k_get_reg(NULL, M68K_REG_D0) & 0xFFFF);
        int16_t y = (int16_t)(m68k_get_reg(NULL, M68K_REG_D1) & 0xFFFF);
        if (y < 0 || y > 255 || x < 0 || x > 319) {
            uint32_t a5 = m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF;
            uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4) & 0xFFFFFF;
            uint32_t a2 = m68k_get_reg(NULL, M68K_REG_A2) & 0xFFFFFF;
            /* Rotated X/Y/Z.l were just stored at A4-12; D3 is the divs divisor. */
            printf("-- proj out at $%06x frame %lu: x=%d y=%d center %d,%d "
                   "D2 %08x D3 %d Z.w %d A1 $%06x rot %d,%d,%d\n",
                   pc, (unsigned long)s_frame, x, y,
                   (int16_t)amiga_peek16(a5), (int16_t)amiga_peek16(a5 + 2),
                   m68k_get_reg(NULL, M68K_REG_D2),
                   (int16_t)(m68k_get_reg(NULL, M68K_REG_D3) & 0xFFFF),
                   (int16_t)(a2 & 0xFFFF),
                   m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF,
                   (int32_t)(((uint32_t)amiga_peek16(a4 - 12) << 16) |
                             amiga_peek16(a4 - 10)),
                   (int32_t)(((uint32_t)amiga_peek16(a4 - 8) << 16) |
                             amiga_peek16(a4 - 6)),
                   (int32_t)(((uint32_t)amiga_peek16(a4 - 4) << 16) |
                             amiga_peek16(a4 - 2)));
            fflush(stdout);
        }
    }

    static uint32_t trace_pc = 1;
    static int trace_n, trace_left, trace_from;
    if (trace_pc == 1) {
        const char *e = getenv("AMI_TRACE");
        trace_pc = e ? strtoul(e, NULL, 16) : 0;
        trace_n = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 100;
        e = getenv("AMI_TRACE_FRAME");
        trace_from = e ? atoi(e) : 0;
    }
    if (trace_pc && pc == trace_pc && trace_left == 0 &&
        (!trace_from || (int)s_frame >= trace_from)) {
        trace_left = trace_n;
    }
    if (trace_left > 0) {
        trace_left--;
        disassemble(pc, 1);
    }

    /* AMI_PCLOG=<file>:<frame>[:<lo hex>-<hi hex>]: each instruction's PC
     * from that frame on (in that range: a game's own code, not the
     * ROM's) and its frame, two 32-bit words in host order, to compare
     * two runs and find where they part (tools/pcdiff.py) */
    static FILE *pclog;
    static int pclog_from = -1;
    static uint32_t pclog_lo, pclog_hi = 0xFFFFFFFF;
    if (pclog_from == -1) {
        pclog_from = -2;
        const char *e = getenv("AMI_PCLOG");
        char path[256];
        int from = 0;
        if (e && sscanf(e, "%255[^:]:%d", path, &from) >= 1) {
            const char *range = strchr(e, ':') ? strchr(strchr(e, ':') + 1, ':') : NULL;
            if (range) {
                sscanf(range + 1, "%x-%x", &pclog_lo, &pclog_hi);
            }
            pclog = fopen(path, "wb");
            pclog_from = pclog ? from : -2;
        }
    }
    if (pclog && (int)s_frame >= pclog_from && pc >= pclog_lo && pc < pclog_hi) {
        static int cyc = -1;
        if (cyc < 0) {
            cyc = getenv("AMI_PCLOGCYC") != NULL;   /* (and the cycles left: a third word) */
        }
        extern int m68ki_remaining_cycles;
        uint32_t rec[3] = { pc, (uint32_t)s_frame, (uint32_t)m68ki_remaining_cycles };
        fwrite(rec, 4, cyc ? 3 : 2, pclog);
    }

    /* AMI_OPSTAT=<from>:<to>: each opcode's count in those frames (the
     * translating core's choice of instructions: opstat_write()) */
    static int opstat_from = -1, opstat_to;
    if (opstat_from == -1) {
        const char *e = getenv("AMI_OPSTAT");
        opstat_from = -2;
        if (e && sscanf(e, "%d:%d", &opstat_from, &opstat_to) != 2) {
            opstat_from = -2;
        }
    }
    if (opstat_from >= 0 && (int)s_frame >= opstat_from && (int)s_frame < opstat_to) {
        uint16_t op = amiga_peek16(pc);
        s_opstat[op]++;
        s_opstat_pc[op] = pc;
    }

    /* AMI_PCCOVER=<file>: each address executed, once (tools/smoke.py's
     * replays) */
    if (s_cover_first) {
        uint32_t k = (pc & 0xFFFFFF) >> 1;
        if (!s_cover_first[k]) {
            s_cover_first[k] = (uint32_t)s_insns;
            s_cover_frame[k] = (uint32_t)s_frame;
        }
    }
}

/* At exit: "<pc> <frame> <instruction>" per address, in the order first
 * executed */
static int cover_order(const void *a, const void *b)
{
    uint32_t x = s_cover_first[*(const uint32_t *)a], y = s_cover_first[*(const uint32_t *)b];
    return x < y ? -1 : x > y;
}

static void cover_write(void)
{
    FILE *f = fopen(getenv("AMI_PCCOVER"), "w");
    if (!f) {
        return;
    }
    uint32_t n = 0;
    for (uint32_t k = 0; k < (1u << 23); k++) {
        n += s_cover_first[k] != 0;
    }
    uint32_t *list = malloc(n * sizeof(uint32_t));
    n = 0;
    for (uint32_t k = 0; k < (1u << 23); k++) {
        if (s_cover_first[k]) {
            list[n++] = k;
        }
    }
    qsort(list, n, sizeof(uint32_t), cover_order);
    for (uint32_t i = 0; i < n; i++) {
        fprintf(f, "%06x %u %u\n", list[i] << 1, s_cover_frame[list[i]], s_cover_first[list[i]]);
    }
    fclose(f);
    free(list);
}

static void exception(unsigned int vector)
{
    static int mode = -1;
    static unsigned stop_vector, stop_after;
    if (mode < 0) {
        const char *e = getenv("AMI_EXC");
        mode = e ? (strcmp(e, "all") == 0 ? 2 : 1) : 0;
        e = getenv("AMI_EXCSTOP");
        if (e) {
            sscanf(e, "%u:%u", &stop_vector, &stop_after);
        }
    }
    if (stop_vector && vector == stop_vector && s_frame >= stop_after) {
        /* Regs only: show_ring() can re-enter here via a faulting disassemble. */
        printf("-- exception %u at PC $%06x (frame %lu), SR %04x\n", vector,
               m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF, (unsigned long)s_frame,
               m68k_get_reg(NULL, M68K_REG_SR));
        printf("D0-7:");
        for (int r = M68K_REG_D0; r <= M68K_REG_D7; r++) {
            printf(" %08x", m68k_get_reg(NULL, r));
        }
        printf("\nA0-7:");
        for (int r = M68K_REG_A0; r <= M68K_REG_A7; r++) {
            printf(" %08x", m68k_get_reg(NULL, r));
        }
        printf("\n");
        fflush(stdout);
        exit(0);
    }
    if (mode == 0 || (mode == 1 && vector >= 24 && vector < 32)) {
        return;
    }
    printf("-- exception %u at PC $%06x (frame %lu), SR %04x, #%llu\n", vector,
           m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF, (unsigned long)s_frame,
           m68k_get_reg(NULL, M68K_REG_SR), (unsigned long long)s_insns);
}

static void disk_dma(uint16_t len, uint16_t sync, uint16_t adkcon, int cylinder, int side, uint32_t pt)
{
    int head = amiga_floppy_head();
    printf("-- disk %s %u words, sync %04x%s, cylinder %d side %d, to $%06x (frame %lu), #%llu, head %d.%03d, "
           "PC $%06x\n",
           (len & 0x4000) ? "write" : "read", len & 0x3FFF, sync, (adkcon & 0x0400) ? "" : " (not used)",
           cylinder, side, (unsigned)pt, (unsigned long)s_frame, (unsigned long long)s_insns, head / 224,
           head % 224, m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF);
}

/* A DMA's words (in memory now: read from the disk, or written to it) as
 * a checksum (FNV-1a), so two runs can tell whether the floppy gave the same */
static void disk_end(uint32_t pt, uint16_t words, bool write)
{
    uint32_t start = (pt - 2u * words) & 0xFFFFFF, sum = 2166136261u;
    for (uint32_t i = 0; i < words; i++) {
        uint16_t w = amiga_peek16(start + 2 * i);
        sum = (sum ^ (w >> 8)) * 16777619u;
        sum = (sum ^ (w & 0xFF)) * 16777619u;
    }
    printf("-- disk end %s %u words at $%06x, sum %08x (frame %lu), #%llu\n", write ? "write" : "read",
           words, (unsigned)start, (unsigned)sum, (unsigned long)s_frame, (unsigned long long)s_insns);
    /* AMI_DISKDUMP=<folder>: each read's words as they came (big-endian), read-<n>.bin */
    static int reads;
    const char *dir = getenv("AMI_DISKDUMP");
    if (dir && !write) {
        char path[512];
        snprintf(path, sizeof(path), "%s/read-%03d.bin", dir, reads++);
        FILE *f = fopen(path, "wb");
        for (uint32_t i = 0; f && i < words; i++) {
            uint16_t w = amiga_peek16(start + 2 * i);
            fputc(w >> 8, f);
            fputc(w & 0xFF, f);
        }
        if (f) {
            fclose(f);
        }
    }
}

/* The CPU's commands to the drives: CIA-B's port B (/MTR, /SEL3-0, /SIDE,
 * DIR, /STEP) as a write changes it */
static void drive_lines(uint8_t old, uint8_t prb)
{
    printf("-- drives %02x (frame %lu), #%llu\n", prb, (unsigned long)s_frame, (unsigned long long)s_insns);
    (void)old;
}

static void serial(int ch)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("AMI_SERIAL") != NULL;
    }
    if (on) {
        putchar(ch);
        fflush(stdout);
    }
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    amiga_screen_signature();       /* (shows the latest frame) */
    int width = amiga_screen_width(), height = amiga_screen_height();
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    static uint16_t line[2 * AMIGA_SCREEN_WIDTH];
    for (int y = 0; y < height; y++) {
        amiga_render_line_rgb565(y, line);
        for (int x = 0; x < width; x++) {
            uint16_t c = line[x];
            uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                               (uint8_t)((c & 31) * 255 / 31) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* AMI_WAV: 16-bit stereo at AMIGA_AUDIO_HZ */
static FILE *s_wav;
static uint32_t s_wav_frames;

static void wav_header(void)
{
    uint32_t bytes = s_wav_frames * 4, rate = AMIGA_AUDIO_HZ;
    uint8_t h[44] = { 'R', 'I', 'F', 'F' };
    uint32_t v;
    v = 36 + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t w = 1; memcpy(h + 20, &w, 2);
    w = 2; memcpy(h + 22, &w, 2);
    memcpy(h + 24, &rate, 4);
    v = rate * 4; memcpy(h + 28, &v, 4);
    w = 4; memcpy(h + 32, &w, 2);
    w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(s_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), s_wav);
    fseek(s_wav, 0, SEEK_END);
}

static void wav_drain(void)
{
    int16_t buf[2 * 1024];
    int n;
    while ((n = amiga_audio_render(buf, 1024)) > 0) {
        if (s_wav) {
            fwrite(buf, 4, (size_t)n, s_wav);
            s_wav_frames += (uint32_t)n;
        }
    }
}

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) {
        perror(path);
        exit(1);
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

/* The clock set by the Amiga (the board saves the offset in NVS) */
static void rtc_changed(int64_t offset)
{
    printf("clock offset now %lld s\n", (long long)offset);
}

static int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return e ? atoi(e) : def;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <kickstart.rom> [frames] [out.ppm]\n", argv[0]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 500;
    const char *out = argc > 3 ? argv[3] : "screen.ppm";

    /* the machine an RP9 names (the board's setup rows: ECS with more than
     * 512 KB chip RAM or Kickstart 2.04, the A600's and A1200's own)
     */
    amiga_rp9_machine_t rm;
    const char *pkg = NULL;
    for (int i = 0; i < 2 && !pkg; i++) {
        const char *p = getenv(i ? "AMI_DF0" : "AMI_HD0");
        if (p && strlen(p) > 4 && strcasecmp(p + strlen(p) - 4, ".rp9") == 0) {
            pkg = p;
        }
    }
    bool from_rp9 = !getenv("AMI_MODEL") && pkg && amiga_rp9_machine(pkg, &rm);

    amiga_config_t cfg = { 0 };
    cfg.model = from_rp9 ? rm.model : (amiga_model_t)env_int("AMI_MODEL", AMIGA_A500);
    cfg.ecs = from_rp9 ? rm.model >= AMIGA_A600 || rm.chip_kb > 512 || rm.kickstart == 204 : getenv("AMI_ECS") != NULL;
    cfg.ntsc = getenv("AMI_NTSC") != NULL;
    cfg.chip_size = (from_rp9 ? rm.chip_kb : env_int("AMI_CHIP_KB", 512)) * 1024;
    cfg.chip = malloc(cfg.chip_size);
    cfg.slow_size = (from_rp9 ? rm.slow_kb : env_int("AMI_SLOW_KB", 512)) * 1024;
    cfg.slow = cfg.slow_size ? malloc(cfg.slow_size) : NULL;
    cfg.fast_size = (from_rp9 ? rm.fast_kb : env_int("AMI_FAST_KB", 0)) * 1024;
    cfg.fast = cfg.fast_size ? malloc(cfg.fast_size) : NULL;
    struct stat st;
    char kick[600];
    snprintf(kick, sizeof(kick), "%s", argv[1]);
    if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) {
        int k = from_rp9 ? rm.kickstart : 130;
        const char *name = k == 120 ? "120" : k == 130 ? "130" : k == 204 ? "204" : k == 205 ? "205-a600"
                           : k == 300                                           ? "300-a1200"
                           : cfg.model == AMIGA_A1200                           ? "310-a1200"
                                                                                : "310-a600";
        snprintf(kick, sizeof(kick), "%s/amiga-os-%s.rom", argv[1], name);
    }
    if (from_rp9) {
        printf("%s: %s, Kickstart %d.%d, %d KB chip, %d KB slow, %d KB fast RAM (%s)\n", rm.title, rm.system,
               rm.kickstart / 100, rm.kickstart % 100 / 10, rm.chip_kb, rm.slow_kb, rm.fast_kb, kick);
    }
    cfg.kickstart = load(kick, &cfg.kickstart_size);
    cfg.rom = malloc(cfg.kickstart_size);
    if (getenv("AMI_EXT")) {
        uint32_t ext_size = 0;
        cfg.extension = load(getenv("AMI_EXT"), &ext_size);
        cfg.ext_rom = malloc(512 * 1024);
        if (ext_size != 512 * 1024) {
            fprintf(stderr, "AMI_EXT: not 512 KB\n");
            return 1;
        }
    }
    cfg.serial = serial;
    cfg.rtc_changed = rtc_changed;
    amiga_exception_hook = exception;
    if (getenv("AMI_DISKLOG")) {
        amiga_disk_hook = disk_dma;
        amiga_disk_end_hook = disk_end;
        amiga_drive_hook = drive_lines;
    }
#ifdef AMI_JIT
    {
        extern bool amiga_jit;
        if (getenv("AMI_JITNOHOOK")) {
            extern bool jit_hooks;
            jit_hooks = false;
        }
        if (getenv("AMI_NOJIT")) {
            amiga_jit = false;          /* (the 68EC020 by the interpreter alone) */
        }
    }
#endif
    if (getenv("AMI_DISKSEED")) {
        amiga_floppy_seed((uint32_t)strtoul(getenv("AMI_DISKSEED"), NULL, 10));
    }
    if (getenv("AMI_PCCOVER")) {
        s_cover_first = calloc(1u << 23, sizeof(uint32_t));
        s_cover_frame = calloc(1u << 23, sizeof(uint32_t));
        atexit(cover_write);
    }
    if (!amiga_init(&cfg)) {
        return 1;
    }

    /* AMI_DIS=<addr hex>:<n>: disassemble n instructions (the ROM is mapped
     * after reset) and stop */
    if (getenv("AMI_DIS")) {
        unsigned pc = 0, count = 20;
        sscanf(getenv("AMI_DIS"), "%x:%u", &pc, &count);
        for (unsigned i = 0; i < count; i++) {
            char text[100];
            unsigned n = m68k_disassemble(text, pc, cfg.model == AMIGA_A1200 ? M68K_CPU_TYPE_68EC020 : M68K_CPU_TYPE_68000);
            printf("  $%06x  %s\n", pc, text);
            pc += n;
        }
        return 0;
    }

    for (int d = 0; d < AMIGA_FLOPPIES; d++) {
        char name[8];
        snprintf(name, sizeof(name), "AMI_DF%d", d);
        if (getenv(name) && !amiga_floppy_insert(d, getenv(name))) {
            fprintf(stderr, "%s: can't use %s\n", name, getenv(name));
            return 1;
        }
    }
    if (getenv("AMI_NEWHD")) {
        char path[400];
        unsigned mb = 0;
        if (sscanf(getenv("AMI_NEWHD"), "%399[^:]:%u", path, &mb) != 2 ||
            !amiga_hd_create(path, mb * 1048576u / 516096u * 516096u)) {
            fprintf(stderr, "AMI_NEWHD: can't make %s\n", getenv("AMI_NEWHD"));
            return 1;
        }
    }
    for (int d = 0; d < AMIGA_HARD_DISKS; d++) {
        char name[8];
        snprintf(name, sizeof(name), "AMI_HD%d", d);
        if (getenv(name) && !amiga_hd_insert(d, getenv(name))) {
            fprintf(stderr, "%s: can't use %s\n", name, getenv(name));
            return 1;
        }
    }

    emudbg_host_run("AMI_DEBUG", amiga_run_frame, amiga_screen_hz());

    if (getenv("AMI_WAV")) {
        s_wav = fopen(getenv("AMI_WAV"), "wb");
        if (s_wav) {
            wav_header();
            amiga_audio_enable(true);
        }
    }

    int shots = env_int("AMI_SHOTS", 0), log_every = env_int("AMI_LOG", 0);
    int shots_from = env_int("AMI_SHOTFROM", 0);   /* (AMI_SHOTS only from that frame on) */
    clock_t t0 = clock();
    const char *inserts = getenv("AMI_INSERT");
    const char *inputs = getenv("AMI_INPUT");
    int move_x = 0, move_y = 0, buttons = 0;
    for (s_frame = 0; s_frame < (uint32_t)frames; s_frame++) {
        for (const char *p = inputs; p && *p;) {
            unsigned at = 0;
            char kind = 0;
            int a = 0, b = 0, c = 0;
            int n = sscanf(p, "%u:%c:%x:%x", &at, &kind, (unsigned *)&a, (unsigned *)&b);
            if (n >= 2 && at == s_frame) {
                if (kind == 'm' && sscanf(p, "%u:m:%d:%d:%d", &at, &a, &b, &c) == 4) {
                    move_x += a;
                    move_y += b;
                    buttons = c;
                } else if (kind == 'k' && n == 4) {
                    amiga_key((uint8_t)a, b != 0);
                } else if (kind == 'j' && n >= 3) {
                    amiga_joystick((uint8_t)a);
                } else if (kind == 't') {
                    char text[256];
                    size_t len = 0;
                    for (const char *t = strchr(strchr(p, ':') + 1, ':') + 1; *t && *t != ';' && len < sizeof(text) - 1;
                         t++) {
                        if (t[0] == '\\' && t[1] == 'n') {
                            text[len++] = '\n';
                            t++;
                        } else {
                            text[len++] = *t;
                        }
                    }
                    text[len] = '\0';
                    amiga_type(text);
                }
            }
            const char *end = strchr(p, ';');
            p = end ? end + 1 : NULL;
        }
        {
            int dx = move_x > 100 ? 100 : move_x < -100 ? -100 : move_x;
            int dy = move_y > 100 ? 100 : move_y < -100 ? -100 : move_y;
            move_x -= dx;
            move_y -= dy;
            amiga_mouse(dx, dy, (uint8_t)buttons);
        }
        for (const char *p = inserts; p && *p;) {
            unsigned at = 0;
            int drive = 0, used = 0;
            char path[400] = "";
            if (sscanf(p, "%u:%d:%n", &at, &drive, &used) < 2) {
                break;
            }
            const char *name = p + used, *end = strchr(name, ';');
            size_t n = end ? (size_t)(end - name) : strlen(name);
            if (at == s_frame) {
                snprintf(path, sizeof(path), "%.*s", (int)n, name);
                if (n == 0) {
                    amiga_floppy_eject(drive);
                } else if (!amiga_floppy_insert(drive, path)) {
                    fprintf(stderr, "can't insert %s\n", path);
                }
            }
            p = end ? end + 1 : NULL;
        }
#ifdef AMI_JIT
        /* AMI_JITRELEASE=<frame>[:<frame>...]: the translated code's arena
         * given back before those frames (as a disk's buffer would take it) */
        for (const char *p = getenv("AMI_JITRELEASE"); p && *p;) {
            if (strtoul(p, NULL, 10) == s_frame) {
                extern void jit_release(void);
                jit_release();
            }
            const char *end = strchr(p, ':');
            p = end ? end + 1 : NULL;
        }
#endif
        amiga_run_frame();
        wav_drain();
        if (log_every && (s_frame + 1) % log_every == 0) {
            amiga_status_t st;
            amiga_get_status(&st);
            printf("-- frame %lu: PC $%06lx SR %04x INTENA %04x INTREQ %04x DMACON %04x%s\n",
                   (unsigned long)st.frames, (unsigned long)st.pc, st.sr, st.intena, st.intreq,
                   st.dmacon, st.halted ? " HALTED" : "");
            if (getenv("AMI_LOGREGS")) {
                char regs[200];
                amiga_display_regs(regs, sizeof(regs));
                printf("   %s\n", regs);
            }
        }
        /* AMI_LOGFROM=<frame>: log every frame from there (Under Pressure cliff). */
        {
            static int log_from = -1;
            if (log_from < 0) {
                const char *e = getenv("AMI_LOGFROM");
                log_from = e ? atoi(e) : 0;
            }
            if (log_from && (int)s_frame >= log_from) {
                amiga_status_t st;
                amiga_get_status(&st);
                printf("-- frame %lu: PC $%06lx SR %04x INTENA %04x INTREQ %04x DMACON %04x%s\n",
                       (unsigned long)st.frames, (unsigned long)st.pc, st.sr, st.intena, st.intreq,
                       st.dmacon, st.halted ? " HALTED" : "");
                fflush(stdout);
            }
        }
        if (shots && (s_frame + 1) % shots == 0 && (int)(s_frame + 1) >= shots_from) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s-%04lu.ppm", (int)(strlen(out) - 4), out,
                     (unsigned long)(s_frame + 1));
            write_ppm(path);
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%d frames in %.2f s (%.0f%% of real time), %.1f M instructions\n", frames, secs,
           secs > 0 ? frames / (double)amiga_screen_hz() / secs * 100 : 0, s_insns / 1e6);
    amiga_hd_status_t hs;
    amiga_hd_get_status(&hs);
    for (int d = 0; d < AMIGA_HARD_DISKS; d++) {
        if (amiga_hd_path(d)) {
            printf("DH%d: %lu sectors read, %lu written\n", d, (unsigned long)hs.reads[d], (unsigned long)hs.writes[d]);
        }
    }
    for (int d = 0; d < AMIGA_FLOPPIES + AMIGA_HARD_DISKS; d++) {
        uint64_t free_bytes, size;
        if (amiga_disk_space(d, &free_bytes, &size)) {
            printf("%s%d: %llu bytes free of %llu\n", d < AMIGA_FLOPPIES ? "DF" : "DH",
                   d < AMIGA_FLOPPIES ? d : d - AMIGA_FLOPPIES, (unsigned long long)free_bytes,
                   (unsigned long long)size);
        }
    }
    amiga_mem_free_t mem[3];
    if (amiga_mem_free(&mem[0], &mem[1], &mem[2])) {
        static const char *const kinds[] = { "chip", "slow", "fast" };
        for (int k = 0; k < 3; k++) {
            if (mem[k].found) {
                printf("%s RAM: %lu bytes free, largest block %lu\n", kinds[k], (unsigned long)mem[k].free,
                       (unsigned long)mem[k].largest);
            }
        }
    }
    if (getenv("AMI_RING")) {
        show_ring(atoi(getenv("AMI_RING")));
    }
    if (getenv("AMI_OPSTAT")) {
        opstat_write();
    }
    if (getenv("AMI_JITSTAT")) {
        extern struct { unsigned long translations, flushes, full, drops, blocks_run, code_bytes, insns, distinct, exits, fbs, rv_words; } jit_stats;
        extern size_t jit_arena_size(size_t *routines);
        size_t routines, arena = jit_arena_size(&routines);
        printf("jit: %lu translations (%lu instructions, %lu bytes of code), %lu flushes (%lu full), %lu drops, %lu blocks run; "
               "the arena %zu KB (the shared routines %zu bytes)\n",
               jit_stats.translations, jit_stats.insns, jit_stats.code_bytes, jit_stats.flushes, jit_stats.full,
               jit_stats.drops, jit_stats.blocks_run, arena / 1024, routines);
        printf("jit: %lu distinct block addresses; per instruction %.1f RISC-V instructions, %.2f exits, %.2f handlers\n",
               jit_stats.distinct, (double)jit_stats.rv_words / jit_stats.insns, (double)jit_stats.exits / jit_stats.insns,
               (double)jit_stats.fbs / jit_stats.insns);
        {
            /* the handlers the translated code still calls, most first */
            extern uint32_t jit_fb_count[65536];
            static uint16_t ops[65536];
            int n = 0;
            for (int op = 0; op < 65536; op++) {
                if (jit_fb_count[op]) {
                    ops[n++] = (uint16_t)op;
                }
            }
            for (int i = 0; i < n; i++) {
                for (int j = i + 1; j < n; j++) {
                    if (jit_fb_count[ops[j]] > jit_fb_count[ops[i]]) {
                        uint16_t t = ops[i];
                        ops[i] = ops[j];
                        ops[j] = t;
                    }
                }
                if (i >= 30) {
                    break;
                }
            }
            for (int i = 0; i < n && i < 30; i++) {
                printf("jit handler %04x %10u\n", ops[i], jit_fb_count[ops[i]]);
            }
        }
        extern double jit_t_decode, jit_t_emit;
        printf("jit: %.3f s decoding, %.3f s emitting\n", jit_t_decode, jit_t_emit);
    }
    write_ppm(out);
    amiga_floppy_flush();
    if (getenv("AMI_DUMP")) {
        unsigned addr = 0, len = 0;
        char file[400] = "";
        if (sscanf(getenv("AMI_DUMP"), "%x:%u:%399s", &addr, &len, file) == 3) {
            FILE *f = fopen(file, "wb");
            for (unsigned i = 0; f && i < len; i++) {
                fputc(amiga_peek8(addr + i), f);
            }
            if (f) {
                fclose(f);
            }
        }
    }
    if (s_wav) {
        wav_header();
        fclose(s_wav);
    }
    return 0;
}
