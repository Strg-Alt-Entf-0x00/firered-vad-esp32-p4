#include "vad_runner.h"
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_attr.h"
#include "esp_firevad.h"
#include "esp_firevad_dsp.h"
#include "config.h"
#include "metrics.h"
#include "audio_manager.h"
#include "freertos/FreeRTOS.h"

// DUAL-CORE SUPPORT
// SESSION 15: Test showed no difference, re-enabling for production
#define ENABLE_DUAL_CORE 1
#if ENABLE_DUAL_CORE
#include "dual_core_vad.h"
static dual_core_vad_t g_dual_core_vad = {};
static bool g_dual_core_active = false;
#endif

static const char* TAG = "VAD_RUNNER";

static EspFirevadModel g_model = {};
static EspFirevadModel g_offline_model = {};
static uint8_t* g_model_buffer = NULL;
static uint8_t* g_offline_model_buffer = NULL;
static bool g_model_loaded = false;
static bool g_cascade_active = false;
static float g_frame_features[FEATURES_PER_FRAME] = {0.0f};
static float g_frame_probs[8] = {0.0f};

// Pre-VAD Variables
static float g_baseline_noise_energy = 0.0f;
static float g_pre_vad_multiplier = 0.0f; // 0.0f means disabled
static bool g_is_calibrating = false;
static int g_calibration_frames_left = 0;
static float g_calibration_energy_sum = 0.0f;
static int g_calibration_total_frames = 0;

bool vad_runner_is_model_loaded(void) {
    return g_model_loaded;
}

bool vad_runner_is_causal(void) {
    if (!g_model_loaded) return false;
    return g_model.arch.N2 == 0;
}

int vad_runner_load_model(const char* filename) {
    char path[MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/%s", FS_MOUNT_POINT, filename);
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open '%s'", path);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    ESP_LOGI(TAG, "Loading model (%zu KB)...", file_size / 1024);
    
    vad_runner_free_model();
    
    g_model_buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!g_model_buffer) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM", file_size);
        fclose(f);
        return 1;
    }
    
    size_t bytes_read = 0;
    while (bytes_read < file_size) {
        size_t remaining = file_size - bytes_read;
        size_t chunk_size = remaining > 64 * 1024 ? 64 * 1024 : remaining;
        size_t chunk_read = fread(g_model_buffer + bytes_read, 1, chunk_size, f);
        if (chunk_read == 0) break;
        bytes_read += chunk_read;
    }
    fclose(f);
    
    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "Failed to read complete file (%zu/%zu bytes)", bytes_read, file_size);
        heap_caps_free(g_model_buffer);
        g_model_buffer = NULL;
        return 1;
    }
    
    int ret = esp_firevad_load(g_model_buffer, file_size, &g_model);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse FireVAD model (error: %d)", ret);
        vad_runner_free_model();
        return 1;
    }
    
    esp_firevad_reset(&g_model);
    g_model_loaded = true;
    metrics_reset();

#if ENABLE_DUAL_CORE
    // Initialize dual-core VAD for single model
    if (dual_core_vad_init(&g_dual_core_vad, &g_model, nullptr) == ESP_OK) {
        ESP_LOGI(TAG, "Dual-Core VAD initialized successfully");
    } else {
        ESP_LOGW(TAG, "Failed to initialize Dual-Core VAD");
    }
#endif

    if (vad_runner_is_stream_model()) {
        ESP_LOGI(TAG, "Model family: stream-vad (live microphone supported)");
    } else if (g_model.arch.odim == 3) {
        ESP_LOGI(TAG, "Model family: aed (offline classification)");
    } else {
        ESP_LOGI(TAG, "Model family: vad (offline chunk processing)");
    }

    return 0;
}

void vad_runner_free_model(void) {
    if (g_model_loaded) {
#if ENABLE_DUAL_CORE
        if (g_dual_core_active) {
            dual_core_vad_stop(&g_dual_core_vad);
            g_dual_core_active = false;
        }
        dual_core_vad_deinit(&g_dual_core_vad);
#endif
        esp_firevad_free(&g_model);
        if (g_model_buffer) {
            heap_caps_free(g_model_buffer);
            g_model_buffer = NULL;
        }
        if (g_cascade_active) {
            esp_firevad_free(&g_offline_model);
            if (g_offline_model_buffer) {
                heap_caps_free(g_offline_model_buffer);
                g_offline_model_buffer = NULL;
            }
            g_cascade_active = false;
        }
        g_model_loaded = false;
    }
}

