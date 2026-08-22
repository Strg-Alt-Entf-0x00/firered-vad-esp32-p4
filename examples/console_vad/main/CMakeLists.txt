idf_component_register(
    SRCS "main.cpp" "cmd_vad_cli.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_firevad
        vad_app
        console
        nvs_flash
        espressif__esp_codec_dev
        file_protocol
        filesystem_manager
    PRIV_REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_uart
        esp_driver_i2s            # New I2S STD/PDM/TDM driver (ESP-IDF v6.0+)
        esp_driver_sdmmc          # SDMMC host driver for SD card
        sdmmc                     # SD/MMC protocol driver
        fatfs                     # FAT filesystem for SD card
        esp_timer
        esp_pm
        esp_ringbuf
)

# Audio support configuration notes:
# - Uses modern ESP-IDF v6.0+ driver APIs
# - esp_driver_gpio: New GPIO driver
# - esp_driver_i2c: New I2C master bus driver
# - esp_driver_i2s: New I2S standard mode driver
# - Configure GPIO pins via menuconfig (Component config -> FireVAD -> Audio Input)
