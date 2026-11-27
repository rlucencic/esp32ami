/*
 * Amiga 500, 2000 and 600 (OCS/ECS, a 68000 at 7.09 MHz) and 1200 (a
 * 68EC020 at 14.19 MHz), platform
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
    AMIGA_A1200,                /* the 68EC020, 2 MB chip RAM, Gayle */
} amiga_model_t;

/* The CPU core's handler and cycle tables, copied (see amiga_config_t) */
#define AMIGA_OPCODE_TABLES_SIZE    (0x10000 * sizeof(void *) + 0x10000)

/* The picture: the overscan window in hires pixels (lores ones doubled),
 * its lines doubled (an interlaced frame's two fields line by line): PAL
 * lines 29-312, NTSC 21-262 (amiga_screen_height()). A frame with ECS
 * super-hires lines is twice as wide (amiga_screen_width()).
 */
#define AMIGA_SCREEN_WIDTH          720
#define AMIGA_SCREEN_LINES          284         /* beam lines shown (PAL, the most) */
#define AMIGA_SCREEN_LINES_NTSC     242
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
    /* AROS's extension ROM for $E00000-$E7FFFF (as dumped, 512 KB; NULL:
     * none) and 512 KB for its working copy
     */
    const uint8_t *extension;
    uint8_t *ext_rom;
    /* AMIGA_OPCODE_TABLES_SIZE bytes for copies of the CPU core's opcode
     * tables (the cycle counts are adjusted to the Amiga's bus), or NULL to
     * allocate them
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
    int line, cycle;            /* the beam's line, the 68000's clocks into it */
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
int amiga_screen_height(void);      /* AMIGA_SCREEN_HEIGHT, NTSC's less */

/* The picture (the presenter's task): a signature that changes with it
 * (and picks the frame), its width (AMIGA_SCREEN_WIDTH or, super-hires,
 * twice that), its lines
 */
uint32_t amiga_screen_signature(void);
int amiga_screen_width(void);
void amiga_render_line_rgb565(int y, uint16_t *dst);

void amiga_get_status(amiga_status_t *status);
/* The display's registers as text (logs) */
void amiga_display_regs(char *buf, size_t len);

/* ---- The web page's machine card: the state now, plain copies (read
 * from another task while the machine runs: now and then a moment's mix;
 * nothing here changes the machine) ---- */

typedef struct {
    uint8_t agnus_id;               /* VPOSR's bits 14-8: $00 OCS, $20 ECS, $23 Alice (NTSC: + $10) */
    uint16_t denise_id;             /* DENISEID: $FFFF (OCS's has none), $FFFC ECS, $00F8 Lisa */
    bool gayle;                     /* Gayle there (the A600's, A1200's, or for a hard disk) */
    uint8_t gayle_id;               /* $D0, the A1200's $D1 */
    bool ntsc_beam;                 /* the beam's standard now (ECS: BEAMCON0's PAL bit) */
    bool ovl;                       /* the ROM over chip RAM from $000000 (reads) */
    uint32_t fast_base;             /* where autoconfig put the fast RAM (0: not yet) */
    uint32_t hd_base;               /* our hard disk board (0: not configured) */
    bool ext_rom;                   /* AROS's extension ROM at $E00000 */
    uint16_t dmacon, intena, intreq;
    int ipl;                        /* the interrupt level Paula asks for (0: none) */
    uint32_t cop1lc;                /* the copper's list */
    uint32_t cacr;                  /* the 68EC020's cache control */
    bool led;                       /* the power LED bright (CIA-A PRA bit 1 low): the sound filter on */
} amiga_chips_t;
void amiga_get_chips(amiga_chips_t *chips);

/* The screen last drawn (its latest frame's line records): the mode the
 * most lines in a row have (lines with bitplanes, the same BPLCON0 mode
 * bits and window), the colours as its middle line began
 */
