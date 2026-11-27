#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

#define ROM_MAX_SIZE    (512 * 1024)

/* Load Kickstart `kick` into buf (ROM_MAX_SIZE bytes) and, when it has one
 * (AROS), its extension ROM into ext_buf (512 KB): built in, from their
 * flash partitions (roms/README.md: written by idf.py flash); a file in the
 * SD card's "amiga/roms" folder with that version overrides one. Both are
 * found and checked before either buffer is written, so on false both are
 * as they were (they are the running machine's working copies). Its size
 * in *size, its name ("Kickstart 1.3 (34.5)") in *name.
 */
bool rom_store_load(kick_t kick, uint8_t *buf, uint8_t *ext_buf, uint32_t *size, const char **name);
/* Where the last one loaded came from: "the flash", "the SD card" (NULL: none yet) */
const char *rom_store_source(void);

/* Whether it has an extension ROM (AROS's, for $E00000) */
bool rom_store_has_extension(kick_t kick);
