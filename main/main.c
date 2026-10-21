/*
 * Amiga 500, 2000 and 600 emulator on the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Amiga into it: memory, Kickstart, settings, drives (floppies, a hard
 * disk), the clock, keyboard, mouse, joystick and touch, sound, status,
 * console commands.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "emu/board.h"
#include "emu/console.h"
#include "emu/emu.h"
#include "amiga.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static amiga_config_t s_cfg;
static uint8_t *s_kick_image, *s_ext_image;
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
#define FAST_MAX        (8 * 1024 * 1024)

static void reserve_memory(void)
{
    settings_load();
    s_rtc_offset = settings_load_rtc();
    s_cfg.chip = heap_caps_aligned_alloc(16, CHIP_MAX, MALLOC_CAP_SPIRAM);
    s_cfg.slow = heap_caps_aligned_alloc(16, SLOW_MAX, MALLOC_CAP_SPIRAM);
    s_cfg.fast = heap_caps_aligned_alloc(16, FAST_MAX, MALLOC_CAP_SPIRAM);
    s_cfg.rom = heap_caps_aligned_alloc(16, ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    s_kick_image = heap_caps_malloc(ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.ext_rom = heap_caps_aligned_alloc(16, ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    s_ext_image = heap_caps_malloc(ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.opcode_tables = heap_caps_malloc(AMIGA_OPCODE_TABLES_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.picture = heap_caps_calloc(1, amiga_picture_size(), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "memory %s", s_cfg.chip && s_cfg.slow && s_cfg.fast && s_cfg.rom && s_kick_image &&
             s_cfg.opcode_tables && s_cfg.picture ? "ok" : "MISSING");
}

/* ---- Starting ---- */

