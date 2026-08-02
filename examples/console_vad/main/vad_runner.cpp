#include "vad_runner.h"
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_firevad.h"
#include "esp_firevad_dsp.h"
#include "config.h"
#include "metrics.h"
#include "audio_manager.h"
#include "freertos/FreeRTOS.h"

static const char* TAG = "VAD_RUNNER";

static EspFirevadModel g_model = {};
static uint8_t* g_model_buffer = NULL;
static bool g_model_loaded = false;

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
    
    size_t bytes_read = fread(g_model_buffer, 1, file_size, f);
    fclose(f);
    
    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "Failed to read complete file");
        vad_runner_free_model();
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
    
    return 0;
}

void vad_runner_free_model(void) {
    if (g_model_loaded) {
        esp_firevad_free(&g_model);
        if (g_model_buffer) {
            heap_caps_free(g_model_buffer);
            g_model_buffer = NULL;
        }
        g_model_loaded = false;
    }
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
        printf("\n");
    }
    printf("  N1 (Past):    %" PRIu32 " frames (%.1f ms)\n", g_model.arch.N1, g_model.arch.N1 * 10.0f);
    printf("  N2 (Future):  %" PRIu32 " frames (%.1f ms)\n", g_model.arch.N2, g_model.arch.N2 * 10.0f);
    printf("\n");
}

bool vad_runner_is_stream_model(void) {
    if (!g_model_loaded) return false;
    return (g_model.arch.odim == 1 && g_model.arch.N2 == 0);
}

void vad_runner_reset(void) {
    if (g_model_loaded) {
        esp_firevad_reset(&g_model);
    }
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
    
    int total_frames = seconds * 100; // 100 frames/sec (10ms each)
    float energy_sum = 0.0f;
    int16_t pcm_frame[160];
    
    for (int i = 0; i < total_frames; i++) {
        int read = audio_manager_read(pcm_frame, 160, portMAX_DELAY);
        if (read > 0) {
            float dummy[80];
            float energy = 0.0f;
            vad_runner_extract_features(pcm_frame, dummy, &energy);
            energy_sum += energy;
        }
    }
    
    audio_manager_stop_capture();
    
    g_baseline_noise_energy = energy_sum / total_frames;
    ESP_LOGI(TAG, "Calibration Complete. Baseline Noise Energy: %.2f", g_baseline_noise_energy);
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


void vad_runner_extract_features(int16_t* pcm_samples, float* features, float* out_energy) {
    esp_firevad_dsp_extract_features(pcm_samples, features, out_energy);
}

float vad_runner_infer_frame(int16_t* pcm_frame) {
    if (!g_model_loaded) return 0.0f;
    
    float features[FEATURES_PER_FRAME];
    float energy = 0.0f;
    vad_runner_extract_features(pcm_frame, features, &energy);
    
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
    
    float probs[8] = {0.0f};
    
    uint32_t start_cycles = esp_cpu_get_cycle_count();
    uint64_t start_time = esp_timer_get_time();
    
    esp_firevad_infer_frame(&g_model, features, true, probs);
    
    uint64_t end_time = esp_timer_get_time();
    uint32_t end_cycles = esp_cpu_get_cycle_count();
    
    metrics_record_inference((uint32_t)(end_time - start_time), end_cycles - start_cycles);
    
    return probs[0];
}

void vad_runner_infer_chunk(float* chunk_features, size_t frames, float* chunk_probs) {
    if (!g_model_loaded) return;
    esp_firevad_infer_chunk(&g_model, chunk_features, frames, true, chunk_probs);
}
