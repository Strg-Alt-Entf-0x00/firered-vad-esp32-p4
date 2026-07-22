/**
 * @file audio_hal_i2s.h
 * @brief I2S Audio HAL for TTS output via ES8311 codec on ESP32-P4
 *
 * Default pin assignment for Waveshare ESP32-P4 boards:
 *   MCLK  -> GPIO 13
 *   BCLK  -> GPIO 12
 *   WS    -> GPIO 10
 *   DOUT  -> GPIO 9
 *   DIN   -> GPIO 11 (-1 = unused for TX-only)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default I2S pin mapping for Waveshare ESP32-P4 with ES8311 codec */
#define AUDIO_HAL_I2S_PORT_DEFAULT  0
#define AUDIO_HAL_MCLK_PIN_DEFAULT  13
#define AUDIO_HAL_BCK_PIN_DEFAULT   12
#define AUDIO_HAL_WS_PIN_DEFAULT    10
#define AUDIO_HAL_DOUT_PIN_DEFAULT  9
#define AUDIO_HAL_DIN_PIN_DEFAULT   -1   /* Not needed for TTS output-only */

/**
 * Initialize I2S peripheral and power amplifier for TTS audio output.
 *
 * @param sample_rate   Target sample rate in Hz (e.g. 22050, 16000)
 * @param i2s_port      I2S port number (0)
 * @param mclk_pin      MCLK GPIO (required by ES8311 for PLL lock)
 * @param bck_pin       Bit clock GPIO
 * @param ws_pin        Word select GPIO
 * @param data_out_pin  Data out (TX) GPIO
 * @param data_in_pin   Data in (RX) GPIO, or -1 if unused
 * @return true on success, false on hardware failure
 */
bool audio_hal_i2s_init(uint32_t sample_rate, int i2s_port,
                        int mclk_pin, int bck_pin, int ws_pin,
                        int data_out_pin, int data_in_pin);

/**
 * Deinitialize I2S and disable power amplifier.
 */
void audio_hal_i2s_deinit(void);

/**
 * Write PCM audio samples (16-bit stereo interleaved) to the I2S output.
 *
 * @param data       Pointer to PCM data buffer
 * @param size_bytes Number of bytes to write
 * @return Number of bytes actually written
 */
size_t audio_hal_i2s_write(const void *data, size_t size_bytes);

/**
 * @brief Get the I2S TX channel handle
 * @return void* Handle (cast to i2s_chan_handle_t) or NULL if not initialized
 */
void *audio_hal_i2s_get_tx_handle(void);

/**
 * @brief Get the I2S RX channel handle
 * @return void* Handle (cast to i2s_chan_handle_t) or NULL if not initialized
 */
void *audio_hal_i2s_get_rx_handle(void);

#ifdef __cplusplus
}
#endif