static bool start(void)
{
    if (!s_cfg.chip || !s_cfg.slow || !s_cfg.fast || !s_cfg.rom || !s_kick_image || !s_cfg.picture) {
        emu_set_message("Not enough memory for the Amiga.");
        return false;
    }
    uint32_t size;
    bool ext = rom_store_has_extension(settings.kick);
    if (!rom_store_load(settings.kick, s_kick_image, &size, &s_kick_name) ||
        (ext && (!s_ext_image || !s_cfg.ext_rom || !rom_store_load_extension(settings.kick, s_ext_image)))) {
        static char text[200];
        static const char *const names[KICK_COUNT] = {
            "Kickstart 1.3 (34.5)", "Kickstart 2.04 (37.175)", "Kickstart 2.05 (37.350)", "Kickstart 3.1 (40.63)",
            "Kickstart 1.2 (33.180)", "AROS ROM and its extension ROM",
        };
        snprintf(text, sizeof(text),
                 "No %s: put it in roms/ and flash with idf.py flash, or copy it into the \"roms\" folder "
                 "on the SD card.", names[settings.kick]);
        emu_set_message(text);
        return false;
    }
    s_cfg.extension = ext ? s_ext_image : NULL;
    s_cfg.model = settings.model;
    s_cfg.chip_size = settings.chip_kb * 1024u;
    s_cfg.slow_size = settings.slow_kb * 1024u;
    s_cfg.fast_size = settings.fast_kb * 1024u;
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

/* Super-hires (ECS) frames: their pixels one to one on the panel, the
 * standard display window's 1280 (from lores pixel $81 on: the overscan at
 * the sides cut), all the lines, centred (the overscan above and below in
 * view)
 */
#define SHRES_FIRST     (4 * (0x81 - 108))

static void screen_mode(emu_screen_t *mode)
{
    mode->width = amiga_screen_width();
    mode->height = amiga_screen_height();
    mode->hz = amiga_screen_hz();
    if (mode->width > AMIGA_SCREEN_WIDTH) {
        mode->src_w = mode->width - SHRES_FIRST < BOARD_LCD_H_RES ? mode->width - SHRES_FIRST : BOARD_LCD_H_RES;
        mode->src_x = SHRES_FIRST;
        mode->view_w = mode->src_w;
        mode->view_h = mode->height;
    }
}

static bool halted(void)
{
    amiga_status_t st;
    amiga_get_status(&st);
    return st.halted;
}

/* ---- Keyboard, mouse, joystick, touch ----
 *
 * Keys by position (a US/UK keyboard onto the Amiga's): F11 is Help, the
 * Windows/Command keys are the Amiga keys, Caps Lock toggles as the
 * Amiga's does. F12 makes the cursor keys and the numeric keypad (8 4 6 2,
 * 0 fire) the joystick in port 2, Alt its fire button.
 */
#define K(code)     ((code) | 0x80)
static const uint8_t usage_to_amiga[0x68] = {
    [0x04] = K(0x20), [0x05] = K(0x35), [0x06] = K(0x33), [0x07] = K(0x22),     /* a b c d */
    [0x08] = K(0x12), [0x09] = K(0x23), [0x0A] = K(0x24), [0x0B] = K(0x25),     /* e f g h */
    [0x0C] = K(0x17), [0x0D] = K(0x26), [0x0E] = K(0x27), [0x0F] = K(0x28),     /* i j k l */
    [0x10] = K(0x37), [0x11] = K(0x36), [0x12] = K(0x18), [0x13] = K(0x19),     /* m n o p */
    [0x14] = K(0x10), [0x15] = K(0x13), [0x16] = K(0x21), [0x17] = K(0x14),     /* q r s t */
    [0x18] = K(0x16), [0x19] = K(0x34), [0x1A] = K(0x11), [0x1B] = K(0x32),     /* u v w x */
    [0x1C] = K(0x15), [0x1D] = K(0x31),                                         /* y z */
    [0x1E] = K(0x01), [0x1F] = K(0x02), [0x20] = K(0x03), [0x21] = K(0x04),     /* 1 2 3 4 */
    [0x22] = K(0x05), [0x23] = K(0x06), [0x24] = K(0x07), [0x25] = K(0x08),     /* 5 6 7 8 */
    [0x26] = K(0x09), [0x27] = K(0x0A),                                         /* 9 0 */
    [0x28] = K(0x44),       /* Return */
    [0x29] = K(0x45),       /* Esc */
    [0x2A] = K(0x41),       /* Backspace */
    [0x2B] = K(0x42),       /* Tab */
    [0x2C] = K(0x40),       /* Space */
    [0x2D] = K(0x0B),       /* - */
    [0x2E] = K(0x0C),       /* = */
    [0x2F] = K(0x1A),       /* [ */
    [0x30] = K(0x1B),       /* ] */
    [0x31] = K(0x0D),       /* \ */
    [0x32] = K(0x2B),       /* non-US # (next to Return) */
    [0x33] = K(0x29),       /* ; */
    [0x34] = K(0x2A),       /* ' */
    [0x35] = K(0x00),       /* ` */
    [0x36] = K(0x38),       /* , */
    [0x37] = K(0x39),       /* . */
    [0x38] = K(0x3A),       /* / */
    [0x3A] = K(0x50), [0x3B] = K(0x51), [0x3C] = K(0x52), [0x3D] = K(0x53),     /* F1-F4 */
    [0x3E] = K(0x54), [0x3F] = K(0x55), [0x40] = K(0x56), [0x41] = K(0x57),     /* F5-F8 */
    [0x42] = K(0x58), [0x43] = K(0x59),                                         /* F9 F10 */
    [0x44] = K(0x5F),       /* F11 -> Help */
    [0x4C] = K(0x46),       /* Delete -> Del */
    [0x4F] = K(0x4E),       /* Right */
    [0x50] = K(0x4F),       /* Left */
    [0x51] = K(0x4D),       /* Down */
    [0x52] = K(0x4C),       /* Up */
    [0x53] = K(0x5A),       /* Num Lock -> keypad ( */
    [0x54] = K(0x5C),       /* keypad / */
    [0x55] = K(0x5D),       /* keypad * */
    [0x56] = K(0x4A),       /* keypad - */
    [0x57] = K(0x5E),       /* keypad + */
    [0x58] = K(0x43),       /* keypad Enter */
    [0x59] = K(0x1D), [0x5A] = K(0x1E), [0x5B] = K(0x1F),                       /* keypad 1 2 3 */
    [0x5C] = K(0x2D), [0x5D] = K(0x2E), [0x5E] = K(0x2F),                       /* keypad 4 5 6 */
    [0x5F] = K(0x3D), [0x60] = K(0x3E), [0x61] = K(0x3F),                       /* keypad 7 8 9 */
    [0x62] = K(0x0F),       /* keypad 0 */
    [0x63] = K(0x3C),       /* keypad . */
    [0x64] = K(0x30),       /* non-US \ (the ISO key left of Z) */
    [0x67] = K(0x5B),       /* keypad = -> keypad ) */
};

/* Modifiers $E0-$E7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI (one Ctrl) */
static const uint8_t modifier_to_amiga[8] = { 0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67 };

static uint8_t s_modifiers;
static bool s_caps;
static bool s_joy_keys;             /* F12: cursor keys and keypad are the joystick */
static uint8_t s_joy_bits;

static bool joystick_key(uint8_t usage, bool pressed)
{
    uint8_t b = 0;
    switch (usage) {
    case 0x4F: case 0x5E: b = AMIGA_JOY_RIGHT; break;
    case 0x50: case 0x5C: b = AMIGA_JOY_LEFT; break;
    case 0x51: case 0x5A: b = AMIGA_JOY_DOWN; break;
    case 0x52: case 0x60: b = AMIGA_JOY_UP; break;
    case 0x5F: b = AMIGA_JOY_UP | AMIGA_JOY_LEFT; break;
    case 0x61: b = AMIGA_JOY_UP | AMIGA_JOY_RIGHT; break;
    case 0x59: b = AMIGA_JOY_DOWN | AMIGA_JOY_LEFT; break;
    case 0x5B: b = AMIGA_JOY_DOWN | AMIGA_JOY_RIGHT; break;
    case 0x62: case 0x5D: b = AMIGA_JOY_FIRE; break;
    default: return false;
    }
    s_joy_bits = pressed ? s_joy_bits | b : s_joy_bits & ~b;
    amiga_joystick(s_joy_bits);
    return true;
}

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        int bit = usage - 0xE0;
        if (s_joy_keys && (bit == 2 || bit == 6)) {
            /* Alt is fire while the keys are the joystick */
            s_joy_bits = pressed ? s_joy_bits | AMIGA_JOY_FIRE : s_joy_bits & ~AMIGA_JOY_FIRE;
            amiga_joystick(s_joy_bits);
            return;
        }
        uint8_t code = modifier_to_amiga[bit];
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++) {
            mask |= modifier_to_amiga[i] == code ? 1u << i : 0;
        }
        bool was = (s_modifiers & mask) != 0;
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        bool now = (s_modifiers & mask) != 0;
        if (was != now) {
            amiga_key(code, now);
        }
        return;
    }
    if (usage == 0x45) {            /* F12: keys <-> joystick */
        if (pressed) {
            s_joy_keys = !s_joy_keys;
            s_joy_bits = 0;
            amiga_joystick(0);
            ESP_LOGI(TAG, "cursor keys and keypad: %s", s_joy_keys ? "joystick" : "keys");
        }
        return;
    }
    if (usage == 0x39) {            /* Caps Lock: down while it's on */
        if (pressed) {
            s_caps = !s_caps;
            amiga_key(0x62, s_caps);
        }
        return;
    }
    if (s_joy_keys && joystick_key(usage, pressed)) {
        return;
    }
    if (usage < sizeof(usage_to_amiga) && usage_to_amiga[usage]) {
        amiga_key(usage_to_amiga[usage] & 0x7F, pressed);
    }
}

