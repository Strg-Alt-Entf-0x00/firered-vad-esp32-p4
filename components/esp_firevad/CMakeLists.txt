idf_component_register(SRCS "src/esp_firevad.cpp" "src/esp_firevad_dsp.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp-dsp esp_timer)
