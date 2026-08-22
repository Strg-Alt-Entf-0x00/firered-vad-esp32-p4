idf_component_register(SRCS "src/esp_firevad.cpp" "src/esp_firevad_dsp.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp-dsp esp-nn esp_timer)

# Aggressive compiler optimization flags for FireRedVAD inference performance
# These flags provide ~6% performance improvement over standard -O2
target_compile_options(${COMPONENT_LIB} PRIVATE 
    -Ofast                    # Enable all -O3 optimizations + aggressive FP math
    -funroll-loops            # Force loop unrolling for FSMN/Dense layers
    -fno-strict-aliasing      # Allow aggressive pointer optimization
    -falign-functions=32      # 32-byte function alignment for ESP32-P4 cache (32KB L1)
    -falign-loops=16          # 16-byte loop alignment for better instruction fetch
)