static uint8_t s_mouse_buttons;

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    s_mouse_buttons = buttons & 7;
    amiga_mouse(dx, dy, s_mouse_buttons);
}

/* The touch screen as a touchpad: a finger moving moves the pointer (in the
 * picture's pixels: Workbench's hires pixels, half lines), a tap clicks the
 * left button, a tap and then touching again within 300 ms drags with it,
 * holding still for half a second holds the right one (the menus: move to
 * an item, let go to choose it)
 */
#define TOUCH_SLOP      6
#define TOUCH_RIGHT_US  500000
#define TOUCH_AGAIN_US  300000

static void touch(int x, int y, bool pressed)
{
    static enum { IDLE, PENDING, MOVE, DRAG, RIGHT } state;
    static int lx, ly, x0, y0;
    static int64_t t0, t_tap;
    int64_t now = esp_timer_get_time();
    x = x * AMIGA_SCREEN_WIDTH / amiga_screen_width();     /* (super-hires: hires pixels too) */
    if (!pressed) {
        if (state == PENDING) {
            amiga_mouse(0, 0, 1);
            vTaskDelay(pdMS_TO_TICKS(60));
            amiga_mouse(0, 0, 0);
            t_tap = now;
        } else if (state == DRAG || state == RIGHT) {
            amiga_mouse(0, 0, 0);
        }
        state = IDLE;
        return;
    }
    switch (state) {
    case IDLE:
        x0 = lx = x;
        y0 = ly = y;
        t0 = now;
        state = PENDING;
        break;
    case PENDING:
        if (abs(x - x0) > TOUCH_SLOP || abs(y - y0) > TOUCH_SLOP) {
            state = now - t_tap < TOUCH_AGAIN_US ? DRAG : MOVE;
            amiga_mouse(x - lx, y - ly, state == DRAG ? 1 : 0);
            lx = x;
            ly = y;
        } else if (now - t0 >= TOUCH_RIGHT_US) {
            state = RIGHT;
            amiga_mouse(0, 0, 2);
        }
        break;
    default:
        amiga_mouse(x - lx, y - ly, state == DRAG ? 1 : state == RIGHT ? 2 : 0);
        lx = x;
        ly = y;
        break;
    }
}

