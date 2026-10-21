# The web page's script for DMS archives and gzipped images (main/web)
set(embed "")
if(CONFIG_EMU_WEB)
    list(APPEND embed "web/amifiles.js")
endif()

idf_component_register(SRCS "main.c" "settings.c" "rom_store.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES ${embed}
                       PRIV_REQUIRES emu amiga esp_partition nvs_flash esp_psram)
