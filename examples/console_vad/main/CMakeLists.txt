idf_component_register(
    SRCS "main.cpp" "cmd_vad_cli.cpp" "vad_runner.cpp" "metrics.cpp" "audio_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_firevad
        console
        nvs_flash
        joltwallet__littlefs
        espressif__esp_codec_dev  # For audio codec support
    PRIV_REQUIRES
        driver                    # Legacy driver component (transitional)
        esp_driver_gpio           # New GPIO driver (ESP-IDF v6.0+)
        esp_driver_i2c            # New I2C master driver (ESP-IDF v6.0+)
        esp_driver_i2s            # New I2S STD/PDM/TDM driver (ESP-IDF v6.0+)
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
