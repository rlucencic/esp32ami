#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "ami"

static const char *TAG = "settings";

settings_t settings;

bool settings_kick_ok(int model, int kick)
{
    if (model == AMIGA_A600) {
        return kick == KICK_205 || kick == KICK_31;
    }
    return kick == KICK_13 || kick == KICK_204 || kick == KICK_31;
}

bool settings_chip_ok(int model, int kb)
{
    return kb == 512 || kb == 1024 || (kb == 2048 && model == AMIGA_A600);
}

bool settings_slow_ok(int model, int kb)
{
    return kb == 0 || (kb == 512 && model != AMIGA_A600);
}

void settings_model_defaults(int model)
{
    settings.model = (uint8_t)model;
    switch (model) {
    case AMIGA_A2000:
        settings.kick = KICK_13;
        settings.chip_kb = 1024;
        settings.slow_kb = 0;
        break;
    case AMIGA_A600:
        settings.kick = KICK_205;
        settings.chip_kb = 1024;
        settings.slow_kb = 0;
        break;
    default:
        settings.kick = KICK_13;
        settings.chip_kb = 512;
        settings.slow_kb = 512;
        break;
    }
}

void settings_load(void)
{
    settings_model_defaults(AMIGA_A500);
    settings.ntsc = 0;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 <= AMIGA_A600) {
        settings_model_defaults(u8);
    }
    if (nvs_get_u8(h, "kick", &u8) == ESP_OK && settings_kick_ok(settings.model, u8)) {
        settings.kick = u8;
    }
    if (nvs_get_u16(h, "chip_kb", &u16) == ESP_OK && settings_chip_ok(settings.model, u16)) {
        settings.chip_kb = u16;
    }
    if (nvs_get_u16(h, "slow_kb", &u16) == ESP_OK && settings_slow_ok(settings.model, u16)) {
        settings.slow_kb = u16;
    }
    if (nvs_get_u8(h, "ntsc", &u8) == ESP_OK && u8 <= 1) {
        settings.ntsc = u8;
    }
    nvs_close(h);
}

int64_t settings_load_rtc(void)
{
    int64_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i64(h, "rtc_ofs", &v);
        nvs_close(h);
    }
    return v;
}

void settings_save_rtc(int64_t offset)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "rtc_ofs", offset);
        nvs_commit(h);
        nvs_close(h);
    }
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u8(h, "kick", settings.kick);
    nvs_set_u16(h, "chip_kb", settings.chip_kb);
    nvs_set_u16(h, "slow_kb", settings.slow_kb);
    nvs_set_u8(h, "ntsc", settings.ntsc);
    nvs_commit(h);
    nvs_close(h);
}
