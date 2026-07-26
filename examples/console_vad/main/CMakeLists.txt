idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_firevad
        console
        nvs_flash
        joltwallet__littlefs
)
