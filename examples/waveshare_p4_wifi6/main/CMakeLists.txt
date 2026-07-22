idf_component_register(SRCS "main.cpp" "synth_fx.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_codec_dev esp_firevad spiffs fatfs console)