typedef struct {
    int planes;                     /* that mode's bitplanes (0: no line has any) */
    uint16_t bplcon0, bplcon2, fmode;
    int width;                      /* the window's width, in the mode's pixels */
    int lines;                      /* the lines in that mode (a field's: interlace doubles them) */
    int modes;                      /* modes on the lines with bitplanes (up to 8) */
    uint8_t sprites;                /* bit n: sprite n shown on some line */
    int ncolors;                    /* colour registers: 32, AGA 256 */
    uint32_t color[32];             /* the first 32 as 24-bit RGB */
} amiga_screen_t;
void amiga_get_screen(amiga_screen_t *screen);

/* Paula's channels now */
typedef struct {
    struct {
        bool dma;                   /* playing from memory */
        bool playing;               /* a word playing (DMA, or written to AUDxDAT) */
        uint8_t attached;           /* ADKCON: 1 the next one's volume, 2 its period */
        uint16_t period;            /* colour clocks a byte */
        uint8_t volume;             /* 0-64 */
    } ch[4];
    uint32_t clock;                 /* colour clocks a second (PAL 3546895, NTSC 3579545) */
} amiga_sound_t;
void amiga_get_sound(amiga_sound_t *sound);

/* Memory as the CPU sees it, without side effects (RAM and ROM; else 0) */
uint16_t amiga_peek16(uint32_t address);
/* Free memory by Kickstart's lists, by kind (false until it has made
 * them; read while the 68000 runs, so now and then a moment's figures)
 */
typedef struct {
    bool found;                     /* the kind is in the lists */
    uint32_t free, largest;         /* bytes; largest 0: not known */
} amiga_mem_free_t;
bool amiga_mem_free(amiga_mem_free_t *chip, amiga_mem_free_t *slow, amiga_mem_free_t *fast);
/* The free space on a drive's disk (0-1 DF0:-DF1:, 2-3 DH0:-DH1:) as its
 * AmigaDOS volumes count it, in bytes: false if not known (no disk, not
 * AmigaDOS, its bitmap being validated); updated once a second, a hard
 * disk once its writes stop
 */
bool amiga_disk_space(int drive, uint64_t *free_bytes, uint64_t *size);
uint8_t amiga_peek8(uint32_t address);
void amiga_poke8(uint32_t address, uint8_t value);

/* ---- Input (from any task) ---- */

/* A key by its Amiga raw code ($00-$67: $45 Esc, $44 Return, $40 Space,
 * $60/$61 Shift, $63 Ctrl, $64/$65 Alt, $66/$67 the Amiga keys, $62 Caps
 * Lock: down while it's on). Ctrl and both Amiga keys reset the machine.
 */
void amiga_key(uint8_t code, bool pressed);
/* Type text with the US keymap's keys, one a few frames ('\n' Return,
 * '\t' Tab, '\b' Backspace; others it hasn't are skipped); replaces what's
 * still being typed
 */
void amiga_type(const char *text);
bool amiga_typing(void);
/* The mouse in port 1: counts moved (down positive), buttons bit 0 left,
 * 1 right, 2 middle
 */
void amiga_mouse(int dx, int dy, uint8_t buttons);

#define AMIGA_JOY_UP        0x01
#define AMIGA_JOY_DOWN      0x02
#define AMIGA_JOY_LEFT      0x04
#define AMIGA_JOY_RIGHT     0x08
#define AMIGA_JOY_FIRE      0x10
#define AMIGA_JOY_FIRE2     0x20

/* The joystick in port 2 */
void amiga_joystick(uint8_t bits);

/* ---- Sound: 16-bit stereo for the speaker's task ---- */

#define AMIGA_AUDIO_HZ      44100

void amiga_audio_enable(bool on);
int amiga_audio_render(int16_t *dst, int frames);   /* frames given */
int amiga_audio_pending(void);                      /* frames waiting */

/* ---- Floppy disks: ADF images in DF0: and DF1: ---- */

#define AMIGA_FLOPPIES      2

