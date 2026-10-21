/*
 * Run the Amiga on the host: boot a Kickstart for a number of frames and
 * write the last picture as a PPM image.
 *
 *   ./ami_host <kickstart.rom> [frames] [out.ppm]
 *
 * Environment: AMI_CHIP_KB (512), AMI_SLOW_KB (512; 0 for none), AMI_ECS,
 * AMI_NTSC, AMI_SERIAL (print what's sent on the serial port),
 * AMI_SHOTS=<n> (a picture every n frames, <out>-<frame>.ppm), AMI_LOG=<n>
 * (the machine's state every n frames), AMI_RING=<n> (the last n
 * instructions at the end), AMI_STOP=<pc>[:<frame>], AMI_TRACE=<pc>:<n>,
 * AMI_EXC (exceptions other than interrupts; "all" with them),
 * AMI_DEBUG=<port> (serve the debugger, in real time), AMI_DF0, AMI_DF1
 * (ADF images in the drives: written to!), AMI_INSERT=<frame>:<drive>:<adf>
 * [;...] (later; an empty name ejects).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "amiga.h"
#include "m68k.h"
#include "emudbg.h"

static uint32_t s_frame;
static uint64_t s_insns;

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
        exit(0);
    }

    static uint32_t trace_pc = 1;
    static int trace_n, trace_left;
    if (trace_pc == 1) {
        const char *e = getenv("AMI_TRACE");
        trace_pc = e ? strtoul(e, NULL, 16) : 0;
        trace_n = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 100;
    }
    if (trace_pc && pc == trace_pc && trace_left == 0) {
        trace_left = trace_n;
    }
    if (trace_left > 0) {
        trace_left--;
        disassemble(pc, 1);
    }
}

static void exception(unsigned int vector)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("AMI_EXC");
        mode = e ? (strcmp(e, "all") == 0 ? 2 : 1) : 0;
    }
    if (mode == 0 || (mode == 1 && vector >= 24 && vector < 32)) {
        return;
    }
    printf("-- exception %u at PC $%06x (frame %lu), SR %04x\n", vector,
           m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF, (unsigned long)s_frame,
           m68k_get_reg(NULL, M68K_REG_SR));
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
    fprintf(f, "P6\n%d %d\n255\n", AMIGA_SCREEN_WIDTH, AMIGA_SCREEN_HEIGHT);
    static uint16_t line[AMIGA_SCREEN_WIDTH];
    for (int y = 0; y < AMIGA_SCREEN_HEIGHT; y++) {
        amiga_render_line_rgb565(y, line);
        for (int x = 0; x < AMIGA_SCREEN_WIDTH; x++) {
            uint16_t c = line[x];
            uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                               (uint8_t)((c & 31) * 255 / 31) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
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

    amiga_config_t cfg = { 0 };
    cfg.model = AMIGA_A500;
    cfg.ecs = getenv("AMI_ECS") != NULL;
    cfg.ntsc = getenv("AMI_NTSC") != NULL;
    cfg.chip_size = env_int("AMI_CHIP_KB", 512) * 1024;
    cfg.chip = malloc(cfg.chip_size);
    cfg.slow_size = env_int("AMI_SLOW_KB", 512) * 1024;
    cfg.slow = cfg.slow_size ? malloc(cfg.slow_size) : NULL;
    cfg.kickstart = load(argv[1], &cfg.kickstart_size);
    cfg.rom = malloc(cfg.kickstart_size);
    cfg.serial = serial;
    amiga_exception_hook = exception;
    if (!amiga_init(&cfg)) {
        return 1;
    }

    for (int d = 0; d < AMIGA_FLOPPIES; d++) {
        char name[8];
        snprintf(name, sizeof(name), "AMI_DF%d", d);
        if (getenv(name) && !amiga_floppy_insert(d, getenv(name))) {
            fprintf(stderr, "%s: can't use %s\n", name, getenv(name));
            return 1;
        }
    }

    emudbg_host_run("AMI_DEBUG", amiga_run_frame, amiga_screen_hz());

    int shots = env_int("AMI_SHOTS", 0), log_every = env_int("AMI_LOG", 0);
    clock_t t0 = clock();
    const char *inserts = getenv("AMI_INSERT");
    for (s_frame = 0; s_frame < (uint32_t)frames; s_frame++) {
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
        amiga_run_frame();
        if (log_every && (s_frame + 1) % log_every == 0) {
            amiga_status_t st;
            amiga_get_status(&st);
            printf("-- frame %lu: PC $%06lx SR %04x INTENA %04x INTREQ %04x DMACON %04x%s\n",
                   (unsigned long)st.frames, (unsigned long)st.pc, st.sr, st.intena, st.intreq,
                   st.dmacon, st.halted ? " HALTED" : "");
        }
        if (shots && (s_frame + 1) % shots == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s-%04lu.ppm", (int)(strlen(out) - 4), out,
                     (unsigned long)(s_frame + 1));
            write_ppm(path);
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%d frames in %.2f s (%.0f%% of real time), %.1f M instructions\n", frames, secs,
           secs > 0 ? frames / (double)amiga_screen_hz() / secs * 100 : 0, s_insns / 1e6);
    if (getenv("AMI_RING")) {
        show_ring(atoi(getenv("AMI_RING")));
    }
    write_ppm(out);
    amiga_floppy_flush();
    return 0;
}
