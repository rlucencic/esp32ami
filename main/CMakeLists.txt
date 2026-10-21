idf_component_register(SRCS "main.c" "settings.c" "rom_store.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES emu amiga esp_partition nvs_flash esp_psram)