/* An ADF (read only if the file is; an extended one always), or an RP9
 * package's first disk (unpacked once into a hidden ADF beside it; the
 * same package again: its next disk). The disk turns 8x as fast when it
 * can take it: an AmigaDOS disk with a filesystem, an RP9 marked
 * "turbo-floppy"; others (own loaders, copy protection) at the real speed.
 */
bool amiga_floppy_insert(int drive, const char *path);
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
    uint8_t disk[AMIGA_FLOPPIES], disks[AMIGA_FLOPPIES];    /* an RP9's disk n of disks (0: none) */
} amiga_floppy_status_t;

void amiga_floppy_status(amiga_floppy_status_t *status);

/* ---- Hard disks: HDF images as IDE drives on Gayle (the A600's, and the
 * other models' while a hard disk is in), read and written in place;
 * Kickstart 2.05 and 3.1 mount a disk with an RDB
 */
#define AMIGA_HARD_DISKS    2

bool amiga_hd_insert(int n, const char *path);
void amiga_hd_eject(int n);
const char *amiga_hd_path(int n);           /* NULL: none */
bool amiga_hd_write_protected(int n);
const char *amiga_hd_detail(int n);         /* how an RP9's disk was put together (NULL: a plain file) */

typedef struct {
    int light;                      /* the drive last used (-1: none) */
    uint32_t reads[AMIGA_HARD_DISKS], writes[AMIGA_HARD_DISKS];    /* sectors */
} amiga_hd_status_t;

void amiga_hd_get_status(amiga_hd_status_t *status);

/* A blank hard disk image of `bytes`: an RDB with one partition (DH0,
 * bootable, the old filesystem's type: every Kickstart has it; 2.0 and
 * later format it FFS too), to be formatted */
bool amiga_hd_create(const char *path, uint32_t bytes);

/* An RP9 package with a hard disk image (else floppies) */
bool amiga_rp9_hard_disk(const char *path);

/* The floppy disks in an RP9 package (0: none, or not a package). One with
 * more than one can be in DF0: and DF1: at once, each drive with a disk of
 * its own (amiga_floppy_insert()) */
int amiga_rp9_floppies(const char *path);

/* The machine an RP9 package's manifest names: its system (a-500, a-500plus,
 * a-2000, a-600, a-1200, a-4000 as an A1200), Kickstart and RAM (the
 * system's own unless it says). False if it isn't a package or names none.
 */
typedef struct {
    char title[64];
    char system[16];                /* as the manifest says ("a-500plus") */
    amiga_model_t model;
    int kickstart;                  /* 120, 130, 204, 205, 300, 310 */
    int chip_kb, slow_kb, fast_kb;
} amiga_rp9_machine_t;
bool amiga_rp9_machine(const char *path, amiga_rp9_machine_t *m);

/* Debugging: called as the 68000 takes each exception (vector number), as
 * a disk DMA starts (DSKLEN's value, DSKSYNC, ADKCON, the drive's
 * cylinder and side, DSKPT), as it ends (DSKPT past its last word, the
 * words, a write or not), and as a write to CIA-B's port B changes the
 * drives' lines (the old and new values: motor, selects, side, step)
 */
extern void (*amiga_exception_hook)(unsigned int vector);
extern void (*amiga_disk_hook)(uint16_t dsklen, uint16_t sync, uint16_t adkcon, int cylinder, int side,
                               uint32_t pt);
extern void (*amiga_disk_end_hook)(uint32_t pt, uint16_t words, bool write);
extern void (*amiga_drive_hook)(uint8_t old, uint8_t prb);
/* Debugging: where the head of the drive Paula reads is, in 68000 clocks
 * from its turn's start (224 a word), -1 for none */
int amiga_floppy_head(void);
/* Testing: another sequence of the drives' speeds from turn to turn (0: the
 * usual one), as another drive would have */
void amiga_floppy_seed(uint32_t seed);

/* The Kickstart's version (e.g. 34.5) from its header; false if the image
 * isn't one (size, checksum)
 */
bool amiga_kickstart_version(const uint8_t *image, uint32_t size, int *major, int *minor);
