idf_component_register(SRCS "src/audio_manager.cpp"
                            "src/benchmark.cpp"
                            "src/dual_core_vad.cpp"
                            "src/metrics.cpp"
                            "src/vad_runner.cpp"
                            "src/dsp_pipeline.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_firevad esp_codec_dev esp-dsp esp_driver_i2c esp_driver_i2s esp_timer esp_ringbuf
                    PRIV_REQUIRES fatfs nvs_flash)
