#include "audio_manager.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char* TAG = "AUDIO_MANAGER";

static bool g_initialized = false;
static bool g_capture_running = false;
static bool g_playback_running = false;
static uint8_t g_current_mic_gain = CONFIG_FIREVAD_MIC_GAIN;

// I2C Master Bus Handle
static i2c_master_bus_handle_t g_i2c_bus_handle = NULL;

// I2S Handles
static i2s_chan_handle_t g_i2s_rx_handle = NULL;
static i2s_chan_handle_t g_i2s_tx_handle = NULL;

// Codec Interfaces
static const audio_codec_ctrl_if_t *g_codec_ctrl_if = NULL;
static const audio_codec_data_if_t *g_codec_data_if = NULL;
static const audio_codec_gpio_if_t *g_codec_gpio_if = NULL;
static const audio_codec_if_t *g_codec_if = NULL;

static esp_err_t audio_i2c_init(void) {
    i2c_master_bus_config_t i2c_mst_config = {};
    i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_mst_config.i2c_port = I2C_NUM_0;
    i2c_mst_config.scl_io_num = (gpio_num_t)CONFIG_FIREVAD_I2C_SCL_GPIO;
    i2c_mst_config.sda_io_num = (gpio_num_t)CONFIG_FIREVAD_I2C_SDA_GPIO;
    i2c_mst_config.glitch_ignore_cnt = 7;
    i2c_mst_config.flags.enable_internal_pullup = true;
    
    ESP_LOGI(TAG, "Initializing I2C Master: SCL=%d, SDA=%d, Freq=400000Hz", 
             CONFIG_FIREVAD_I2C_SCL_GPIO, CONFIG_FIREVAD_I2C_SDA_GPIO);
             
    return i2c_new_master_bus(&i2c_mst_config, &g_i2c_bus_handle);
}

static esp_err_t audio_i2s_init(void) {
    ESP_LOGI(TAG, "Initializing I2S STD: MCLK=%d, BCLK=%d, WS=%d, DIN=%d, DOUT=%d",
             CONFIG_FIREVAD_I2S_MCLK_GPIO, CONFIG_FIREVAD_I2S_BCLK_GPIO, 
             CONFIG_FIREVAD_I2S_WS_GPIO, CONFIG_FIREVAD_I2S_DIN_GPIO, CONFIG_FIREVAD_I2S_DOUT_GPIO);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    // Allocate BOTH TX and RX channels
    esp_err_t ret = i2s_new_channel(&chan_cfg, &g_i2s_tx_handle, &g_i2s_rx_handle);
    if (ret != ESP_OK) return ret;
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_FIREVAD_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)CONFIG_FIREVAD_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)CONFIG_FIREVAD_I2S_MCLK_GPIO,
            .bclk = (gpio_num_t)CONFIG_FIREVAD_I2S_BCLK_GPIO,
            .ws   = (gpio_num_t)CONFIG_FIREVAD_I2S_WS_GPIO,
            .dout = (gpio_num_t)CONFIG_FIREVAD_I2S_DOUT_GPIO,
            .din  = (gpio_num_t)CONFIG_FIREVAD_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL; // High quality clock, required for stable MCLK
    
    // Initialize RX channel
    ret = i2s_channel_init_std_mode(g_i2s_rx_handle, &std_cfg);
    if (ret != ESP_OK) return ret;
    
    // Initialize TX channel
    ret = i2s_channel_init_std_mode(g_i2s_tx_handle, &std_cfg);
    return ret;
}

static esp_err_t audio_codec_init(void) {
    ESP_LOGI(TAG, "Initializing ES8311 codec");
    
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_0;
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = g_i2c_bus_handle;
    
    g_codec_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!g_codec_ctrl_if) return ESP_FAIL;
    
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = g_i2s_rx_handle;
    i2s_cfg.tx_handle = g_i2s_tx_handle;
    
    g_codec_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!g_codec_data_if) return ESP_FAIL;
    
    if (CONFIG_FIREVAD_PA_PIN_GPIO >= 0) {
        g_codec_gpio_if = audio_codec_new_gpio();
    }
    
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = g_codec_ctrl_if;
    es8311_cfg.gpio_if = g_codec_gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = CONFIG_FIREVAD_PA_PIN_GPIO;
    es8311_cfg.pa_reverted = false;
    es8311_cfg.master_mode = false;
    es8311_cfg.use_mclk = true;
    es8311_cfg.digital_mic = false;
    es8311_cfg.invert_mclk = false;
    es8311_cfg.invert_sclk = false;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    
    g_codec_if = es8311_codec_new(&es8311_cfg);
    if (!g_codec_if) return ESP_FAIL;
    
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = CONFIG_FIREVAD_BITS_PER_SAMPLE;
    fs.channel = 2; // Match I2S Stereo mode
    fs.sample_rate = CONFIG_FIREVAD_SAMPLE_RATE;
    
    // PERMANENTLY enable I2S channels to keep the master clock running at all times!
    // This prevents the 4-second DMA stall on the ESP32-P4.
    i2s_channel_enable(g_i2s_rx_handle);
    i2s_channel_enable(g_i2s_tx_handle);
    
    esp_err_t ret = g_codec_if->set_fs(g_codec_if, &fs);
    
    // Set microphone gain
    float gain_db = g_current_mic_gain * 2.0f;
    g_codec_if->set_mic_gain(g_codec_if, gain_db);
    
    // Set speaker volume to default 50%
    audio_manager_set_speaker_vol(50);
    
    if (ret != ESP_OK) return ret;
    
    ESP_LOGI(TAG, "ES8311 codec initialized successfully (BOTH mode)");
    return ESP_OK;
}

