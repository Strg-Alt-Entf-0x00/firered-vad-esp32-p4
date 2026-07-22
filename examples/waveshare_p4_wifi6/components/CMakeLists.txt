idf_component_register(
    SRCS "audio_hal_i2s.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2s esp_driver_gpio
)
