#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max file path length for SPIFFS
#define MAX_FILE_PATH 300

/**
 * @brief Check if the audio subsystem is initialized
 */
bool audio_manager_is_initialized(void);

/**
 * @brief Initialize the audio I2C, I2S, and ES8311 Codec
 */
esp_err_t audio_manager_init(void);

/**
 * @brief Deinitialize the audio subsystem
 */


/**
 * @brief Set the microphone input gain (hardware ADC gain in dB)
 * 
 * @param gain_db 0.0 to 42.0 dB
 */
esp_err_t audio_manager_set_mic_gain(float gain_db);

/**
 * @brief Get current microphone hardware gain in dB
 */
esp_err_t audio_manager_get_mic_gain(float* gain_db);

/**
 * @brief Set the speaker output volume (hardware DAC gain)
 * 
 * @param vol_level 0-100 (percentage)
 */
esp_err_t audio_manager_set_speaker_vol(uint8_t vol_level);

typedef enum {
    MIC_I2S1_INMP441,
    MIC_I2S0_ES8311
} mic_type_t;

/**
 * @brief Set the active microphone input path
 */
esp_err_t audio_manager_set_mic(mic_type_t mic);

/**
 * @brief Get the active microphone input path
 */
mic_type_t audio_manager_get_mic(void);

/**
 * @brief Start continuous audio capture (I2S RX)
 */
esp_err_t audio_manager_start_capture(void);

/**
 * @brief Stop continuous audio capture
 */
esp_err_t audio_manager_stop_capture(void);

/**
 * @brief Get diagnostics levels from 1 second of audio
 */
esp_err_t audio_manager_get_levels(float *rms, float *peak, int *clipping_count);

/**
 * @brief Read audio samples from the active I2S RX channel
 * 
 * @param buffer Output buffer
 * @param samples Number of samples to read
 * @param timeout_ms Timeout in milliseconds
 * @return Number of samples actually read
 */
int audio_manager_read(int16_t* buffer, size_t samples, uint32_t timeout_ms);

/**
 * @brief Start continuous audio playback (I2S TX)
 */
esp_err_t audio_manager_start_playback(void);

/**
 * @brief Stop continuous audio playback
 */
esp_err_t audio_manager_stop_playback(void);

/**
 * @brief Write audio samples to the I2S TX channel
 * 
 * @param buffer Input buffer
 * @param samples Number of samples to write
 * @param timeout_ms Timeout in milliseconds
 * @return Number of samples actually written
 */
int audio_manager_write(const int16_t* buffer, size_t samples, uint32_t timeout_ms);

// ---------------------------------------------------------
// Sound Effects
// ---------------------------------------------------------

void audio_manager_play_boot_sequence(void);

#ifdef __cplusplus
}
#endif
