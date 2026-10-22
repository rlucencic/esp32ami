#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "emu/sdcard.h"
#include "rom_store.h"

#define ROM_PARTITION_TYPE  0x40
#define ROM_FOLDER          SDCARD_MOUNT_POINT "/roms"
#define EXT_SIZE            (512 * 1024)

static const char *TAG = "rom";

static const struct {
    const char *partition;
    uint32_t size;
    int major, minor;               /* (AROS: any version) */
    const char *name;
    const char *ext_partition;      /* its extension ROM's (NULL: none) */
} s_kick[KICK_COUNT] = {
    [KICK_12] = { "kick_120", 256 * 1024, 33, 180, "Kickstart 1.2 (33.180)", NULL },
    [KICK_13] = { "kick_130", 256 * 1024, 34, 5, "Kickstart 1.3 (34.5)", NULL },
    [KICK_204] = { "kick_204", 512 * 1024, 37, 175, "Kickstart 2.04 (37.175)", NULL },
    [KICK_205] = { "kick_205_a600", 512 * 1024, 37, 350, "Kickstart 2.05 (37.350)", NULL },
    [KICK_31] = { "kick_310_a600", 512 * 1024, 40, 63, "Kickstart 3.1 (40.63)", NULL },
    [KICK_AROS] = { "kick_aros", 512 * 1024, 0, 0, NULL, "kick_aros_ext" },
    [KICK_30] = { "kick_300_a1200", 512 * 1024, 39, 106, "Kickstart 3.0 (39.106)", NULL },
    [KICK_31_A1200] = { "kick_310_a1200", 512 * 1024, 40, 68, "Kickstart 3.1 (40.68)", NULL },
};

/* That Kickstart, or its extension ROM (AROS's say what they are at $18:
 * "AROS ROM", "AROS EXT")
 */
static bool is_rom(kick_t kick, bool ext, const uint8_t *buf, uint32_t size)
{
    int major, minor;
    if (!amiga_kickstart_version(buf, size, &major, &minor)) {
        return false;
    }
    if (kick == KICK_AROS) {
        return memcmp(buf + 0x18, ext ? "AROS EXT" : "AROS ROM", 8) == 0;
    }
    return !ext && major == s_kick[kick].major && minor == s_kick[kick].minor;
}

static bool from_partition(kick_t kick, bool ext, uint8_t *buf)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROM_PARTITION_TYPE,
                                                           ext ? s_kick[kick].ext_partition : s_kick[kick].partition);
    uint32_t size = ext ? EXT_SIZE : s_kick[kick].size;
    if (part == NULL || part->size < size) {
        return false;
    }
    /* Through a mapping, not esp_partition_read() (that turns the caches off
     * while it reads)
     */
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        return false;
    }
    memcpy(buf, map, size);
    esp_partition_munmap(handle);
    return is_rom(kick, ext, buf, size);
}

/* Any file in the SD card's "roms" folder with that version */
static bool from_sd(kick_t kick, bool ext, uint8_t *buf)
{
    DIR *dir = opendir(ROM_FOLDER);
    if (dir == NULL) {
        return false;
    }
    uint32_t size = ext ? EXT_SIZE : s_kick[kick].size;
    bool ok = false;
    struct dirent *e;
    while (!ok && (e = readdir(dir)) != NULL) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", ROM_FOLDER, e->d_name);
        if (e->d_name[0] == '.' || stat(path, &st) != 0 || st.st_size != size) {
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            continue;
        }
        ok = fread(buf, 1, size, f) == size && is_rom(kick, ext, buf, size);
        fclose(f);
        if (ok) {
            ESP_LOGI(TAG, "%s from %s", ext ? "Extension ROM" : "Kickstart", path);
        }
    }
    closedir(dir);
    return ok;
}

bool rom_store_load(kick_t kick, uint8_t *buf, uint32_t *size, const char **name)
{
    if ((unsigned)kick >= KICK_COUNT) {
        return false;
    }
    if (!from_partition(kick, false, buf) && !from_sd(kick, false, buf)) {
        return false;
    }
    *size = s_kick[kick].size;
    *name = s_kick[kick].name;
    if (*name == NULL) {
        static char aros[32];
        int major, minor;
        amiga_kickstart_version(buf, *size, &major, &minor);
        snprintf(aros, sizeof(aros), "AROS (%d.%d)", major, minor);
        *name = aros;
    }
    return true;
}

bool rom_store_has_extension(kick_t kick)
{
    return (unsigned)kick < KICK_COUNT && s_kick[kick].ext_partition != NULL;
}

bool rom_store_load_extension(kick_t kick, uint8_t *buf)
{
    return rom_store_has_extension(kick) && (from_partition(kick, true, buf) || from_sd(kick, true, buf));
}