bool vad_runner_is_cascade_active(void) {
    return g_cascade_active;
}

// Helper to load a single model into a specific buffer and model pointer
static int internal_load_model(const char* filename, uint8_t** buffer_ptr, EspFirevadModel* model_ptr) {
    char path[MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/%s", FS_MOUNT_POINT, filename);
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open '%s'", path);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    *buffer_ptr = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!*buffer_ptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM", file_size);
        fclose(f);
        return 1;
    }
    
    size_t bytes_read = 0;
    while (bytes_read < file_size) {
        size_t remaining = file_size - bytes_read;
        size_t chunk_size = remaining > 64 * 1024 ? 64 * 1024 : remaining;
        size_t chunk_read = fread(*buffer_ptr + bytes_read, 1, chunk_size, f);
        if (chunk_read == 0) break;
        bytes_read += chunk_read;
    }
    fclose(f);
    
    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "Failed to read complete file (%zu/%zu bytes)", bytes_read, file_size);
        heap_caps_free(*buffer_ptr);
        *buffer_ptr = NULL;
        return 1;
    }
    
    int ret = esp_firevad_load(*buffer_ptr, file_size, model_ptr);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse FireVAD model (error: %d)", ret);
        heap_caps_free(*buffer_ptr);
        *buffer_ptr = NULL;
        return 1;
    }
    
    esp_firevad_reset(model_ptr);
    return 0;
}

int vad_runner_load_cascade(const char* stream_filename, const char* offline_filename) {
    vad_runner_free_model();
    
    ESP_LOGI(TAG, "Loading cascade models...");
    
    if (internal_load_model(stream_filename, &g_model_buffer, &g_model) != 0) {
        return 1;
    }
    
    if (internal_load_model(offline_filename, &g_offline_model_buffer, &g_offline_model) != 0) {
        vad_runner_free_model();
        return 1;
    }
    
    g_model_loaded = true;
    g_cascade_active = true;
    metrics_reset();

#if ENABLE_DUAL_CORE
    if (dual_core_vad_init(&g_dual_core_vad, &g_model, &g_offline_model) == ESP_OK) {
        ESP_LOGI(TAG, "Cascade Dual-Core VAD initialized successfully");
    } else {
        ESP_LOGW(TAG, "Failed to initialize Cascade Dual-Core VAD");
    }
#endif

    return 0;
}

void vad_runner_print_info(void) {
    if (!g_model_loaded) return;

    const char* type_str = "Unknown";
    if (g_model.arch.odim == 1) {
        type_str = (g_model.arch.N2 == 0) ? "Stream-VAD" : "Offline VAD";
    } else if (g_model.arch.odim == 3) {
        type_str = "AED (Audio Event Detection)";
    }

    const char* prec_str = "Unknown";
    if (g_model.version == 1) prec_str = "Float32";
    else if (g_model.version == 2) prec_str = "Int8";
    else if (g_model.version == 3) prec_str = "Int16";
    else if (g_model.version == 4) prec_str = "Int8-CH";

    const char* mode_str = (g_model.arch.N2 == 0) ? "CAUSAL (Streaming)" : "NON-CAUSAL (Offline)";

    size_t mem_usage = esp_firevad_memory_usage(&g_model);

    printf("\n=== Model Information ===\n");
    printf("Type:         %s\n", type_str);
    printf("Precision:    %s\n", prec_str);
    printf("Mode:         %s\n", mode_str);
    printf("Memory Usage: %zu KB\n", mem_usage / 1024);
    printf("\nArchitecture:\n");
    printf("  D (Input):    %" PRIu32 "\n", g_model.arch.D);
    printf("  H (Hidden):   %" PRIu32 "\n", g_model.arch.H);
    printf("  P (Proj):     %" PRIu32 "\n", g_model.arch.P);
    printf("  odim:         %" PRIu32, g_model.arch.odim);
    if (g_model.arch.odim == 3) {
        printf(" (Speech/Music/Singing)\n");
    } else {
        printf(" (Speech)\n");
    }
    printf("  N1 (Past):    %" PRIu32 " frames (%.1f ms)\n", g_model.arch.N1, g_model.arch.N1 * 10.0f);
    printf("  N2 (Future):  %" PRIu32 " frames (%.1f ms)\n", g_model.arch.N2, g_model.arch.N2 * 10.0f);
    printf("\n");
}

