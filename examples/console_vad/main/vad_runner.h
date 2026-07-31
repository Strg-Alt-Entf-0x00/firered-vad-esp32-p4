#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if a model is currently loaded
 */
bool vad_runner_is_model_loaded(void);

/**
 * @brief Check if the model is causal
 */
bool vad_runner_is_causal(void);

/**
 * @brief Load a model from the SPIFFS partition
 * 
 * @param filename Name of the .frvd file
 * @return 0 on success, non-zero on error
 */
int vad_runner_load_model(const char* filename);

/**
 * @brief Free the currently loaded model and its memory
 */
void vad_runner_free_model(void);

/**
 * @brief Print detailed information about the loaded model
 */
void vad_runner_print_info(void);

/**
 * @brief Check if the loaded model is a Stream-VAD (causal) model
 */
bool vad_runner_is_stream_model(void);

/**
 * @brief Reset the model state (clear history buffers)
 */
void vad_runner_reset(void);

/**
 * @brief Calibrate background noise level (Pre-VAD)
 * 
 * @param seconds Number of seconds to collect noise samples (e.g. 1 or 2)
 */
void vad_runner_calibrate_noise(int seconds);

/**
 * @brief Calibrate background noise level (Pre-VAD) blocking synchronously
 * 
 * @param seconds Number of seconds to collect noise samples (e.g. 1 or 2)
 */
void vad_runner_calibrate_noise_blocking(int seconds);

/**
 * @brief Set the sensitivity for the Energy Pre-VAD
 * 
 * @param multiplier Energy multiplier above baseline to trigger NN (e.g. 1.5f). 
 *                   Set to 0.0f to disable Pre-VAD.
 */
void vad_runner_set_pre_vad_threshold(float multiplier);

/**
 * @brief Extract features from raw PCM samples
 * 
 * @param pcm_samples Array of 160 samples (10ms @ 16kHz)
 * @param features Output array for 80 log-mel features
 * @param out_energy Optional pointer to store RMS energy of the frame
 */
void vad_runner_extract_features(int16_t* pcm_samples, float* features, float* out_energy);

/**
 * @brief Run inference on a single frame (Stream-VAD)
 * This function also records performance metrics (latency/cycles).
 * 
 * @param pcm_frame Array of 160 samples (10ms @ 16kHz)
 * @return Speech probability (0.0 to 1.0)
 */
float vad_runner_infer_frame(int16_t* pcm_frame);

/**
 * @brief Run batch inference on a chunk (Offline VAD)
 * 
 * @param chunk_features Array of features [frames * 80]
 * @param frames Number of frames in the chunk
 * @param chunk_probs Output array for probabilities
 */
void vad_runner_infer_chunk(float* chunk_features, size_t frames, float* chunk_probs);

#ifdef __cplusplus
}
#endif