/* ---- Setup screen choices ---- */

enum { CHOICE_MODEL, CHOICE_KICK, CHOICE_CHIP, CHOICE_SLOW, CHOICE_FAST, CHOICE_VIDEO, CHOICES };

static const char *const model_names[] = { "A500", "A2000", "A600", "" };
static const char *const kick_names_a500[] = { "1.2", "1.3", "2.04", "3.1", "AROS", "" };
static const uint8_t kicks_a500[] = { KICK_12, KICK_13, KICK_204, KICK_31, KICK_AROS };
static const char *const kick_names_a600[] = { "2.05", "3.1", "AROS", "" };
static const uint8_t kicks_a600[] = { KICK_205, KICK_31, KICK_AROS };
static const char *const chip_names[] = { "512 KB", "1 MB", "" };
static const char *const chip_names_a600[] = { "512 KB", "1 MB", "2 MB", "" };
static const uint16_t chip_sizes[] = { 512, 1024, 2048 };
/* The A500's trapdoor RAM (the A501 card) at $C00000: on the chip bus but
 * not for the chips, "slow RAM"; fast RAM (the 68000's own) is an
 * autoconfig board
 */
static const char *const slow_names[] = { "None", "512 KB trapdoor\n(at $C00000)", "" };
static const char *const slow_names_a600[] = { "None", "" };
static const char *const fast_names[] = { "None", "1 MB", "2 MB", "4 MB", "8 MB", "" };
static const uint16_t fast_sizes[] = { 0, 1024, 2048, 4096, 8192 };
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
    *n = a600() ? 2 : 4;
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

static int fast_get(void)
{
    for (int i = 0; i < 5; i++) {
        if (fast_sizes[i] == settings.fast_kb) {
            return i;
        }
    }
    return 0;
}