bool vad_runner_is_stream_model(void) {
    if (!g_model_loaded) return false;
    return (g_model.arch.odim == 1 && g_model.arch.N2 == 0);
}

bool vad_runner_is_offline_model(void) {
    if (!g_model_loaded) return false;
    return !vad_runner_is_stream_model();
}

bool vad_runner_is_realtime_compatible(void) {
    if (!g_model_loaded) return false;
    if (!vad_runner_is_stream_model()) return false;
    return (g_model.version == 2 || g_model.version == 4);
}

const char* vad_runner_model_family_name(void) {
    if (!g_model_loaded) return "none";
    if (g_model.arch.odim == 3) return "aed";
    if (g_model.arch.N2 == 0) return "stream-vad";
    return "vad";
}

void vad_runner_reset(void) {
    if (g_model_loaded) {
        esp_firevad_reset(&g_model);
    }
    esp_firevad_dsp_reset();
}

void vad_runner_calibrate_noise(int seconds) {
    if (seconds <= 0) seconds = 1;
    // 100 frames per second (10ms per frame)
    g_calibration_total_frames = seconds * 100;
    g_calibration_frames_left = g_calibration_total_frames;
    g_calibration_energy_sum = 0.0f;
    g_is_calibrating = true;
    ESP_LOGI(TAG, "Starting noise calibration for %d seconds...", seconds);
}

void vad_runner_calibrate_noise_blocking(int seconds) {
    if (seconds <= 0) seconds = 1;
    
    ESP_LOGI(TAG, "Starting blocking noise calibration for %d seconds...", seconds);
    
    audio_manager_start_capture();
    
    // Flush I2S DMA buffers to remove startup pop (read 200ms of data)
    int16_t pcm_frame[160];
    for (int i = 0; i < 20; i++) {
        audio_manager_read(pcm_frame, 160, portMAX_DELAY);
    }
    
    int total_frames = seconds * 100; // 100 frames/sec (10ms each)
    float energy_sum = 0.0f;
    float max_energy = 0.0f;
    
    for (int i = 0; i < total_frames; i++) {
        int read = audio_manager_read(pcm_frame, 160, portMAX_DELAY);
        if (read > 0) {
            float dummy[80];
            float energy = 0.0f;
            vad_runner_extract_features(pcm_frame, dummy, &energy);
            energy_sum += energy;
            if (energy > max_energy) max_energy = energy;
        }
    }
    
    audio_manager_stop_capture();
    
    g_baseline_noise_energy = energy_sum / total_frames;
    ESP_LOGI(TAG, "Calibration Complete.");
    ESP_LOGI(TAG, " -> Average Noise Energy: %.2f", g_baseline_noise_energy);
    ESP_LOGI(TAG, " -> Peak Noise Energy   : %.2f", max_energy);
}

void vad_runner_set_pre_vad_threshold(float multiplier) {
    g_pre_vad_multiplier = multiplier;
    if (multiplier > 0.0f) {
        ESP_LOGI(TAG, "Pre-VAD Enabled. Multiplier: %.2f (Baseline: %.2f, Threshold: %.2f)", 
                 multiplier, g_baseline_noise_energy, g_baseline_noise_energy * multiplier);
    } else {
        ESP_LOGI(TAG, "Pre-VAD Disabled.");
    }
}

float vad_runner_get_pre_vad_multiplier(void) {
    return g_pre_vad_multiplier;
}

// Silent version for internal use (e.g. sleep loop bypass - avoids log spam)
void vad_runner_set_pre_vad_threshold_silent(float multiplier) {
    g_pre_vad_multiplier = multiplier;
}


void IRAM_ATTR vad_runner_extract_features(int16_t* pcm_samples, float* features, float* out_energy) {
    esp_firevad_dsp_extract_features(pcm_samples, features, out_energy);
}

