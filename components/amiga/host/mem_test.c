/* The chip bus as seen through floating custom registers. No ROM needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_internal.h"
#include "m68k.h"

static uint8_t chip[512 * 1024], slow[512 * 1024], rom[256 * 1024];
static int failures;

#define CHECK(cond, message) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", message); } \
} while (0)

void m68k_host_instruction(unsigned int pc) { (void)pc; }

static int s_watch_hits;
static uint32_t s_watch_addr, s_watch_value;
static int s_watch_bytes;

void ami_host_watch_hit(uint32_t addr, int bytes, uint32_t value)
{
    s_watch_hits++;
    s_watch_addr = addr;
    s_watch_bytes = bytes;
    s_watch_value = value;
}

static unsigned floating_word(void) { return m68k_read_memory_16(0xDFF0FA); }

/* The 68000's bus cycles: an instruction's accesses from `start` clocks
 * into the frame on a line from `line` with even slots taken (three in a
 * row, then single ones), through the callbacks' inline paths or (`slow`:
 * a blit started, none under way) bus_cycle()'s. Each access's remaining
 * cycles go into out[], then which t bus_waited_over() takes.
 */
extern int m68ki_initial_cycles, m68ki_remaining_cycles;

static int accesses(bool slow, int line, int start, int out[])
{
    static const int taken[] = { 8, 12, 16, 40, 100, 452 };
    int k = 0;
    ami.cpu_running = true;
    ami.cycle = 0;
    ami.line_start = line;
    bus_line_start();
    for (unsigned i = 0; i < sizeof(taken) / sizeof(taken[0]); i++) {
        bus_take(line + taken[i]);
    }
    if (slow) {
        bus_blit_started();
    }
    m68ki_remaining_cycles = 123456;
    m68k_read_memory_16(0xF80000);                  /* (an instruction before: not this one) */
    m68ki_initial_cycles = 1000 + start;
    m68ki_remaining_cycles = 1000;                  /* (the instruction's first access at `start`) */
    m68k_read_memory_16(0x2000);                    /* (works the slots' table out) */
    out[k++] = m68ki_remaining_cycles;
    m68k_read_memory_32(0x2004);
    out[k++] = m68ki_remaining_cycles;
    m68k_write_memory_16(0xC00100, 0x1234);
    out[k++] = m68ki_remaining_cycles;
    m68k_read_memory_8(0xF80001);                   /* (off the chip bus: counted, no wait) */
    out[k++] = m68ki_remaining_cycles;
    m68k_write_memory_32(0x2008, 0x12345678);
    out[k++] = m68ki_remaining_cycles;
    m68k_read_memory_8(0xC00201);
    out[k++] = m68ki_remaining_cycles;
    m68ki_remaining_cycles -= 12;                   /* (the next instruction) */
    m68k_write_memory_8(0x2001, 0x55);
    out[k++] = m68ki_remaining_cycles;
    m68k_read_memory_32(0xC00300);
    out[k++] = m68ki_remaining_cycles;
    ami.cycle = 100000;
    ami.cpu_running = false;
    for (int t = 0; t < 600; t += 2) {
        out[k++] = bus_waited_over(line + t);
    }
    return k;
}

/* The slot model: an access at colour clock h waits for the taken even
 * slots in a row from slot (h + 1) / 2 on, 4 clocks each (a line's slots
 * taken in any order, again and again); through the inline paths or
 * bus_cycle()'s. The accesses that wait otherwise.
 */
#define MODEL_SLOTS 116

