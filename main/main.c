/*
 * Amiga 500, 2000 and 600 emulator on the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Amiga into it: memory, Kickstart, settings, floppy drives, the clock,
 * status, console commands. (No keyboard, mouse or sound yet.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/console.h"
#include "emu/emu.h"
#include "amiga.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static amiga_config_t s_cfg;
static uint8_t *s_kick_image;
static const char *s_kick_name;

/* ---- The battery-backed clock: its offset kept in NVS, from core 0 ---- */

static volatile int64_t s_rtc_offset;
static volatile bool s_rtc_dirty;

static void rtc_changed(int64_t offset)
{
    s_rtc_offset = offset;
    s_rtc_dirty = true;
}

/* ---- Memory: the most of each kind, first thing at power-on ---- */

#define CHIP_MAX        (2 * 1024 * 1024)
#define SLOW_MAX        (512 * 1024)

static void reserve_memory(void)
{
    settings_load();
    s_rtc_offset = settings_load_rtc();
    s_cfg.chip = heap_caps_aligned_alloc(16, CHIP_MAX, MALLOC_CAP_SPIRAM);
    s_cfg.slow = heap_caps_aligned_alloc(16, SLOW_MAX, MALLOC_CAP_SPIRAM);
    s_cfg.rom = heap_caps_aligned_alloc(16, ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    s_kick_image = heap_caps_malloc(ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.opcode_tables = heap_caps_malloc(AMIGA_OPCODE_TABLES_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.picture = heap_caps_calloc(1, amiga_picture_size(), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "memory %s", s_cfg.chip && s_cfg.slow && s_cfg.rom && s_kick_image && s_cfg.opcode_tables &&
             s_cfg.picture ? "ok" : "MISSING");
}

/* ---- Starting ---- */

static bool start(void)
{
    if (!s_cfg.chip || !s_cfg.slow || !s_cfg.rom || !s_kick_image || !s_cfg.picture) {
        emu_set_message("Not enough memory for the Amiga.");
        return false;
    }
    uint32_t size;
    if (!rom_store_load(settings.kick, s_kick_image, &size, &s_kick_name)) {
        static char text[200];
        static const char *const names[KICK_COUNT] = {
            "Kickstart 1.3 (34.5)", "Kickstart 2.04 (37.175)", "Kickstart 2.05 (37.350)", "Kickstart 3.1 (40.63)",
        };
        snprintf(text, sizeof(text),
                 "No %s: put it in roms/ and flash with idf.py flash, or copy it into the \"roms\" folder "
                 "on the SD card.", names[settings.kick]);
        emu_set_message(text);
        return false;
    }
    s_cfg.model = settings.model;
    s_cfg.chip_size = settings.chip_kb * 1024u;
    s_cfg.slow_size = settings.slow_kb * 1024u;
    s_cfg.ecs = settings.model == AMIGA_A600 || settings.chip_kb > 512 || settings.kick == KICK_204;
    s_cfg.ntsc = settings.ntsc;
    s_cfg.kickstart = s_kick_image;
    s_cfg.kickstart_size = size;
    s_cfg.rtc_offset = s_rtc_offset;
    s_cfg.rtc_changed = rtc_changed;
    if (!amiga_init(&s_cfg)) {
        emu_set_message("The Amiga could not be set up: see the log.");
        return false;
    }
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    mode->width = AMIGA_SCREEN_WIDTH;
    mode->height = AMIGA_SCREEN_HEIGHT;
    mode->hz = amiga_screen_hz();
}

static bool halted(void)
{
    amiga_status_t st;
    amiga_get_status(&st);
    return st.halted;
}

/* ---- Setup screen choices ---- */

enum { CHOICE_MODEL, CHOICE_KICK, CHOICE_CHIP, CHOICE_SLOW, CHOICE_VIDEO, CHOICES };

static const char *const model_names[] = { "A500", "A2000", "A600", "" };
static const char *const kick_names_a500[] = { "1.3", "2.04", "3.1", "" };
static const uint8_t kicks_a500[] = { KICK_13, KICK_204, KICK_31 };
static const char *const kick_names_a600[] = { "2.05", "3.1", "" };
static const uint8_t kicks_a600[] = { KICK_205, KICK_31 };
static const char *const chip_names[] = { "512 KB", "1 MB", "" };
static const char *const chip_names_a600[] = { "512 KB", "1 MB", "2 MB", "" };
static const uint16_t chip_sizes[] = { 512, 1024, 2048 };
static const char *const slow_names[] = { "None", "512 KB", "" };
static const char *const slow_names_a600[] = { "None", "" };
static const char *const video_names[] = { "PAL\n50 Hz", "NTSC\n60 Hz", "" };

static bool a600(void)
{
    return settings.model == AMIGA_A600;
}

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= AMIGA_A500 && i <= AMIGA_A600 && i != settings.model) {
        settings_model_defaults(i);
        settings_save();
    }
}

static const uint8_t *kicks(int *n)
{
    *n = a600() ? 2 : 3;
    return a600() ? kicks_a600 : kicks_a500;
}

static int kick_get(void)
{
    int n;
    const uint8_t *k = kicks(&n);
    for (int i = 0; i < n; i++) {
        if (k[i] == settings.kick) {
            return i;
        }
    }
    return 0;
}

static void kick_set(int i)
{
    int n;
    const uint8_t *k = kicks(&n);
    if (i >= 0 && i < n) {
        settings.kick = k[i];
        settings_save();
    }
}

static int chip_get(void)
{
    for (int i = 0; i < 3; i++) {
        if (chip_sizes[i] == settings.chip_kb) {
            return i;
        }
    }
    return 0;
}

static void chip_set(int i)
{
    if (i >= 0 && i < 3 && settings_chip_ok(settings.model, chip_sizes[i])) {
        settings.chip_kb = chip_sizes[i];
        settings_save();
    }
}

static int slow_get(void)
{
    return settings.slow_kb ? 1 : 0;
}

static void slow_set(int i)
{
    if (i == 0 || (i == 1 && settings_slow_ok(settings.model, 512))) {
        settings.slow_kb = i ? 512 : 0;
        settings_save();
    }
}

static int video_get(void)
{
    return settings.ntsc;
}

static void video_set(int i)
{
    if (i == 0 || i == 1) {
        settings.ntsc = (uint8_t)i;
        settings_save();
    }
}

static const emu_choice_t choices[CHOICES] = {
    { "Model", model_names, model_get, model_set, false },
    { "Kickstart", kick_names_a500, kick_get, kick_set, false },
    { "Chip RAM", chip_names, chip_get, chip_set, false },
    { "Slow RAM", slow_names, slow_get, slow_set, false },
    { "Video", video_names, video_get, video_set, false },
};

/* The Kickstart and memory rows follow the model */
static const char *const *choice_names(int i)
{
    switch (i) {
    case CHOICE_KICK:
        return a600() ? kick_names_a600 : kick_names_a500;
    case CHOICE_CHIP:
        return a600() ? chip_names_a600 : chip_names;
    case CHOICE_SLOW:
        return a600() ? slow_names_a600 : slow_names;
    default:
        return NULL;
    }
}

/* ---- Floppy drives: DF0:, DF1: (ADF), written tracks to the card ---- */

static const char *const extensions[] = { ".adf", NULL };
static const char *const drive_names[AMIGA_FLOPPIES] = { "DF0:", "DF1:" };
static const disks_size_t sizes[] = { { 901120, "880 KB" } };

static bool disk_create(const char *path, uint32_t bytes)
{
    (void)bytes;
    return amiga_floppy_create(path);
}

static const disks_config_t disks = {
    .folder = "floppies",
    .extensions = extensions,
    .description = "Disk images (.ADF)",
    .title = "Floppy disks",
    .button = "Floppy",
    .nvs_namespace = "amifloppy",
    .drives = AMIGA_FLOPPIES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".adf",
    .sizes = sizes,
    .size_count = 1,
    .max_upload = 4 * 1024 * 1024,
    .insert = amiga_floppy_insert,
    .eject = amiga_floppy_eject,
    .path = amiga_floppy_path,
    .write_protected = amiga_floppy_write_protected,
    .create = disk_create,
};

static void flush(void *arg)
{
    (void)arg;
    amiga_floppy_flush();
}

/* The clock's new offset to NVS; written tracks to the card once the disk
 * has been still for 2 s
 */
static void every_second(void)
{
    if (s_rtc_dirty) {
        s_rtc_dirty = false;
        settings_save_rtc(s_rtc_offset);
    }
    static int dirty_for;
    if (amiga_floppy_dirty()) {
        if (++dirty_for >= 2) {
            emu_paused(flush, NULL);
            dirty_for = 0;
        }
    } else {
        dirty_for = 0;
    }
}

/* ---- Status ---- */

static const char *const models[] = { "Amiga 500", "Amiga 2000", "Amiga 600" };

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_DRIVES) {
        amiga_floppy_status_t fs;
        amiga_floppy_status(&fs);
        for (int d = 0; d < AMIGA_FLOPPIES; d++) {
            const char *name = disks_name(d);
            snprintf(text, sizeof(text), "cylinder %u", (unsigned)fs.cylinder[d]);
            emu_json_drive(j, drive_names[d], name, amiga_floppy_write_protected(d), fs.light == d, fs.reads[d],
                           fs.writes[d], name ? text : NULL);
        }
        return;
    }
    if (section != EMU_STATUS_ROWS) {
        return;
    }
    if (s_cfg.slow_size) {
        snprintf(text, sizeof(text), "%s, %u KB chip RAM + %u KB slow RAM", models[s_cfg.model],
                 (unsigned)(s_cfg.chip_size >> 10), (unsigned)(s_cfg.slow_size >> 10));
    } else {
        snprintf(text, sizeof(text), "%s, %u KB chip RAM", models[s_cfg.model], (unsigned)(s_cfg.chip_size >> 10));
    }
    emu_json_row(j, "System", text);
    emu_json_row(j, "Kickstart", s_kick_name ? s_kick_name : "-");
    snprintf(text, sizeof(text), "%s, %s", s_cfg.ecs ? "ECS" : "OCS", s_cfg.ntsc ? "NTSC (60 Hz)" : "PAL (50 Hz)");
    emu_json_row(j, "Chipset", text);
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    amiga_status_t st;
    amiga_get_status(&st);
    snprintf(buf, len, "frames %lu PC $%06lX SR %04X INTENA %04X INTREQ %04X DMACON %04X%s",
             (unsigned long)st.frames, (unsigned long)st.pc, st.sr, st.intena, st.intreq, st.dmacon,
             st.halted ? " halted" : "");
}

