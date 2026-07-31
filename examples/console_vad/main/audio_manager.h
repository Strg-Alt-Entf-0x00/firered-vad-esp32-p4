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
esp_err_t audio_manager_deinit(void);

/**
 * @brief Set the microphone input gain (hardware ADC gain)
 * 
 * @param gain_level 0-11 mapping to 0dB-24dB
 */
esp_err_t audio_manager_set_mic_gain(uint8_t gain_level);

/**
 * @brief Get current microphone hardware gain
 */
esp_err_t audio_manager_get_mic_gain(uint8_t* gain_level);

/**
 * @brief Set the speaker output volume (hardware DAC gain)
 * 
 * @param vol_level 0-100 (percentage)
 */
esp_err_t audio_manager_set_speaker_vol(uint8_t vol_level);

/**
 * @brief Start continuous audio capture (I2S RX)
 */
esp_err_t audio_manager_start_capture(void);

/**
 * @brief Stop continuous audio capture
 */
esp_err_t audio_manager_stop_capture(void);

/**
 * @brief Read audio samples from the I2S RX channel
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
