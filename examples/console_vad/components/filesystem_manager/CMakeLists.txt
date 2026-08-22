idf_component_register(
    SRCS
        "filesystem_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        log
        vfs
        fatfs
        sdmmc
        esp_psram
        spiffs
)