/*   R <model> <kick> [chip KB] [slow KB]   restart as model 0 A500, 1 A2000,
 *                      2 A600 with Kickstart 0 1.3, 1 2.04, 2 2.05, 3 3.1
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'R': {
        int model = 0, kick = 0, chip = 0, slow = -1;
        int n = sscanf(line + 1, "%d %d %d %d", &model, &kick, &chip, &slow);
        if (n >= 2 && model >= AMIGA_A500 && model <= AMIGA_A600 && settings_kick_ok(model, kick)) {
            settings_model_defaults(model);
            settings.kick = (uint8_t)kick;
            if (n >= 3 && settings_chip_ok(model, chip)) {
                settings.chip_kb = (uint16_t)chip;
            }
            if (n >= 4 && settings_slow_ok(model, slow)) {
                settings.slow_kb = (uint16_t)slow;
            }
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Amiga",
    .short_name = "Amiga",
    .view_width = 800,          /* 4:3: 960 x 720 on the Tab5 */
    .scale = EMU_SCALE_SMOOTH,
    .reserve_memory = reserve_memory,
    .start = start,
    .reset = amiga_reset,
    .run_frame = amiga_run_frame,
    .halted = halted,
    .screen_mode = screen_mode,
    .screen_signature = amiga_screen_signature,
    .render_line = amiga_render_line_rgb565,
    .choices = choices,
    .choice_count = CHOICES,
    .choice_names = choice_names,
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
