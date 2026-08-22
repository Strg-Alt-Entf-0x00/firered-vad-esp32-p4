#pragma once

#include "esp_firevad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdbool.h>

// Dual-Core VAD Configuration
#define DUAL_CORE_ENABLED          1
#define NUM_FRAME_SLOTS            8  // Increased from 4 to handle slower FP32/INT16
#define FRAME_SIZE_SAMPLES         160
#define FEATURES_DIM               80
#define OUTPUT_DIM                 8

// Frame slot structure for ring buffer
typedef struct {
    float features[FEATURES_DIM];
    float probs[OUTPUT_DIM];
    volatile bool ready_for_compute;
    volatile bool compute_done;
    SemaphoreHandle_t mutex;
    // Metrics recorded in Core 1 during inference
    uint32_t inference_time_us;
    uint32_t inference_cycles;
} frame_slot_t;

// Dual-Core VAD State
typedef struct {
    // Models for Cascade
    EspFirevadModel* stream_model;
    EspFirevadModel* offline_model;
    bool is_cascade;
    
    // Ring buffer
    frame_slot_t frame_buffer[NUM_FRAME_SLOTS];
    volatile int write_idx;
    volatile int read_idx;
    
    // Tasks
    TaskHandle_t core0_task_handle;
    TaskHandle_t core1_task_handle;
    
    // Status
    volatile bool running;
    volatile uint32_t frames_extracted;
    volatile uint32_t frames_computed;
    
    // Metrics
    volatile uint32_t cascade_wakeups;
    volatile uint32_t cascade_fast_paths;
    
} dual_core_vad_t;

// Initialize dual-core VAD system (supports single or cascade)
esp_err_t dual_core_vad_init(dual_core_vad_t* vad, EspFirevadModel* stream_model, EspFirevadModel* offline_model);

// Start dual-core processing
esp_err_t dual_core_vad_start(dual_core_vad_t* vad);

// Stop dual-core processing
esp_err_t dual_core_vad_stop(dual_core_vad_t* vad);

// Submit audio frame for processing (called from audio callback)
esp_err_t dual_core_vad_submit_frame(dual_core_vad_t* vad, const int16_t* pcm_160, float* features_out);

// Get latest VAD result (non-blocking)
bool dual_core_vad_get_result(dual_core_vad_t* vad, float* probs_out, uint32_t* time_us = nullptr, uint32_t* cycles = nullptr);

// Cleanup dual-core VAD
void dual_core_vad_deinit(dual_core_vad_t* vad);

// Get statistics
void dual_core_vad_get_stats(dual_core_vad_t* vad, uint32_t* extracted, uint32_t* computed, int* backlog);

// Get cascade metrics
void dual_core_vad_get_cascade_stats(dual_core_vad_t* vad, uint32_t* wakeups, uint32_t* fast_paths);

// Get last inference time (from most recently completed frame)
bool dual_core_vad_get_last_inference_time(dual_core_vad_t* vad, uint32_t* time_us, uint32_t* cycles);