bool audio_manager_is_initialized(void) {
    return g_initialized;
}

esp_err_t audio_manager_init(void) {
    if (g_initialized) return ESP_OK;
    
    esp_err_t ret = audio_i2c_init();
    if (ret != ESP_OK) return ret;
    
    ret = audio_i2s_init();
    if (ret != ESP_OK) return ret;
    
    ret = audio_codec_init();
    if (ret != ESP_OK) return ret;
    
    g_initialized = true;
    ESP_LOGI(TAG, "=== Audio Manager Initialized Successfully ===");
    return ESP_OK;
}

esp_err_t audio_manager_deinit(void) {
    if (!g_initialized) return ESP_OK;
    
    if (g_codec_if) {
        g_codec_if->close(g_codec_if);
        g_codec_if = NULL;
    }
    if (g_codec_gpio_if) {
        audio_codec_delete_gpio_if(g_codec_gpio_if);
        g_codec_gpio_if = NULL;
    }
    if (g_codec_data_if) {
        audio_codec_delete_data_if(g_codec_data_if);
        g_codec_data_if = NULL;
    }
    if (g_codec_ctrl_if) {
        audio_codec_delete_ctrl_if(g_codec_ctrl_if);
        g_codec_ctrl_if = NULL;
    }
    if (g_i2s_rx_handle) {
        i2s_del_channel(g_i2s_rx_handle);
        g_i2s_rx_handle = NULL;
    }
    if (g_i2s_tx_handle) {
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
    }
    if (g_i2c_bus_handle) {
        i2c_del_master_bus(g_i2c_bus_handle);
        g_i2c_bus_handle = NULL;
    }
    
    g_initialized = false;
    ESP_LOGI(TAG, "Audio manager deinitialized");
    return ESP_OK;
}

esp_err_t audio_manager_set_mic_gain(uint8_t gain_level) {
    if (gain_level > 11) return ESP_ERR_INVALID_ARG;
    if (!g_codec_if) return ESP_FAIL;
    
    float gain_db = gain_level * 2.0f;
    esp_err_t ret = g_codec_if->set_mic_gain(g_codec_if, gain_db);
    if (ret == ESP_OK) {
        g_current_mic_gain = gain_level;
        ESP_LOGI(TAG, "Mic gain set to %ddB", (int)gain_db);
    }
    return ret;
}

esp_err_t audio_manager_get_mic_gain(uint8_t* gain_level) {
    if (!gain_level) return ESP_ERR_INVALID_ARG;
    *gain_level = g_current_mic_gain;
    return ESP_OK;
}

esp_err_t audio_manager_set_speaker_vol(uint8_t vol_level) {
    if (vol_level > 100) return ESP_ERR_INVALID_ARG;
    if (!g_codec_if) return ESP_FAIL;
    
    // Interpolate volume 0-100 to approx dB range (-95.5 to 0 or 32)
    // Simple mapping: 0 = -90dB, 100 = 0dB
    float db_val = (vol_level == 0) ? -90.0f : ((vol_level - 100.0f) * 0.5f); 
    if (db_val > 0.0f) db_val = 0.0f;
    
    esp_err_t ret = g_codec_if->set_vol(g_codec_if, db_val);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Speaker volume set to %d%% (%.1f dB)", vol_level, db_val);
    }
    return ret;
}

esp_err_t audio_manager_start_capture(void) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (g_capture_running) return ESP_OK;
    
    // Channels are already enabled permanently. Just enable the codec.
    esp_err_t ret = g_codec_if->enable(g_codec_if, true);
    if (ret != ESP_OK) return ret;
    
    g_capture_running = true;
    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

