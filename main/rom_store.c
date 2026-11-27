#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "emu/sdcard.h"
#include "rom_store.h"

#define ROM_PARTITION_TYPE  0x40
#define ROM_FOLDER          SDCARD_MOUNT_POINT "/amiga/roms"
#define EXT_SIZE            (512 * 1024)

static const char *TAG = "rom";
static const char *s_source;         /* where the last Kickstart came from */

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
 * "AROS ROM", "AROS EXT"), from its first 32 bytes and its checksum (all
 * longwords added with the carry round: $FFFFFFFF, amiga_kickstart_version()'s
 * test)
 */
static bool is_rom(kick_t kick, bool ext, const uint8_t *head, uint32_t sum)
{
    if (sum != 0xFFFFFFFF || head[0] != 0x11 || (head[1] != 0x11 && head[1] != 0x14)) {
        return false;
    }
    if (kick == KICK_AROS) {
        return memcmp(head + 0x18, ext ? "AROS EXT" : "AROS ROM", 8) == 0;
    }
    int major = (head[12] << 8) | head[13], minor = (head[14] << 8) | head[15];
    return !ext && major == s_kick[kick].major && minor == s_kick[kick].minor;
}

static uint32_t add_longs(uint32_t sum, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = ((uint32_t)p[i] << 24) | (p[i + 1] << 16) | (p[i + 2] << 8) | p[i + 3];
        uint32_t t = sum + w;
        sum = t + (t < sum);
    }
    return sum;
}

/* Where a ROM was found (checked): a mapping of its partition, or a file */
typedef struct {
    uint32_t size;
    const void *map;
    esp_partition_mmap_handle_t handle;
    char path[300];
} src_t;

/* Through a mapping, not esp_partition_read() (that turns the caches off
 * while it reads)
 */
static bool in_partition(kick_t kick, bool ext, src_t *s)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROM_PARTITION_TYPE,
                                                           ext ? s_kick[kick].ext_partition : s_kick[kick].partition);
    s->size = ext ? EXT_SIZE : s_kick[kick].size;
    s->map = NULL;
    if (part == NULL || part->size < s->size ||
        esp_partition_mmap(part, 0, s->size, ESP_PARTITION_MMAP_DATA, &s->map, &s->handle) != ESP_OK) {
        s->map = NULL;
        return false;
    }
    if (!is_rom(kick, ext, s->map, add_longs(0, s->map, s->size))) {
        esp_partition_munmap(s->handle);
        s->map = NULL;
        return false;
    }
    return true;
}

/* A file in the SD card's "amiga/roms" folder with that version, read a
 * piece at a time (the buffer it goes into is still the running machine's) */
static bool on_sd(kick_t kick, bool ext, src_t *s)
{
    DIR *dir = opendir(ROM_FOLDER);
    if (dir == NULL) {
        return false;
    }
    s->size = ext ? EXT_SIZE : s_kick[kick].size;
    s->map = NULL;
    bool ok = false;
    struct dirent *e;
    while (!ok && (e = readdir(dir)) != NULL) {
        struct stat st;
        snprintf(s->path, sizeof(s->path), "%s/%s", ROM_FOLDER, e->d_name);
        if (e->d_name[0] == '.' || stat(s->path, &st) != 0 || st.st_size != s->size) {
            continue;
        }
        FILE *f = fopen(s->path, "rb");
        if (f == NULL) {
            continue;
        }
        uint8_t piece[256], head[32];
        uint32_t sum = 0, at;
        for (at = 0; at < s->size && fread(piece, 1, sizeof(piece), f) == sizeof(piece); at += sizeof(piece)) {
            if (at == 0) {
                memcpy(head, piece, sizeof(head));
            }
            sum = add_longs(sum, piece, sizeof(piece));
        }
        fclose(f);
        ok = at == s->size && is_rom(kick, ext, head, sum);
    }
    closedir(dir);
    return ok;
}

/* Found on the card (it overrides the built-in one) or in the flash */
static bool find(kick_t kick, bool ext, src_t *s)
{
    return on_sd(kick, ext, s) || in_partition(kick, ext, s);
}

static void forget(src_t *s)
{
    if (s->map) {
        esp_partition_munmap(s->handle);
        s->map = NULL;
    }
}

static bool copy(src_t *s, uint8_t *buf, bool ext)
{
    bool ok;
    if (s->map) {
        memcpy(buf, s->map, s->size);
        forget(s);
        ok = true;
    } else {
        FILE *f = fopen(s->path, "rb");
        ok = f && fread(buf, 1, s->size, f) == s->size;
        if (f) {
            fclose(f);
        }
        if (ok) {
            ESP_LOGI(TAG, "%s from %s", ext ? "Extension ROM" : "Kickstart", s->path);
        }
    }
    return ok;
}

bool rom_store_load(kick_t kick, uint8_t *buf, uint8_t *ext_buf, uint32_t *size, const char **name)
{
    if ((unsigned)kick >= KICK_COUNT) {
        return false;
    }
    bool ext = rom_store_has_extension(kick);
    src_t k, x = { 0 };
    if (!find(kick, false, &k)) {
        return false;
    }
    if (ext && (ext_buf == NULL || !find(kick, true, &x))) {
        forget(&k);
        return false;
    }
    s_source = k.map ? "the flash" : "the SD card";
    /* (both there: only a card failing now leaves a buffer half written) */
    if (!copy(&k, buf, false) || (ext && !copy(&x, ext_buf, true))) {
        forget(&x);
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

const char *rom_store_source(void)
{
    return s_source;
}

bool rom_store_has_extension(kick_t kick)
{
    return (unsigned)kick < KICK_COUNT && s_kick[kick].ext_partition != NULL;
}