static void fast_set(int i)
{
    if (i >= 0 && i < 5) {
        settings.fast_kb = fast_sizes[i];
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
    { "Fast RAM", fast_names, fast_get, fast_set, false },
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

/* ---- Drives: DF0:, DF1: (ADF, written tracks to the card), DH0:, DH1:
 * (HDF: our hard disk board with 1.3 and 2.04; with 2.05 and 3.1 Gayle's
 * IDE takes DH0:, the board DH1:) ---- */

#define DRIVES          (AMIGA_FLOPPIES + AMIGA_HARD_DISKS)

static const char *const extensions[] = { ".adf", ".rp9", ".hdf", NULL };
static const char *const drive_names[DRIVES] = { "DF0:", "DF1:", "DH0:", "DH1:" };
/* (hard disks in whole cylinders of 16 x 63 blocks) */
#define HD_MB(n)        ((uint32_t)(n) * 1048576u / 516096u * 516096u)
static const disks_size_t sizes[] = {
    { 901120, "880 KB" },
    { HD_MB(20), "20 MB hard disk (formatted on the Amiga)" },
    { HD_MB(50), "50 MB hard disk (formatted on the Amiga)" },
    { HD_MB(100), "100 MB hard disk (formatted on the Amiga)" },
};

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

/* Hard disk images (and RP9s with one) for DH0:, the rest for the floppies */
static bool disk_accepts(int drive, const char *name)
{
    bool hd = drive >= AMIGA_FLOPPIES;
    if (has_ext(name, ".rp9")) {
        char path[200];
        snprintf(path, sizeof(path), "%s/%s", disks_folder(), name);
        return amiga_rp9_hard_disk(path) == hd;
    }
    return has_ext(name, ".hdf") == hd;
}

static bool disk_takes_blank(int drive)
{
    (void)drive;
    return true;
}

static uint32_t disk_default_size(int drive)
{
    return drive >= AMIGA_FLOPPIES ? HD_MB(50) : 901120;
}

static const char *disk_extension_for(uint32_t bytes)
{
    return bytes > 901120 ? ".hdf" : ".adf";
}

static bool disk_insert(int drive, const char *path)
{
    return drive >= AMIGA_FLOPPIES ? amiga_hd_insert(drive - AMIGA_FLOPPIES, path) : amiga_floppy_insert(drive, path);
}

static void disk_eject(int drive)
{
    if (drive >= AMIGA_FLOPPIES) {
        amiga_hd_eject(drive - AMIGA_FLOPPIES);
    } else {
        amiga_floppy_eject(drive);
    }
}

static const char *disk_path(int drive)
{
    return drive >= AMIGA_FLOPPIES ? amiga_hd_path(drive - AMIGA_FLOPPIES) : amiga_floppy_path(drive);
}

static bool disk_write_protected(int drive)
{
    return drive >= AMIGA_FLOPPIES ? amiga_hd_write_protected(drive - AMIGA_FLOPPIES)
                                   : amiga_floppy_write_protected(drive);
}

static bool disk_create(const char *path, uint32_t bytes)
{
    return bytes > 901120 ? amiga_hd_create(path, bytes) : amiga_floppy_create(path);
}

/* A package deleted or replaced: its unpacked disks go too
 * (".<name>.rp9.<n>.adf", ".<name>.rp9.hdf")
 */
static void disk_removed(const char *path)
{
    if (!has_ext(path, ".rp9")) {
        return;
    }
    const char *slash = strrchr(path, '/');
    int dir = slash ? (int)(slash - path + 1) : 0;
    const char *base = slash ? slash + 1 : path;
    char copy[256];
    for (int n = 1; n <= 8; n++) {
        snprintf(copy, sizeof(copy), "%.*s.%s.%d.adf", dir, path, base, n);
        remove(copy);
    }
    snprintf(copy, sizeof(copy), "%.*s.%s.hdf", dir, path, base);
    remove(copy);
}

static const disks_config_t disks = {
    .folder = "floppies",
    .extensions = extensions,
    .description = "Disk images (.ADF, .HDF) and Amiga Forever games (.RP9)",
    .title = "Disks",
    .button = "Disks",
    .nvs_namespace = "amifloppy",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".adf",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 256ull * 1024 * 1024,
    .insert = disk_insert,
    .eject = disk_eject,
    .path = disk_path,
    .write_protected = disk_write_protected,
    .create = disk_create,
    .removed = disk_removed,
    .accepts = disk_accepts,
    .takes_blank = disk_takes_blank,
    .default_size = disk_default_size,
    .extension_for = disk_extension_for,
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

/* Joystick keys on the web page's status */
static const char *input_text(void)
{
    return s_joy_keys ? "cursor keys and keypad are the joystick (F12 switches back), Alt fire"
                      : "keys (F12: cursor keys and keypad as the joystick)";
}


static const char *const models[] = { "Amiga 500", "Amiga 2000", "Amiga 600" };

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_DRIVES) {
        amiga_floppy_status_t fs;
        amiga_floppy_status(&fs);
        for (int d = 0; d < AMIGA_FLOPPIES; d++) {
            const char *name = disks_name(d);
            if (fs.disks[d]) {
                snprintf(text, sizeof(text), "disk %u of %u, cylinder %u (the same package again: its next disk)",
                         (unsigned)fs.disk[d], (unsigned)fs.disks[d], (unsigned)fs.cylinder[d]);
            } else {
                snprintf(text, sizeof(text), "cylinder %u", (unsigned)fs.cylinder[d]);
            }
            uint64_t free_bytes = 0, size = 0;
            amiga_disk_space(d, &free_bytes, &size);
            emu_json_drive_space(j, drive_names[d], name, amiga_floppy_write_protected(d), fs.light == d, fs.reads[d],
                                 fs.writes[d], name ? text : NULL, free_bytes, size);
        }
        amiga_hd_status_t hs;
        amiga_hd_get_status(&hs);
        bool ide = settings.kick == KICK_205 || settings.kick == KICK_31 || settings.kick == KICK_AROS;
        for (int h = 0; h < AMIGA_HARD_DISKS; h++) {
            const char *name = disks_name(AMIGA_FLOPPIES + h), *how = amiga_hd_detail(h);
            const char *by = settings.kick == KICK_12 ? "Kickstart 1.2 can't boot from a hard disk (1.3 can)"
                             : ide && h == 0 ? "IDE on Gayle" : "the esp32ami hard disk board";
            snprintf(text, sizeof(text), "%s%s%s", by, how ? ": " : "", how ? how : "");
            uint64_t free_bytes = 0, size = 0;
            amiga_disk_space(AMIGA_FLOPPIES + h, &free_bytes, &size);
            emu_json_drive_space(j, drive_names[AMIGA_FLOPPIES + h], name, amiga_hd_write_protected(h), hs.light == h,
                                 hs.reads[h], hs.writes[h], name ? text : NULL, free_bytes, size);
        }
        return;
    }
    if (section != EMU_STATUS_ROWS) {
        return;
    }
    int n = snprintf(text, sizeof(text), "%s, %u KB chip RAM", models[s_cfg.model],
                     (unsigned)(s_cfg.chip_size >> 10));
    if (s_cfg.slow_size) {
        n += snprintf(text + n, sizeof(text) - n, " + %u KB slow RAM", (unsigned)(s_cfg.slow_size >> 10));
    }
    if (s_cfg.fast_size) {
        snprintf(text + n, sizeof(text) - n, " + %u MB fast RAM", (unsigned)(s_cfg.fast_size >> 20));
    }
    emu_json_row(j, "System", text);
    amiga_mem_free_t mem[3];
    if (amiga_mem_free(&mem[0], &mem[1], &mem[2])) {
        static const char *const labels[] = { "Free chip RAM", "Free slow RAM", "Free fast RAM" };
        const uint32_t sizes[] = { s_cfg.chip_size, s_cfg.slow_size, s_cfg.fast_size };
        for (int k = 0; k < 3; k++) {
            if (!mem[k].found || sizes[k] == 0) {
                continue;
            }
            bool mb = sizes[k] >= 1024 * 1024;
            int len = snprintf(text, sizeof(text), "%lu KB of %lu %s", (unsigned long)(mem[k].free / 1024),
                               (unsigned long)(mb ? sizes[k] >> 20 : sizes[k] >> 10), mb ? "MB" : "KB");
            if (mem[k].largest && mem[k].largest / 1024 < mem[k].free / 1024) {
                snprintf(text + len, sizeof(text) - len, " <span class=\"dim\">(largest block %lu KB)</span>",
                         (unsigned long)(mem[k].largest / 1024));
            }
            int used = 100 - (int)((uint64_t)mem[k].free * 100 / sizes[k]);
            emu_json_row_bar(j, labels[k], text, used < 0 ? 0 : used);
        }
    }
    emu_json_row(j, "Kickstart", s_kick_name ? s_kick_name : "-");
    snprintf(text, sizeof(text), "%s, %s", s_cfg.ecs ? "ECS" : "OCS", s_cfg.ntsc ? "NTSC (60 Hz)" : "PAL (50 Hz)");
    emu_json_row(j, "Chipset", text);
    emu_json_row(j, "Cursor keys", input_text());
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

/*   k <hex>            tap Amiga key <hex> (e.g. 44 Return, 40 Space)
 *   K <hex> <0|1>      key down (1) or up (0)
 *   T <text>           type text (US keymap; "\n" for Return)
 *   m <dx> <dy> <btn>  move the mouse; buttons 1 left, 2 right, 4 middle
 *   j <hex>            the joystick: 1 up, 2 down, 4 left, 8 right, 10 fire
 *   R <model> <kick> [chip KB] [slow KB] [fast KB]   restart as model 0 A500,
 *                      1 A2000, 2 A600 with Kickstart 0 1.3, 1 2.04, 2 2.05,
 *                      3 3.1, 4 1.2
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned code = strtoul(line + 1, NULL, 16);
        amiga_key((uint8_t)code, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        amiga_key((uint8_t)code, false);
        return true;
    }
    case 'T': {
        char text[256];
        size_t n = 0;
        for (const char *p = line[1] == ' ' ? line + 2 : line + 1; *p && n < sizeof(text) - 1; p++) {
            if (p[0] == '\\' && p[1] == 'n') {
                text[n++] = '\n';
                p++;
            } else {
                text[n++] = *p;
            }
        }
        text[n] = '\0';
        amiga_type(text);
        return true;
    }
    case 'K': {
        unsigned code = 0, down = 0;
        if (sscanf(line + 1, "%x %u", &code, &down) == 2) {
            amiga_key((uint8_t)code, down != 0);
        }
        return true;
    }
    case 'm': {
        int dx = 0, dy = 0, buttons = 0;
        sscanf(line + 1, "%d %d %d", &dx, &dy, &buttons);
        while (dx || dy) {
            int sx = dx > 100 ? 100 : dx < -100 ? -100 : dx;
            int sy = dy > 100 ? 100 : dy < -100 ? -100 : dy;
            amiga_mouse(sx, sy, (uint8_t)buttons);
            dx -= sx;
            dy -= sy;
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        amiga_mouse(0, 0, (uint8_t)buttons);
        return true;
    }
    case 'j':
        amiga_joystick((uint8_t)strtoul(line + 1, NULL, 16));
        return true;
    case 'R': {
        int model = 0, kick = 0, chip = 0, slow = -1, fast = -1;
        int n = sscanf(line + 1, "%d %d %d %d %d", &model, &kick, &chip, &slow, &fast);
        if (n >= 2 && model >= AMIGA_A500 && model <= AMIGA_A600 && settings_kick_ok(model, kick)) {
            settings_model_defaults(model);
            settings.kick = (uint8_t)kick;
            if (n >= 3 && settings_chip_ok(model, chip)) {
                settings.chip_kb = (uint16_t)chip;
            }
            if (n >= 4 && settings_slow_ok(model, slow)) {
                settings.slow_kb = (uint16_t)slow;
            }
            if (n >= 5 && settings_fast_ok(model, fast)) {
                settings.fast_kb = (uint16_t)fast;
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

#if CONFIG_EMU_WEB
/* web/amifiles.js: DMS archives and gzipped images unpacked in the browser */
extern const char amifiles_js_start[] asm("_binary_amifiles_js_start");
extern const char amifiles_js_end[] asm("_binary_amifiles_js_end");
#endif

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
    .audio_hz = AMIGA_AUDIO_HZ,
    .audio_enable = amiga_audio_enable,
    .audio_render = amiga_audio_render,
    .audio_pending = amiga_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .touch = touch,
    .choices = choices,
    .choice_count = CHOICES,
    .choice_names = choice_names,
    .disks = &disks,
#if CONFIG_EMU_WEB
    .web_script = amifiles_js_start,
    .web_script_end = amifiles_js_end,
#endif
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
