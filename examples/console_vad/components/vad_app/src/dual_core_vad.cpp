#include "dual_core_vad.h"
#include "esp_firevad_dsp.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include <string.h>

static const char* TAG = "DUAL_CORE_VAD";

// Core 1 Task: Inference Engine
static void IRAM_ATTR core1_inference_task(void* arg) {
    dual_core_vad_t* vad = (dual_core_vad_t*)arg;
    
    ESP_LOGI(TAG, "Core 1 Inference Task started on CPU %d", xPortGetCoreID());
    
    int frame_count = 0;
    while (vad->running) {
        // Wait for notification from Core 0
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        if (!vad->running) break;
        
        // Prevent IDLE1 starvation and TWDT panics during heavy WAV benchmarking
        // by yielding 1 tick to Priority 0 every 50 frames (~1.3s for FP32).
        if (++frame_count % 50 == 0) {
            vTaskDelay(1);
        }
        
        // Get next frame to process
        int slot = vad->read_idx % NUM_FRAME_SLOTS;
        frame_slot_t* frame = &vad->frame_buffer[slot];
        
        // Take mutex
        if (xSemaphoreTake(frame->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (frame->ready_for_compute && !frame->compute_done) {
                // SESSION 17: Professional measurement - measure CMVN + Inference (like baseline)
                // This ensures apples-to-apples comparison with baseline measurement
                
                // Start timer BEFORE CMVN (to match baseline measurement)
                uint32_t start_cycles = esp_cpu_get_cycle_count();
                uint64_t start_time = esp_timer_get_time();
                
                // Step 1: Apply CMVN normalization
                float prepared_features[FEATURES_DIM];
                
                EspFirevadModel* target_model = vad->is_cascade ? vad->offline_model : vad->stream_model;
                esp_firevad_prepare_features(target_model, frame->features, true, prepared_features);
                
                // Step 2: Run inference on prepared features
                esp_firevad_infer_prepared(target_model, prepared_features, frame->probs);
                
                // Stop timer AFTER both CMVN and Inference
                uint64_t end_time = esp_timer_get_time();
                uint32_t end_cycles = esp_cpu_get_cycle_count();
                
                // Store metrics in the frame
                frame->inference_time_us = (uint32_t)(end_time - start_time);
                frame->inference_cycles = end_cycles - start_cycles;
                
                // Mark as complete
                frame->compute_done = true;
                frame->ready_for_compute = false;
                
                // Atomic increment
                uint32_t old_count = vad->frames_computed;
                vad->frames_computed = old_count + 1;
            }
            xSemaphoreGive(frame->mutex);
        }
        
        // Atomic increment
        int old_idx = vad->read_idx;
        vad->read_idx = old_idx + 1;
    }
    
    ESP_LOGI(TAG, "Core 1 Inference Task stopped");
    vTaskDelete(NULL);
}

// Initialize dual-core VAD system (supports single or cascade)
esp_err_t dual_core_vad_init(dual_core_vad_t* vad, EspFirevadModel* stream_model, EspFirevadModel* offline_model) {
    if (!vad || !stream_model) return ESP_ERR_INVALID_ARG;
    
    memset(vad, 0, sizeof(dual_core_vad_t));
    vad->stream_model = stream_model;
    vad->offline_model = offline_model;
    vad->is_cascade = (offline_model != nullptr);
    
    // Initialize frame buffer and mutexes
    for (int i = 0; i < NUM_FRAME_SLOTS; i++) {
        vad->frame_buffer[i].mutex = xSemaphoreCreateMutex();
        if (!vad->frame_buffer[i].mutex) {
            ESP_LOGE(TAG, "Failed to create mutex %d", i);
            // Cleanup already created mutexes
            for (int j = 0; j < i; j++) {
                vSemaphoreDelete(vad->frame_buffer[j].mutex);
            }
            return ESP_ERR_NO_MEM;
        }
        vad->frame_buffer[i].ready_for_compute = false;
        vad->frame_buffer[i].compute_done = false;
    }
    
    ESP_LOGI(TAG, "Dual-Core VAD initialized with %d frame slots", NUM_FRAME_SLOTS);
    return ESP_OK;
}

// Start dual-core processing
esp_err_t dual_core_vad_start(dual_core_vad_t* vad) {
    if (!vad) return ESP_ERR_INVALID_ARG;
    
    vad->running = true;
    vad->write_idx = 0;
    vad->read_idx = 0;
    vad->frames_extracted = 0;
    vad->frames_computed = 0;
    
    // Create Core 1 task (inference)
    BaseType_t ret = xTaskCreatePinnedToCore(
        core1_inference_task,
        "vad_infer",
        8192,                   // Stack size
        vad,                    // Task parameter
        5,                      // Priority
        &vad->core1_task_handle,
        1                       // Pin to Core 1
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Core 1 task");
        vad->running = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Dual-Core VAD started - Feature extraction on Core 0, Inference on Core 1");
    return ESP_OK;
}

// Stop dual-core processing
esp_err_t dual_core_vad_stop(dual_core_vad_t* vad) {
    if (!vad) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Stopping Dual-Core VAD...");
    vad->running = false;
    
    // Notify Core 1 task to exit
    if (vad->core1_task_handle) {
        xTaskNotifyGive(vad->core1_task_handle);
        vTaskDelay(pdMS_TO_TICKS(100)); // Give time to exit
        vad->core1_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "Dual-Core VAD stopped. Frames: extracted=%u, computed=%u",
             (unsigned)vad->frames_extracted, (unsigned)vad->frames_computed);
    
    return ESP_OK;
}

// Submit audio frame for processing (Core 0 - called from main thread)
esp_err_t dual_core_vad_submit_frame(dual_core_vad_t* vad, const int16_t* pcm_160, float* features_out) {
    if (!vad || !pcm_160) return ESP_ERR_INVALID_ARG;
    
    // Step 1: Extract features
    float raw_features[FEATURES_DIM];
    float energy = 0.0f;
    esp_firevad_dsp_extract_features(pcm_160, raw_features, &energy);
    
    if (features_out) {
        memcpy(features_out, raw_features, FEATURES_DIM * sizeof(float));
    }
    
    bool needs_heavy_inference = true;
    
    // Step 2: Gatekeeper (Cascade Mode)
    float stream_probs_saved[OUTPUT_DIM] = {0};
    if (vad->is_cascade && vad->stream_model) {
        float prepared_features[FEATURES_DIM];
        
        esp_firevad_prepare_features(vad->stream_model, raw_features, true, prepared_features);
        esp_firevad_infer_prepared(vad->stream_model, prepared_features, stream_probs_saved);
        
        // Confidence check (assuming index 1 is speech)
        float speech_prob = stream_probs_saved[1];
        if (speech_prob < 0.2f || speech_prob > 0.8f) {
            // High confidence - skip heavy model!
            needs_heavy_inference = false;
            vad->cascade_fast_paths = vad->cascade_fast_paths + 1;
        } else {
            vad->cascade_wakeups = vad->cascade_wakeups + 1;
        }
    }
    
    // Get next slot
    int slot = vad->write_idx % NUM_FRAME_SLOTS;
    frame_slot_t* frame = &vad->frame_buffer[slot];
    
    if (xSemaphoreTake(frame->mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    memcpy(frame->features, raw_features, FEATURES_DIM * sizeof(float));
    
    if (!needs_heavy_inference) {
        // Fast path: bypass Core 1, result is already known
        memcpy(frame->probs, stream_probs_saved, OUTPUT_DIM * sizeof(float));
        frame->ready_for_compute = false;
        frame->compute_done = true;
        frame->inference_time_us = 0; // Or measure the 6ms time if needed
        frame->inference_cycles = 0;
        vad->frames_computed = vad->frames_computed + 1;
        vad->read_idx = vad->read_idx + 1; // Advance reader since Core 1 won't do it
    } else {
        // Mark as ready for compute
        frame->ready_for_compute = true;
        frame->compute_done = false;
    }
    xSemaphoreGive(frame->mutex);
    
    vad->write_idx = vad->write_idx + 1;
    vad->frames_extracted = vad->frames_extracted + 1;
    
    // Wake up Core 1
    if (vad->core1_task_handle && needs_heavy_inference) {
        xTaskNotifyGive(vad->core1_task_handle);
    }
    
    return ESP_OK;
}

// Get latest VAD result (non-blocking) with optional metrics
bool dual_core_vad_get_result(dual_core_vad_t* vad, float* probs_out, uint32_t* time_us, uint32_t* cycles) {
    if (!vad || !probs_out) return false;
    
    // Search backwards from most recent frame
    for (int i = 0; i < NUM_FRAME_SLOTS; i++) {
        int slot = (vad->write_idx - 1 - i + NUM_FRAME_SLOTS) % NUM_FRAME_SLOTS;
        frame_slot_t* frame = &vad->frame_buffer[slot];
        
        // Use small timeout to wait for mutex (handles FP32/INT16 long inference times)
        if (xSemaphoreTake(frame->mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (frame->compute_done) {
                memcpy(probs_out, frame->probs, OUTPUT_DIM * sizeof(float));
                // Also return metrics if requested
                if (time_us) {
                    *time_us = frame->inference_time_us;
                    // DEBUG: Log first few results
                    static int debug_count = 0;
                    if (debug_count++ < 3) {
                        ESP_LOGI("DUAL_CORE_VAD", "DEBUG: Got result with time_us=%u", frame->inference_time_us);
                    }
                }
                if (cycles) *cycles = frame->inference_cycles;
                xSemaphoreGive(frame->mutex);
                return true;
            }
            xSemaphoreGive(frame->mutex);
        }
    }
    
    return false; // No completed frame available yet
}

// Cleanup dual-core VAD
void dual_core_vad_deinit(dual_core_vad_t* vad) {
    if (!vad) return;
    
    // Stop if still running
    if (vad->running) {
        dual_core_vad_stop(vad);
    }
    
    // Delete mutexes
    for (int i = 0; i < NUM_FRAME_SLOTS; i++) {
        if (vad->frame_buffer[i].mutex) {
            vSemaphoreDelete(vad->frame_buffer[i].mutex);
            vad->frame_buffer[i].mutex = NULL;
        }
    }
    
    ESP_LOGI(TAG, "Dual-Core VAD deinitialized");
}

// Get statistics
void dual_core_vad_get_stats(dual_core_vad_t* vad, uint32_t* extracted, uint32_t* computed, int* backlog) {
    if (!vad) return;
    
    if (extracted) *extracted = vad->frames_extracted;
    if (computed) *computed = vad->frames_computed;
    if (backlog) *backlog = (int)vad->frames_extracted - (int)vad->frames_computed;
}

// Get cascade metrics
void dual_core_vad_get_cascade_stats(dual_core_vad_t* vad, uint32_t* wakeups, uint32_t* fast_paths) {
    if (!vad) return;
    if (wakeups) *wakeups = vad->cascade_wakeups;
    if (fast_paths) *fast_paths = vad->cascade_fast_paths;
}

// Get last inference time from most recently completed frame
bool dual_core_vad_get_last_inference_time(dual_core_vad_t* vad, uint32_t* time_us, uint32_t* cycles) {
    if (!vad || !time_us) return false;
    
    // Search backwards from most recent frame to find a completed one
    for (int i = 0; i < NUM_FRAME_SLOTS; i++) {
        int slot = (vad->write_idx - 1 - i + NUM_FRAME_SLOTS) % NUM_FRAME_SLOTS;
        frame_slot_t* frame = &vad->frame_buffer[slot];
        
        if (xSemaphoreTake(frame->mutex, 0) == pdTRUE) {
            if (frame->compute_done) {
                *time_us = frame->inference_time_us;
                if (cycles) *cycles = frame->inference_cycles;
                xSemaphoreGive(frame->mutex);
                return true;
            }
            xSemaphoreGive(frame->mutex);
        }
    }
    
    return false; // No completed frame with metrics available
}