static int slot_model_mismatches(bool slow)
{
    unsigned seed = 12345;
    int bad = 0;
    for (int round = 0; round < 300; round++) {
        bool taken[MODEL_SLOTS] = { false };
        ami.cpu_running = true;
        ami.cycle = 0;
        ami.line_start = 0;
        bus_line_start();
        seed = seed * 1103515245 + 12345;
        int count = (seed >> 16) % 80;
        for (int i = 0; i < count; i++) {
            seed = seed * 1103515245 + 12345;
            int e = (seed >> 16) % (MODEL_SLOTS + 4);   /* (a few past the line: ignored) */
            if (e < MODEL_SLOTS) {
                taken[e] = true;
            }
            bus_take(4 * e);
        }
        if (slow) {
            bus_blit_started();
        }
        for (int h = 0; h < 2 * MODEL_SLOTS; h++) {
            m68ki_remaining_cycles = 123456;
            m68k_read_memory_16(0xF80000);          /* (an instruction before) */
            m68ki_initial_cycles = 1000 + 2 * h;
            m68ki_remaining_cycles = 1000;
            m68k_read_memory_16(0x2000);
            int wait = 0;
            for (int e = (h + 1) / 2; e < MODEL_SLOTS && taken[e]; e++) {
                wait += 4;
            }
            bad += 1000 - m68ki_remaining_cycles != wait;
        }
    }
    ami.cpu_running = false;
    return bad;
}

static void check_scan_doubling(int first, uint16_t pixels[4])
{
    display_reset();
    display_write(0x08E, (first << 8) | 0x81, 0);
    display_write(0x090, 0x40C1, 0);
    display_write(0x092, 0x0038, 0);
    display_write(0x094, 0x00D0, 0);
    display_write(0x0E2, 0x2000, 0);
    display_write(0x0E6, 0x3000, 0);
    display_write(0x100, 0x2000, 0);
    display_write(0x108, (uint16_t)-40, 0);
    display_write(0x10A, 0, 0);
    display_write(0x1FC, 0x4000, 0);
    ami.line = first - 1;
    ami.line_start = ami.line * 454;
    display_line_start();
    display_write(0x182, 0x0F00, ami.line_start);
    display_write(0x184, 0x00F0, ami.line_start);
    display_line_end();
    for (ami.line = first; ami.line < first + 4; ami.line++) {
        ami.line_start = ami.line * 454;
        display_line_start();
        display_fetch();
        display_line_end();
    }
    display_frame_end();
    amiga_screen_signature();
    for (int i = 0; i < 4; i++) {
        uint16_t line[AMIGA_SCREEN_WIDTH];
        amiga_render_line_rgb565(2 * (first - 29 + i), line);
        pixels[i] = line[AMIGA_SCREEN_WIDTH / 2];
    }
}

