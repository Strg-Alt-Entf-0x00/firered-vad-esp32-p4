#include "dsp_pipeline.h"
#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"
#include "config.h"

static const char* TAG = "DSP_PIPELINE";

static agc_config_t g_agc_cfg = {
    .enabled = false,
    .target_rms = 1500.0f,
    .max_gain = 25.0f,
    .min_gain = 1.0f,
    .attack_speed = 0.1f,   // Fast attack (reduce gain quickly when too loud)
    .decay_speed = 0.002f,  // Very slow decay (increase gain slowly during speech)
    .noise_gate_rms = 150.0f, 
    
    .current_gain = 1.0f,
    .smoothed_rms = 0.0f
};

// IIR Biquad Filter state
static float g_biquad_coeffs[5];
static float g_biquad_w[2];
static bool g_biquad_initialized = false;
static float* g_f32_buffer = NULL;
static size_t g_f32_buffer_size = 0;

void dsp_pipeline_init(void) {
    g_agc_cfg.current_gain = 1.0f;
    g_agc_cfg.smoothed_rms = 0.0f;
    
    // Initialize High-Pass Filter (80 Hz cutoff at FIREVAD_SAMPLE_RATE)
    esp_err_t ret = dsps_biquad_gen_hpf_f32(g_biquad_coeffs, 80.0f / (float)FIREVAD_SAMPLE_RATE, 0.707f);
    if (ret == ESP_OK) {
        g_biquad_w[0] = 0;
        g_biquad_w[1] = 0;
        g_biquad_initialized = true;
        ESP_LOGI(TAG, "DSP Pipeline initialized with 80Hz High-Pass Filter");
    } else {
        ESP_LOGE(TAG, "Failed to initialize ESP-DSP Biquad Filter");
    }
}

agc_config_t* dsp_pipeline_get_agc_config(void) {
    return &g_agc_cfg;
}

void dsp_pipeline_process(int16_t* buffer, size_t num_samples) {
    if (!buffer || num_samples == 0) return;
    
    // 1. High-Pass Filter (DC-Blocker) using ESP-DSP
    if (g_biquad_initialized) {
        // Reallocate internal f32 buffer if needed
        if (num_samples > g_f32_buffer_size) {
            if (g_f32_buffer) free(g_f32_buffer);
            g_f32_buffer = (float*)malloc(num_samples * sizeof(float));
            g_f32_buffer_size = num_samples;
        }
        
        if (g_f32_buffer) {
            // Convert to f32
            for (size_t i = 0; i < num_samples; i++) {
                g_f32_buffer[i] = (float)buffer[i];
            }
            
            // Apply Biquad Filter
            dsps_biquad_f32_ansi(g_f32_buffer, g_f32_buffer, num_samples, g_biquad_coeffs, g_biquad_w);
            
            // Convert back to int16
            for (size_t i = 0; i < num_samples; i++) {
                buffer[i] = (int16_t)g_f32_buffer[i];
            }
        }
    }
    
    // 2. AGC Logic
    if (g_agc_cfg.enabled) {
        double sum_sq = 0.0;
        for (size_t i = 0; i < num_samples; i++) {
            float val = (float)buffer[i];
            sum_sq += (val * val);
        }
        float frame_rms = sqrtf((float)(sum_sq / (double)num_samples));
        
        float smoothing_factor = (frame_rms > g_agc_cfg.smoothed_rms) ? 0.2f : 0.01f;
        g_agc_cfg.smoothed_rms = (g_agc_cfg.smoothed_rms * (1.0f - smoothing_factor)) + (frame_rms * smoothing_factor);
        
        float target_gain = g_agc_cfg.current_gain;
        
        if (g_agc_cfg.smoothed_rms > g_agc_cfg.noise_gate_rms) {
            target_gain = g_agc_cfg.target_rms / g_agc_cfg.smoothed_rms;
        } else {
            target_gain = g_agc_cfg.min_gain; 
        }
        
        if (target_gain > g_agc_cfg.max_gain) target_gain = g_agc_cfg.max_gain;
        if (target_gain < g_agc_cfg.min_gain) target_gain = g_agc_cfg.min_gain;
        
        if (target_gain < g_agc_cfg.current_gain) {
            g_agc_cfg.current_gain = (g_agc_cfg.current_gain * (1.0f - g_agc_cfg.attack_speed)) + (target_gain * g_agc_cfg.attack_speed);
        } else {
            g_agc_cfg.current_gain = (g_agc_cfg.current_gain * (1.0f - g_agc_cfg.decay_speed)) + (target_gain * g_agc_cfg.decay_speed);
        }
        
        for (size_t i = 0; i < num_samples; i++) {
            float sample = (float)buffer[i] * g_agc_cfg.current_gain;
            
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            
            buffer[i] = (int16_t)sample;
        }
    }
}
