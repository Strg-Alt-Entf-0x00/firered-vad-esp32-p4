/**
 * @file audio_hal_i2s.c
 * @brief I2S Audio HAL for TTS output via ES8311 codec on ESP32-P4
 *
 * Pin assignment (Waveshare ESP32-P4 with ES8311 codec):
 *   MCLK  -> GPIO 13
 *   BCLK  -> GPIO 12
 *   WS    -> GPIO 10
 *   DOUT  -> GPIO 9
 *   DIN   -> GPIO 11
 *   PA EN -> GPIO 53
 */
#include "audio_hal_i2s.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "tts_audio_hal";
static i2s_chan_handle_t s_tx_channel = NULL;
static i2s_chan_handle_t s_rx_channel = NULL;
static bool s_initialized = false;

/* PA Enable pin: powers the speaker amplifier.
 * Board-specific: GPIO53 on Waveshare ESP32-P4 boards with ES8311. */
#define AUDIO_HAL_PA_GPIO   GPIO_NUM_53

/* MCLK multiple: 384 for ES8311 (required for correct PLL lock). */
#define AUDIO_HAL_MCLK_MULTIPLE  I2S_MCLK_MULTIPLE_384

bool audio_hal_i2s_init(uint32_t sample_rate, int i2s_port,
                        int mclk_pin, int bck_pin, int ws_pin,
                        int data_out_pin, int data_in_pin)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Audio HAL already initialized");
        return true;
    }

    ESP_LOGI(TAG, "Initializing I2S for TTS output (rate: %lu Hz)", sample_rate);

    /* Enable power amplifier if PA pin is valid */
    if (AUDIO_HAL_PA_GPIO != GPIO_NUM_NC) {
        gpio_config_t pa_cfg = {
            .pin_bit_mask = (1ULL << AUDIO_HAL_PA_GPIO),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&pa_cfg);
        gpio_set_level(AUDIO_HAL_PA_GPIO, 1);
        ESP_LOGI(TAG, "Power amplifier enabled (GPIO %d)", AUDIO_HAL_PA_GPIO);
    }

    /* Allocate channels based on configured pins */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* Clear DMA buffer on underrun */

    i2s_chan_handle_t *rx_ptr = (data_in_pin >= 0) ? &s_rx_channel : NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_channel, rx_ptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S channel(s): %s", esp_err_to_name(err));
        return false;
    }

    /* Standard I2S (Philips), 16-bit stereo.
     * ES8311 requires STEREO slots even for mono playback. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)mclk_pin,
            .bclk = (gpio_num_t)bck_pin,
            .ws   = (gpio_num_t)ws_pin,
            .dout = (gpio_num_t)data_out_pin,
            .din  = (data_in_pin >= 0) ? (gpio_num_t)data_in_pin : I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* ES8311 requires MCLK = 384 * Fs for proper PLL lock */
    std_cfg.clk_cfg.mclk_multiple = AUDIO_HAL_MCLK_MULTIPLE;

    err = i2s_channel_init_std_mode(s_tx_channel, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S STD mode: %s", esp_err_to_name(err));
        goto err_cleanup;
    }

    if (s_rx_channel) {
        err = i2s_channel_init_std_mode(s_rx_channel, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init I2S RX STD mode: %s", esp_err_to_name(err));
            goto err_cleanup;
        }
    }

    err = i2s_channel_enable(s_tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX channel: %s", esp_err_to_name(err));
        goto err_cleanup;
    }

    if (s_rx_channel) {
        err = i2s_channel_enable(s_rx_channel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S RX channel: %s", esp_err_to_name(err));
            goto err_cleanup;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "I2S Audio HAL initialized (MCLK=GPIO%d, BCK=GPIO%d, WS=GPIO%d, DOUT=GPIO%d, DIN=GPIO%d)",
             mclk_pin, bck_pin, ws_pin, data_out_pin, data_in_pin);
    return true;

err_cleanup:
    if (s_tx_channel) i2s_del_channel(s_tx_channel);
    if (s_rx_channel) i2s_del_channel(s_rx_channel);
    s_tx_channel = NULL;
    s_rx_channel = NULL;
    return false;
}

void audio_hal_i2s_deinit(void)
{
    if (!s_initialized) return;

    if (s_tx_channel) {
        i2s_channel_disable(s_tx_channel);
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    if (s_rx_channel) {
        i2s_channel_disable(s_rx_channel);
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }

    /* Disable power amplifier */
    if (AUDIO_HAL_PA_GPIO != GPIO_NUM_NC) {
        gpio_set_level(AUDIO_HAL_PA_GPIO, 0);
    }

    s_initialized = false;
    ESP_LOGI(TAG, "I2S Audio HAL deinitialized");
}

size_t audio_hal_i2s_write(const void *data, size_t size_bytes)
{
    if (!s_initialized || !s_tx_channel) return 0;

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_tx_channel, data, size_bytes,
                                      &bytes_written, portMAX_DELAY);
    
    // If the channel was disabled (e.g. by esp_codec_dev state mismatch), enable it and retry
    if (err == ESP_ERR_INVALID_STATE) {
        i2s_channel_enable(s_tx_channel);
        err = i2s_channel_write(s_tx_channel, data, size_bytes,
                                &bytes_written, portMAX_DELAY);
    }
    
    if (err != ESP_OK) {
        // Only log if it's still failing to avoid spamming the watchdog
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
    }
    return bytes_written;
}

void *audio_hal_i2s_get_tx_handle(void)
{
    return (void *)s_tx_channel;
}

void *audio_hal_i2s_get_rx_handle(void)
{
    return (void *)s_rx_channel;
}