int main(void)
{
    memset(&ami, 0, sizeof(ami));
    ami.chip = chip;
    ami.chip_size = sizeof(chip);
    ami.chip_mask = sizeof(chip) - 1;
    ami.slow = slow;
    ami.slow_size = sizeof(slow);
    ami.rom = rom;
    ami.rom_size = sizeof(rom);
    mem_update_map();

    m68k_write_memory_16(0x1000, 0x6602);
    CHECK(floating_word() == 0x6602, "chip RAM word write drives the bus");
    m68k_write_memory_8(0x1001, 0x55);
    CHECK(floating_word() == 0x5555, "68000 byte write drives both lanes");
    m68k_read_memory_8(0x1000);
    CHECK(floating_word() == 0x6655, "chip RAM byte read drives its whole word");
    m68k_write_memory_32(0x1000, 0x12345678);
    CHECK(floating_word() == 0x5678, "long write leaves its second word on the bus");
    m68k_write_memory_16(0xC01000, 0xABCD);
    m68k_read_memory_16(0x1000);
    CHECK(floating_word() == 0x1234, "chip RAM word read replaces the bus word");
    m68k_read_memory_16(0xC01000);
    CHECK(floating_word() == 0xABCD, "slow RAM reads share the chip bus");
    m68k_read_memory_32(0x1000);
    CHECK(floating_word() == 0x5678, "long read leaves its second word on the bus");
    m68k_read_memory_16(0xF80000);
    CHECK(floating_word() == 0x5678, "ROM reads on the CPU bus leave the chip bus alone");

    /* Steel Empire probes at 1 KB intervals, landing on write-only
     * mirrors. Its following opcode fetch must break the apparent RAM. */
    m68k_write_memory_8(0xC801D8, 0x55);
    CHECK(m68k_read_memory_8(0xC801D8) == 0x55, "floating byte after a custom write");
    m68k_read_memory_16(0xC01000);   /* stand in for the next opcode fetch */
    CHECK(m68k_read_memory_8(0xC801D8) != 0x55, "custom mirror must not pass a RAM probe");

    /* Without individual DMA bus values, OCS's missing ID uses the
     * undriven-bus fallback, independent of the last opcode's bit 2. */
    m68k_write_memory_16(0x1000, 0x51C9);
    m68k_read_memory_16(0x1000);
    CHECK(custom_read(0x07C) == 0xFFFF, "OCS must not identify AGA from an opcode word");
    ami.ecs = true;
    CHECK(custom_read(0x07C) == 0xFFFC, "ECS Denise ID");
    ami.aga = true;
    CHECK(custom_read(0x07C) == 0x00F8, "AGA Lisa ID");

    /* The inline paths wait as bus_cycle() does: every start on a line,
     * before it and past its end; then one case against the slot model */
    ami.ecs = ami.aga = false;
    int quick[400], slow_way[400], n = 0, differ = 0;
    for (int line = 0; line <= 454; line += 454) {
        for (int start = line - 24; start < line + 480; start += 2) {
            n = accesses(false, line, start, quick);
            accesses(true, line, start, slow_way);
            differ += memcmp(quick, slow_way, n * sizeof(int)) != 0;
        }
    }
    CHECK(differ == 0, "inline bus cycles wait as bus_cycle() does");
    accesses(false, 0, 4, quick);
    /* from 4: the word at h 2 free; the long's words at h 4 (slots 2-4
     * taken: 12) and h 12; the next long's second word at h 20 (slot 10:
     * 4), then the next instruction's long the same */
    CHECK(quick[0] == 1000 && quick[1] == 988, "a long's first word waits past three taken slots");
    CHECK(quick[4] == 984 && quick[7] == 968, "a long's second word waits for its slot");
    int waited = 0;
    for (int t = 0; t < 300; t++) {
        waited += quick[8 + t] << (t == 21 || t == 22 ? 0 : 8);
    }
    CHECK(waited == 2, "the last access waited from 40 to 44");
    CHECK(slot_model_mismatches(false) == 0, "inline waits follow the slot model");
    CHECK(slot_model_mismatches(true) == 0, "bus_cycle()'s waits follow the slot model");

    /* AMI_WATCH: a CPU write covering the address reports once */
    setenv("AMI_WATCH", "1234", 1);
    mem_watch_reload();
    s_watch_hits = 0;
    ami.frames = 0;
    m68k_write_memory_16(0x1234, 0xABCD);
    CHECK(s_watch_hits == 1 && s_watch_addr == 0x1234 && s_watch_bytes == 2 &&
          s_watch_value == 0xABCD, "AMI_WATCH stops on a covering word write");
    s_watch_hits = 0;
    m68k_write_memory_16(0x2000, 0);
    CHECK(s_watch_hits == 0, "AMI_WATCH ignores other addresses");
    unsetenv("AMI_WATCH");
    mem_watch_reload();

    ami.model = AMIGA_A1200;
    ami.cpu_shift = 1;
    ami.slow_size = 0;
    ami.intreq = 0;
    mem_update_map();
    m68k_write_memory_8(0xCFF09C, 0xFF);
    m68k_write_memory_16(0xD0009C, 0x8020);
    CHECK(m68k_read_memory_16(0xDFF01E) == 0, "A1200 leaves the C0-D7 ranges off the custom bus");
    m68k_write_memory_16(0xDF009C, 0x8020);
    CHECK(m68k_read_memory_16(0xDFF01E) == 0x0020, "A1200 mirrors custom registers throughout DFxxxx");

    m68k_write_memory_32(0x1000, 0x12345678);
    m68k_write_memory_16(0x1004, 0x9ABC);
    CHECK(m68k_read_memory_16(0x1001) == 0x3456, "68EC020 reads an odd word in big-endian order");
    CHECK(m68k_read_memory_32(0x1001) == 0x3456789A, "68EC020 reads an odd long in big-endian order");
    m68k_write_memory_16(0x1001, 0xABCD);
    CHECK(m68k_read_memory_32(0x1000) == 0x12ABCD78, "68EC020 odd word write preserves adjacent bytes");

    m68k_write_memory_16(0x00FFFE, 0xAB12);
    m68k_write_memory_32(0x010000, 0x345678CD);
    CHECK(m68k_read_memory_16(0x00FFFF) == 0x1234, "68EC020 reads an odd word across banks");
    CHECK(m68k_read_memory_32(0x00FFFF) == 0x12345678, "68EC020 reads an odd long across banks");
    m68k_write_memory_32(0x00FFFF, 0xDEADBEEF);
    CHECK(m68k_read_memory_16(0x00FFFE) == 0xABDE &&
          m68k_read_memory_32(0x010000) == 0xADBEEFCD, "68EC020 odd long write preserves adjacent bytes across banks");
    rom[sizeof(rom) - 2] = 0xA5;
    m68k_write_memory_16(0x000000, 0x1234);
    CHECK(m68k_read_memory_16(0xFFFFFF) == 0xA512, "68EC020 odd word read wraps at the 24-bit address boundary");

    ami.aga = ami.ecs = true;
    memset(chip + 0x2000, 0xFF, 40);
    memset(chip + 0x2028, 0, 40);
    memset(chip + 0x3000, 0, 40);
    memset(chip + 0x3028, 0xFF, 40);
    const uint16_t doubled[] = { 0xF800, 0xF800, 0x07E0, 0x07E0 };
    uint16_t pixels[4];
    display_init(NULL);
    for (int first = 29; first <= 30; first++) {
        check_scan_doubling(first, pixels);
        CHECK(memcmp(pixels, doubled, sizeof(pixels)) == 0, "AGA scan doubling repeats both planes for either window parity");
    }

    m68k_write_memory_16(0x000004, 0x1234);
    for (int model = AMIGA_A600; model <= AMIGA_A1200; model++) {
        ami.model = model;
        ami.ovl = true;
        cia_reset();
        mem_update_map();
        m68k_write_memory_8(0xBFD100, 0xFF);
        CHECK(amiga_peek16(0x000004) == amiga_peek16(0xF80004), "CIA-B leaves the Gayle reset overlay active");
        m68k_write_memory_8(0xBFE101, 0xFF);
        CHECK(amiga_peek16(0x000004) == 0x1234, "The first CIA-A write clears the Gayle reset overlay");
        m68k_write_memory_8(0xBFE201, 0x01);
        m68k_write_memory_8(0xBFE001, 0x01);
        CHECK(amiga_peek16(0x000004) == 0x1234, "Gayle cannot re-enable the overlay through CIA-A port A");
    }
    ami.model = AMIGA_A500;
    ami.cpu_shift = 0;
    m68k_write_memory_8(0xBFE001, 0x01);
    CHECK(amiga_peek16(0x000004) == amiga_peek16(0xF80004), "CIA-A port A enables the A500 overlay");
    m68k_write_memory_8(0xBFE001, 0x00);
    CHECK(amiga_peek16(0x000004) == 0x1234, "CIA-A port A clears the A500 overlay");

    copper_reset();
    ami.dmacon = ami.intreq = 0;
    m68k_write_memory_32(0x1000, 0x009C8020); /* Old list sets the vertical blank request. */
    m68k_write_memory_32(0x1004, 0xFFFFFFFE);
    m68k_write_memory_32(0x2000, 0x009C8040); /* New list sets the blitter request. */
    m68k_write_memory_32(0x2004, 0xFFFFFFFE);
    copper_write(0x082, 0x1000, 0, false);
    copper_vblank();
    copper_write(0x082, 0x2000, 8, false);
    custom_write(0x096, 0x8280, 16, false);
    copper_run(28, 28);
    CHECK(ami.intreq == 0x0040, "Copper reloads the current COP1LC when DMA starts after vertical blank");

    puts(failures ? "chip bus checks failed" : "all chip bus checks passed");
    return failures != 0;
}
