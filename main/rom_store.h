#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

#define ROM_MAX_SIZE    (512 * 1024)

/* Load Kickstart `kick` into buf (ROM_MAX_SIZE bytes): from its flash
 * partition (roms/README.md: written by idf.py flash), else from a file in
 * the SD card's "roms" folder with that version. Its size in *size, its
 * name ("Kickstart 1.3 (34.5)") in *name.
 */
bool rom_store_load(kick_t kick, uint8_t *buf, uint32_t *size, const char **name);

/* Its extension ROM (AROS's, for $E00000): 512 KB into buf, the same ways */
bool rom_store_has_extension(kick_t kick);
bool rom_store_load_extension(kick_t kick, uint8_t *buf);
