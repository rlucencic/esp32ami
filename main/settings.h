#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "amiga.h"

/* The Kickstarts (rom_store.c finds them) */
typedef enum {
    KICK_13,                /* 1.3 (34.5): A500, A2000 */
    KICK_204,               /* 2.04 (37.175): A500+ */
    KICK_205,               /* 2.05 (37.350): A600 */
    KICK_31,                /* 3.1 (40.63): A500, A600, A2000 */
    KICK_COUNT,
} kick_t;

/* Machine settings kept in NVS (namespace "ami") */
typedef struct {
    uint8_t model;          /* amiga_model_t */
    uint8_t kick;           /* kick_t */
    uint16_t chip_kb;       /* 512, 1024, 2048 (2 MB: the A600) */
    uint16_t slow_kb;       /* 0 or 512: the A500's and A2000's trapdoor RAM at $C00000 */
    uint8_t ntsc;
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* Does this model take that Kickstart / chip RAM / slow RAM? */
bool settings_kick_ok(int model, int kick);
bool settings_chip_ok(int model, int kb);
bool settings_slow_ok(int model, int kb);
/* A model's defaults */
void settings_model_defaults(int model);

/* The battery-backed clock's offset from the board's time (seconds) */
int64_t settings_load_rtc(void);
void settings_save_rtc(int64_t offset);