float IRAM_ATTR vad_runner_infer_frame(int16_t* pcm_frame) {
    if (!g_model_loaded) return 0.0f;

    float energy = 0.0f;
    
#if ENABLE_DUAL_CORE
    // SESSION 18: SINGLE-CORE FAST PATH for INT8 models
    // INT8 inference is fast enough (~5.5ms) to run inline on Core 0
    // This avoids dual-core overhead (mutex, memcpy, wake-up, cache coherency)
    // Expected improvement: -0.3 to -0.4ms
    bool use_single_core_fast_path = (g_model.is_int8 && !g_model.is_int16) && !g_cascade_active;
    
    if (!use_single_core_fast_path && !g_dual_core_active) {
        // DUAL-CORE PATH: Start dual-core on first frame (for FP32/INT16 or Cascade)
        if (dual_core_vad_start(&g_dual_core_vad) == ESP_OK) {
            g_dual_core_active = true;
            ESP_LOGI(TAG, "[OK] DUAL-CORE MODE ACTIVATED!");
        }
    }
    
    if (!use_single_core_fast_path && g_dual_core_active) {
        // DUAL-CORE PATH: Used for FP32/INT16 (slower models that benefit from parallelism)
        // Submit frame to Core 0 processing (feature extraction)
        // This extracts features and submits to Core 1 for inference
        // Note: May return ESP_ERR_TIMEOUT if Core 1 is slower than Core 0 (expected for FP32/INT16)
        dual_core_vad_submit_frame(&g_dual_core_vad, pcm_frame, g_frame_features);
        
        // ALWAYS try to get the latest completed result, regardless of submit status
        // This is crucial for FP32/INT16 where Core 1 is slower and ESP_ERR_TIMEOUT is common
        uint32_t inference_time_us = 0;
        uint32_t inference_cycles = 0;
        if (dual_core_vad_get_result(&g_dual_core_vad, g_frame_probs, &inference_time_us, &inference_cycles)) {
            // Got a completed result - record metrics if valid
            if (inference_time_us > 0) {
                metrics_record_inference(inference_time_us, inference_cycles);
            } else {
                static int warn_count = 0;
                if (warn_count++ < 3) {
                    ESP_LOGW(TAG, "Got result but inference_time_us is 0!");
                }
            }
            return g_frame_probs[0];
        }
        
        // No completed results yet (happens in first few frames)
        return 0.0f;
    }
#endif
    
    // SINGLE-CORE PATH (fallback OR fast path for INT8)
    // SESSION 18: This path is now used for:
    // 1. INT8 models (fast path - intentional)
    // 2. When dual-core is disabled (fallback)
    // 3. When dual-core initialization fails (fallback)
    vad_runner_extract_features(pcm_frame, g_frame_features, &energy);
    
    if (g_is_calibrating) {
        g_calibration_energy_sum += energy;
        g_calibration_frames_left--;
        if (g_calibration_frames_left <= 0) {
            g_is_calibrating = false;
            g_baseline_noise_energy = g_calibration_energy_sum / g_calibration_total_frames;
            ESP_LOGI(TAG, "Calibration Complete. Baseline Noise Energy: %.2f", g_baseline_noise_energy);
        }
        return 0.0f; // Return silence during calibration
    }

    if (g_pre_vad_multiplier > 0.0f) {
        float threshold = g_baseline_noise_energy * g_pre_vad_multiplier;
        if (energy < threshold) {
            // Silence detected, skip NN
            return 0.0f;
        }
    }
    
    uint32_t start_cycles = esp_cpu_get_cycle_count();
    uint64_t start_time = esp_timer_get_time();

    esp_firevad_infer_frame(&g_model, g_frame_features, true, g_frame_probs);

    uint64_t end_time = esp_timer_get_time();
    uint32_t end_cycles = esp_cpu_get_cycle_count();

    metrics_record_inference((uint32_t)(end_time - start_time), end_cycles - start_cycles);

    return g_frame_probs[0];
}

void vad_runner_infer_chunk(float* chunk_features, size_t frames, float* chunk_probs) {
    if (!g_model_loaded) return;
    if (vad_runner_is_stream_model()) {
        ESP_LOGW(TAG, "Chunk inference was called on a Stream-VAD model; this path is intended for offline models.");
    }
    esp_firevad_infer_chunk(&g_model, chunk_features, frames, true, chunk_probs);
}