esp_err_t audio_manager_stop_capture(void) {
    if (!g_capture_running) return ESP_OK;
    
    // Don't disable codec here if playback is running
    if (!g_playback_running) {
        g_codec_if->enable(g_codec_if, false);
    }
    
    g_capture_running = false;
    ESP_LOGI(TAG, "Audio capture stopped");
    return ESP_OK;
}

int audio_manager_read(int16_t* buffer, size_t samples, uint32_t timeout_ms) {
    if (!g_capture_running) return 0;
    
    int16_t* stereo_buf = (int16_t*)malloc(samples * 2 * sizeof(int16_t));
    if (!stereo_buf) {
        ESP_LOGE(TAG, "Failed to allocate stereo read buffer");
        return -1;
    }
    
    size_t bytes_read = 0;
    size_t requested_bytes = samples * 2 * sizeof(int16_t);
    
    esp_err_t ret = i2s_channel_read(g_i2s_rx_handle, stereo_buf, requested_bytes, &bytes_read, pdMS_TO_TICKS(timeout_ms));
    
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(ret));
        free(stereo_buf);
        return -1;
    }
    
    int valid_stereo_samples = bytes_read / sizeof(int16_t);
    int valid_mono_samples = valid_stereo_samples / 2;
    
    // Extract left channel
    for (int i = 0; i < valid_mono_samples; i++) {
        buffer[i] = stereo_buf[i * 2];
    }
    
    free(stereo_buf);
    return valid_mono_samples;
}

esp_err_t audio_manager_start_playback(void) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (g_playback_running) return ESP_OK;
    
    // Channels are already enabled permanently. Just enable the codec.
    esp_err_t ret = g_codec_if->enable(g_codec_if, true);
    if (ret != ESP_OK) return ret;
    
    // Re-apply volume to ensure DAC is not left muted by capture stop
    audio_manager_set_speaker_vol(100);
    
    g_playback_running = true;
    ESP_LOGI(TAG, "Audio playback started");
    return ESP_OK;
}

esp_err_t audio_manager_stop_playback(void) {
    if (!g_playback_running) return ESP_OK;
    
    // Don't disable codec if capture is running
    if (!g_capture_running) {
        g_codec_if->enable(g_codec_if, false);
    }
    
    g_playback_running = false;
    ESP_LOGI(TAG, "Audio playback stopped");
    return ESP_OK;
}

int audio_manager_write(const int16_t* buffer, size_t samples, uint32_t timeout_ms) {
    if (!g_playback_running) return 0;
    
    int16_t* stereo_buf = (int16_t*)malloc(samples * 2 * sizeof(int16_t));
    if (!stereo_buf) {
        ESP_LOGE(TAG, "Failed to allocate stereo write buffer");
        return -1;
    }
    
    // Duplicate mono samples to left and right channels
    for (size_t i = 0; i < samples; i++) {
        stereo_buf[i * 2] = buffer[i];
        stereo_buf[i * 2 + 1] = buffer[i];
    }
    
    size_t bytes_written = 0;
    size_t requested_bytes = samples * 2 * sizeof(int16_t);
    
    esp_err_t ret = i2s_channel_write(g_i2s_tx_handle, stereo_buf, requested_bytes, &bytes_written, pdMS_TO_TICKS(timeout_ms));
    
    free(stereo_buf);
    
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(ret));
        return -1;
    }
    
    return (bytes_written / sizeof(int16_t)) / 2;
}

void audio_manager_play_boot_sequence(void) {
    if (!g_initialized) return;
    
    // Play a professional "startup chime" (C-Major Arpeggio: C4, E4, G4, C5)
    const float freqs[] = {261.63f, 329.63f, 392.00f, 523.25f};
    const int duration_ms = 150; 
    const int sample_rate = 16000;
    const float amplitude = 8000.0f; // 16-bit max is 32767
    
    audio_manager_start_playback();
    
    for (int note = 0; note < 4; note++) {
        int total_samples = (sample_rate * duration_ms) / 1000;
        int16_t* buffer = (int16_t*)malloc(total_samples * sizeof(int16_t)); // Mono
        if (!buffer) break;
        
        for (int i = 0; i < total_samples; i++) {
            float t = (float)i / sample_rate;
            int16_t val = (int16_t)(amplitude * sin(2.0f * M_PI * freqs[note] * t));
            buffer[i] = val; 
        }
        
        audio_manager_write(buffer, total_samples, 1000);
        free(buffer);
    }
    
    audio_manager_stop_playback();
}
