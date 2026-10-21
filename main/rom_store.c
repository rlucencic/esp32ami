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

static const char *TAG = "rom";

static const struct {
    const char *partition;
    uint32_t size;
    int major, minor;
    const char *name;
} s_kick[KICK_COUNT] = {
    [KICK_13] = { "kick_130", 256 * 1024, 34, 5, "Kickstart 1.3 (34.5)" },
    [KICK_204] = { "kick_204", 512 * 1024, 37, 175, "Kickstart 2.04 (37.175)" },
    [KICK_205] = { "kick_205_a600", 512 * 1024, 37, 350, "Kickstart 2.05 (37.350)" },
    [KICK_31] = { "kick_310_a600", 512 * 1024, 40, 63, "Kickstart 3.1 (40.63)" },
};

static bool is_kick(kick_t kick, const uint8_t *buf, uint32_t size)
{
    int major, minor;
    return amiga_kickstart_version(buf, size, &major, &minor) && major == s_kick[kick].major &&
           minor == s_kick[kick].minor;
}

static bool from_partition(kick_t kick, uint8_t *buf)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROM_PARTITION_TYPE,
                                                           s_kick[kick].partition);
    uint32_t size = s_kick[kick].size;
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
    return is_kick(kick, buf, size);
}

/* Any file in the SD card's "roms" folder with that version */
static bool from_sd(kick_t kick, uint8_t *buf)
{
    DIR *dir = opendir(ROM_FOLDER);
    if (dir == NULL) {
        return false;
    }
    uint32_t size = s_kick[kick].size;
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
        ok = fread(buf, 1, size, f) == size && is_kick(kick, buf, size);
        fclose(f);
        if (ok) {
            ESP_LOGI(TAG, "Kickstart from %s", path);
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
    if (!from_partition(kick, buf) && !from_sd(kick, buf)) {
        return false;
    }
    *size = s_kick[kick].size;
    *name = s_kick[kick].name;
    return true;
}