int vad_runner_dump_golden(const char* wav_filename) {
    if (!g_model_loaded) {
        ESP_LOGE(TAG, "No model loaded.");
        return 1;
    }
    
    char path[256];
    snprintf(path, sizeof(path), "/sd/audio/vad/%s", wav_filename);
    
    FILE* f_wav = fopen(path, "rb");
    if (!f_wav) {
        ESP_LOGE(TAG, "Failed to open WAV: %s", path);
        return 1;
    }
    
    FILE* f_pcm = fopen("/sd/golden_esp32_pcm.bin", "wb");
    FILE* f_pre = fopen("/sd/golden_esp32_fbank_pre.bin", "wb");
    FILE* f_post = fopen("/sd/golden_esp32_fbank_post.bin", "wb");
    FILE* f_probs = fopen("/sd/golden_esp32_probs.bin", "wb");
    
    if (!f_pcm || !f_pre || !f_post || !f_probs) {
        ESP_LOGE(TAG, "Failed to open output dump files on SD card.");
        if (f_wav) fclose(f_wav);
        if (f_pcm) fclose(f_pcm);
        if (f_pre) fclose(f_pre);
        if (f_post) fclose(f_post);
        if (f_probs) fclose(f_probs);
        return 1;
    }
    
    // Check if it's a WAV file to skip header
    const char* ext = strrchr(wav_filename, '.');
    if (ext != NULL && strcasecmp(ext, ".wav") == 0) {
        ESP_LOGI(TAG, "Parsing WAV header to find 'data' chunk...");
        uint8_t header[12];
        if (fread(header, 1, 12, f_wav) == 12) {
            if (memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
                bool data_found = false;
                while (!data_found) {
                    uint8_t chunk_header[8];
                    if (fread(chunk_header, 1, 8, f_wav) != 8) break;
                    
                    uint32_t chunk_size = chunk_header[4] | (chunk_header[5] << 8) | (chunk_header[6] << 16) | (chunk_header[7] << 24);
                    
                    if (memcmp(chunk_header, "data", 4) == 0) {
                        ESP_LOGI(TAG, "Found 'data' chunk at offset %ld, size: %lu bytes", ftell(f_wav), chunk_size);
                        data_found = true;
                    } else {
                        ESP_LOGD(TAG, "Skipping chunk '%.4s' size %lu", chunk_header, chunk_size);
                        fseek(f_wav, chunk_size, SEEK_CUR);
                    }
                }
                if (!data_found) {
                    ESP_LOGW(TAG, "Could not find 'data' chunk, rewinding to byte 44 as fallback.");
                    fseek(f_wav, 44, SEEK_SET);
                }
            } else {
                ESP_LOGW(TAG, "Not a valid RIFF/WAVE file, rewinding.");
                rewind(f_wav);
            }
        } else {
            rewind(f_wav);
        }
    }
    esp_firevad_reset(&g_model);
    esp_firevad_dsp_reset();
    
    const size_t frame_size = 160; 
    int16_t pcm_frame[160];
    float features_80[80];
    
    ESP_LOGI(TAG, "Dumping golden data from %s...", wav_filename);
    uint32_t frames = 0;
    
    while (fread(pcm_frame, sizeof(int16_t), frame_size, f_wav) == frame_size) {
        // Write PCM
        fwrite(pcm_frame, sizeof(int16_t), frame_size, f_pcm);
        
        // Extract Fbank
        esp_firevad_dsp_extract_features(pcm_frame, features_80, NULL);
        fwrite(features_80, sizeof(float), 80, f_pre);
        
        // Apply CMVN manually so we can dump post-cmvn
        for (int i = 0; i < 80; i++) {
            features_80[i] = (features_80[i] - g_model.cmvn_means[i]) * g_model.cmvn_istd[i];
        }
        fwrite(features_80, sizeof(float), 80, f_post);
        
        // Infer (bypassing dual core) - apply_cmvn is false because we already did it
        float prob = 0.0f;
        esp_firevad_infer_frame(&g_model, features_80, false, &prob);
        fwrite(&prob, sizeof(float), 1, f_probs);
        
        frames++;
        if (frames % 50 == 0) {
            vTaskDelay(pdMS_TO_TICKS(10)); // Prevent WDT
            ESP_LOGI(TAG, "Processed %" PRIu32 " frames...", frames);
        }
    }
    
    fclose(f_wav);
    fclose(f_pcm);
    fclose(f_pre);
    fclose(f_post);
    fclose(f_probs);
    
    ESP_LOGI(TAG, "Golden dump completed! Total frames: %" PRIu32, frames);
    return 0;
}
