#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the internal FFT and Mel filterbank state.
 *        Must be called once before extracting features.
 * 
 * @return esp_err_t ESP_OK on success, error code otherwise.
 */
esp_err_t esp_firevad_dsp_init(void);

/**
 * @brief Extract 80-dimensional log-mel filterbank features from a 160-sample PCM frame.
 * 
 * @param pcm_160 Input array of 160 raw audio samples (16kHz, 16-bit).
 * @param features_80 Output array of 80 log-mel features.
 * @param out_energy Optional pointer to store the RMS energy of the frame.
 */
void esp_firevad_dsp_extract_features(const int16_t* pcm_160, float* features_80, float* out_energy);
/**
 * Reset DSP internal state (window buffers, history)
 */
void esp_firevad_dsp_reset(void);

#ifdef __cplusplus
}
#endif
