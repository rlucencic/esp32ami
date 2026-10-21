/*
 * Amiga 500, 2000 and 600 (OCS/ECS, a 68000 at 7.09 MHz), platform
 * independent: the board side (../../main) and the host build (host/) drive
 * it. See ../../docs/architecture.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AMIGA_A500,
    AMIGA_A2000,
    AMIGA_A600,
} amiga_model_t;

#define AMIGA_OPCODE_TABLES_SIZE    0x50000

/* The picture: PAL's overscan window in hires pixels (lores ones doubled),
 * its lines doubled (an interlaced frame's two fields line by line)
 */
#define AMIGA_SCREEN_WIDTH          720
#define AMIGA_SCREEN_LINES          284         /* beam lines shown (PAL) */
#define AMIGA_SCREEN_HEIGHT         (AMIGA_SCREEN_LINES * 2)

typedef struct {
    amiga_model_t model;
    bool ecs;                   /* ECS Agnus and Denise (the A600, an A500+) */
    bool ntsc;
    uint8_t *chip;              /* chip RAM, chip_size bytes */
    uint32_t chip_size;         /* 512 KB, 1 MB or 2 MB */
    uint8_t *slow;              /* the A500's trapdoor RAM at $C00000, or NULL */
    uint32_t slow_size;         /* 0 or 512 KB */
    uint8_t *fast;              /* fast RAM at $200000 (autoconfig), or NULL */
    uint32_t fast_size;
    const uint8_t *kickstart;   /* as dumped (big-endian): 256 or 512 KB */
    uint32_t kickstart_size;
    uint8_t *rom;               /* kickstart_size bytes: the working copy */
    /* AMIGA_OPCODE_TABLES_SIZE bytes for copies of the CPU core's opcode
     * tables, or NULL (on the ESP32 read-only data is in flash, slow to
     * read: these in PSRAM)
     */
    void *opcode_tables;
    /* amiga_picture_size() bytes for three frames of the lines' records
     * (what the presenter draws from), or NULL to allocate them
     */
    void *picture;
    void (*serial)(int ch);     /* what's sent on the serial port, or NULL */
    /* The battery-backed clock: seconds ahead of the host's local time, and
     * what to call when the Amiga sets it (to keep the new offset), or NULL
     */
    int64_t rtc_offset;
    void (*rtc_changed)(int64_t offset);
} amiga_config_t;

typedef struct {
    uint32_t frames;            /* video frames emulated */
    uint32_t pc;
    uint16_t sr;
    uint16_t intena, intreq, dmacon;
    bool halted;                /* stopped by a double bus fault until a reset */
    uint32_t insns;             /* instructions in the last frame (host builds) */
} amiga_status_t;

/* The memory the picture's records take (amiga_config_t.picture) */
size_t amiga_picture_size(void);

/* Set up the machine and cold-reset it; false if the Kickstart is unusable */
bool amiga_init(const amiga_config_t *config);

/* Cold reset (power on): memory cleared */
void amiga_reset(void);

/* One video frame (313 lines PAL, 263 NTSC) */
void amiga_run_frame(void);

int amiga_screen_hz(void);

/* The picture (the presenter's task): a signature that changes with it, its
 * lines (AMIGA_SCREEN_WIDTH pixels)
 */
uint32_t amiga_screen_signature(void);
void amiga_render_line_rgb565(int y, uint16_t *dst);

void amiga_get_status(amiga_status_t *status);

/* Memory as the CPU sees it, without side effects (RAM and ROM; else 0) */
uint16_t amiga_peek16(uint32_t address);
uint8_t amiga_peek8(uint32_t address);
void amiga_poke8(uint32_t address, uint8_t value);

/* ---- Floppy disks: ADF images in DF0: and DF1: ---- */

#define AMIGA_FLOPPIES      2

bool amiga_floppy_insert(int drive, const char *path);  /* read only if the file is */
void amiga_floppy_eject(int drive);
const char *amiga_floppy_path(int drive);
bool amiga_floppy_write_protected(int drive);
bool amiga_floppy_create(const char *path);     /* a blank (unformatted) disk */
bool amiga_floppy_dirty(void);                  /* written tracks not in the files yet */
void amiga_floppy_flush(void);

typedef struct {
    int light;                  /* the drive whose motor is on, or -1 */
    uint32_t reads[AMIGA_FLOPPIES], writes[AMIGA_FLOPPIES];     /* tracks */
    uint8_t cylinder[AMIGA_FLOPPIES];
} amiga_floppy_status_t;

void amiga_floppy_status(amiga_floppy_status_t *status);

/* Debugging: called as the 68000 takes each exception (vector number) */
extern void (*amiga_exception_hook)(unsigned int vector);

/* The Kickstart's version (e.g. 34.5) from its header; false if the image
 * isn't one (size, checksum)
 */
bool amiga_kickstart_version(const uint8_t *image, uint32_t size, int *major, int *minor);
