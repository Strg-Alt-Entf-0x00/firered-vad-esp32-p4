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
#include "freertos/ringbuf.h"
#include "dsp_pipeline.h"
#include <math.h>

#define INMP441_BCLK_GPIO 20
#define INMP441_WS_GPIO   21
#define INMP441_DIN_GPIO  22

static const char* TAG = "AUDIO_MANAGER";

static bool g_initialized = false;
static volatile bool g_capture_running = false;
static volatile bool g_playback_running = false;
static float g_current_mic_gain = (float)CONFIG_FIREVAD_MIC_GAIN;
static mic_type_t g_active_mic = MIC_I2S1_INMP441;

// Ringbuffers
#define TX_RINGBUF_SIZE  32000
#define RX_RINGBUF_SIZE  32000
#define DSP_RINGBUF_SIZE 32000

static RingbufHandle_t g_tx_ringbuf = NULL;
static RingbufHandle_t g_rx_ringbuf = NULL;
static RingbufHandle_t g_dsp_ringbuf = NULL;

static TaskHandle_t g_tx_task_handle = NULL;
static TaskHandle_t g_rx_task_handle = NULL;
static TaskHandle_t g_dsp_task_handle = NULL;

// I2C Master Bus Handle
static i2c_master_bus_handle_t g_i2c_bus_handle = NULL;

// I2S Handles
static i2s_chan_handle_t g_i2s_rx_handle = NULL;
static i2s_chan_handle_t g_i2s_tx_handle = NULL;
static i2s_chan_handle_t g_i2s1_rx_handle = NULL;

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
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
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
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    
    ret = i2s_channel_init_std_mode(g_i2s_rx_handle, &std_cfg);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_init_std_mode(g_i2s_tx_handle, &std_cfg);    
    return ret;
}

static esp_err_t audio_i2s1_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &g_i2s1_rx_handle);
    if (ret != ESP_OK) return ret;
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_FIREVAD_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)CONFIG_FIREVAD_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)INMP441_BCLK_GPIO,
            .ws   = (gpio_num_t)INMP441_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)INMP441_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    
    ret = i2s_channel_init_std_mode(g_i2s1_rx_handle, &std_cfg);
    if (ret != ESP_OK) return ret;
    return i2s_channel_enable(g_i2s1_rx_handle);
}

static esp_err_t audio_codec_init(void) {
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
    fs.channel = 2; 
    fs.sample_rate = CONFIG_FIREVAD_SAMPLE_RATE;
    
    i2s_channel_enable(g_i2s_rx_handle);
    i2s_channel_enable(g_i2s_tx_handle);
    
    esp_err_t ret = g_codec_if->set_fs(g_codec_if, &fs);
    g_codec_if->set_mic_gain(g_codec_if, g_current_mic_gain);
    
    return ret;
}

// -------------------------------------------------------------
// THREADS (TX, RX, DSP)
// -------------------------------------------------------------

static void audio_tx_task(void *pvParameters) {
    while (1) {
        size_t size = 0;
        int16_t *data = (int16_t *)xRingbufferReceiveUpTo(g_tx_ringbuf, &size, portMAX_DELAY, 4000);
        if (data) {
            int samples = size / sizeof(int16_t);
            int16_t* stereo_buf = (int16_t*)malloc(samples * 2 * sizeof(int16_t));
            if (stereo_buf) {
                for (int i = 0; i < samples; i++) {
                    stereo_buf[i * 2] = data[i];
                    stereo_buf[i * 2 + 1] = data[i];
                }
                size_t bytes_written = 0;
                i2s_channel_write(g_i2s_tx_handle, stereo_buf, samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
                free(stereo_buf);
            }
            vRingbufferReturnItem(g_tx_ringbuf, (void *)data);
        }
    }
}

static void audio_rx_task(void *pvParameters) {
    const size_t READ_CHUNK_SAMPLES = 400; // 25ms chunk
    const size_t STEREO_BYTES = READ_CHUNK_SAMPLES * 2 * sizeof(int16_t);
    int16_t* stereo_buf = (int16_t*)malloc(STEREO_BYTES);
    int16_t* mono_buf = (int16_t*)malloc(READ_CHUNK_SAMPLES * sizeof(int16_t));
    
    while (1) {
        size_t bytes_read = 0;
        i2s_chan_handle_t read_handle = (g_active_mic == MIC_I2S1_INMP441) ? g_i2s1_rx_handle : g_i2s_rx_handle;
        
        // Always read from I2S to prevent DMA stalling!
        esp_err_t ret = i2s_channel_read(read_handle, stereo_buf, STEREO_BYTES, &bytes_read, portMAX_DELAY);
        
        if (ret == ESP_OK && g_capture_running) {
            int valid_stereo_samples = bytes_read / sizeof(int16_t);
            int valid_mono_samples = valid_stereo_samples / 2;
            
            // Extract Mono
            for (int i = 0; i < valid_mono_samples; i++) {
                mono_buf[i] = stereo_buf[i * 2];
            }
            
            // Push to RX Ringbuffer (do not block forever if full, we must keep I2S draining!)
            xRingbufferSend(g_rx_ringbuf, mono_buf, valid_mono_samples * sizeof(int16_t), pdMS_TO_TICKS(10));
        }
    }
}

static void audio_dsp_task(void *pvParameters) {
    while (1) {
        size_t size = 0;
        // Wait for mono audio from RX task
        int16_t *data = (int16_t *)xRingbufferReceiveUpTo(g_rx_ringbuf, &size, portMAX_DELAY, FIREVAD_CHUNK_SAMPLES_100MS);
        if (data) {
            int num_samples = size / sizeof(int16_t);
            
            // We can't modify the memory directly inside the ringbuffer if we don't own it, 
            // but RingbufferReceive returns a pointer to actual data we can read. 
            // Let's copy it to a local buffer for DSP processing.
            int16_t* dsp_buf = (int16_t*)malloc(num_samples * sizeof(int16_t));
            if (dsp_buf) {
                memcpy(dsp_buf, data, size);
                
                // 1. Process (High-Pass, AGC, etc)
                dsp_pipeline_process(dsp_buf, num_samples);
                
                // 2. Push to final DSP Ringbuffer for the VAD/User
                xRingbufferSend(g_dsp_ringbuf, dsp_buf, size, portMAX_DELAY);
                
                free(dsp_buf);
            }
            vRingbufferReturnItem(g_rx_ringbuf, (void *)data);
        }
    }
}

// -------------------------------------------------------------
// PUBLIC API
// -------------------------------------------------------------

bool audio_manager_is_initialized(void) { return g_initialized; }

esp_err_t audio_manager_init(void) {
    if (g_initialized) return ESP_OK;
    
    if (audio_i2c_init() != ESP_OK) return ESP_FAIL;
    if (audio_i2s_init() != ESP_OK) return ESP_FAIL;
    if (audio_i2s1_init() != ESP_OK) return ESP_FAIL;
    if (audio_codec_init() != ESP_OK) return ESP_FAIL;
    
    dsp_pipeline_init();
    audio_manager_set_speaker_vol(50);
    
    g_tx_ringbuf = xRingbufferCreate(TX_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    g_rx_ringbuf = xRingbufferCreate(RX_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    g_dsp_ringbuf = xRingbufferCreate(DSP_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    
    if (!g_tx_ringbuf || !g_rx_ringbuf || !g_dsp_ringbuf) {
        ESP_LOGE(TAG, "Failed to create ringbuffers");
        return ESP_FAIL;
    }
    
    // Core 1 (App Core) or Core 0 (Pro Core)? Let's use Core 1 for Audio RX/DSP, Core 0 for VAD.
    // Audio hardware interrupts are usually pinned to Core 1.
    xTaskCreatePinnedToCore(audio_tx_task, "audio_tx", 4096, NULL, 15, &g_tx_task_handle, 1);
    xTaskCreatePinnedToCore(audio_rx_task, "audio_rx", 4096, NULL, 22, &g_rx_task_handle, 1);
    xTaskCreatePinnedToCore(audio_dsp_task, "audio_dsp", 8192, NULL, 18, &g_dsp_task_handle, 1);
    
    g_initialized = true;
    ESP_LOGI(TAG, "=== Multithreaded Audio Manager Initialized ===");
    return ESP_OK;
}

// audio_manager_deinit removed (Dead Code)
esp_err_t audio_manager_set_mic_gain(float gain_db) {
    if (gain_db < 0.0f || gain_db > 42.0f) return ESP_ERR_INVALID_ARG;
    if (!g_codec_if) return ESP_FAIL;
    esp_err_t ret = g_codec_if->set_mic_gain(g_codec_if, gain_db);
    if (ret == ESP_OK) {
        g_current_mic_gain = gain_db;
        ESP_LOGI(TAG, "Mic gain set to %.1fdB", gain_db);
    }
    return ret;
}

esp_err_t audio_manager_get_mic_gain(float* gain_db) {
    if (!gain_db) return ESP_ERR_INVALID_ARG;
    *gain_db = g_current_mic_gain;
    return ESP_OK;
}

esp_err_t audio_manager_set_mic(mic_type_t mic) {
    g_active_mic = mic;
    ESP_LOGI(TAG, "Active microphone set to %s", mic == MIC_I2S1_INMP441 ? "INMP441 (I2S1)" : "ES8311 (I2S0)");
    return ESP_OK;
}

mic_type_t audio_manager_get_mic(void) {
    return g_active_mic;
}

esp_err_t audio_manager_set_speaker_vol(uint8_t vol_level) {
    if (vol_level > 100) return ESP_ERR_INVALID_ARG;
    if (!g_codec_if) return ESP_FAIL;
    float db_val = (vol_level == 0) ? -90.0f : ((vol_level - 100.0f) * 0.5f); 
    if (db_val > 0.0f) db_val = 0.0f;
    return g_codec_if->set_vol(g_codec_if, db_val);
}

esp_err_t audio_manager_start_capture(void) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (g_capture_running) return ESP_OK;
    esp_err_t ret = g_codec_if->enable(g_codec_if, true);
    if (ret != ESP_OK) return ret;
    g_capture_running = true;
    
    // Clear old data in DSP ringbuffer
    size_t dummy;
    void* d = xRingbufferReceiveUpTo(g_dsp_ringbuf, &dummy, 0, DSP_RINGBUF_SIZE);
    if (d) vRingbufferReturnItem(g_dsp_ringbuf, d);
    
    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

esp_err_t audio_manager_stop_capture(void) {
    if (!g_capture_running) return ESP_OK;
    if (!g_playback_running) {
        g_codec_if->enable(g_codec_if, false);
    }
    g_capture_running = false;
    ESP_LOGI(TAG, "Audio capture stopped");
    return ESP_OK;
}

int audio_manager_read(int16_t* buffer, size_t samples, uint32_t timeout_ms) {
    if (!g_capture_running) return 0;
    
    size_t requested_bytes = samples * sizeof(int16_t);
    size_t total_received_bytes = 0;
    uint32_t start_time = xTaskGetTickCount();
    
    while (total_received_bytes < requested_bytes) {
        if (!g_capture_running) break;
        
        uint32_t elapsed = (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS;
        if (elapsed >= timeout_ms && timeout_ms != portMAX_DELAY) {
            break; // Timeout
        }
        
        uint32_t wait_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms - elapsed);
        if (wait_ticks == 0 && timeout_ms != 0) wait_ticks = 1;
        
        size_t received_bytes = 0;
        void *data = xRingbufferReceiveUpTo(g_dsp_ringbuf, &received_bytes, wait_ticks, requested_bytes - total_received_bytes);
        
        if (data) {
            memcpy((uint8_t*)buffer + total_received_bytes, data, received_bytes);
            vRingbufferReturnItem(g_dsp_ringbuf, data);
            total_received_bytes += received_bytes;
        }
    }
    
    return total_received_bytes / sizeof(int16_t);
}

esp_err_t audio_manager_get_levels(float *rms_out, float *peak_out, int *clipping_count) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    
    const int sample_rate = CONFIG_FIREVAD_SAMPLE_RATE;
    const int chunk_size = 400; 
    const int chunks_per_sec = sample_rate / chunk_size;
    
    int total_clipping = 0;
    float max_peak = 0.0f;
    double sum_sq = 0.0;
    
    int16_t* chunk_buf = (int16_t*)malloc(chunk_size * sizeof(int16_t));
    if (!chunk_buf) return ESP_ERR_NO_MEM;
    
    bool was_running = g_capture_running;
    if (!was_running) {
        audio_manager_start_capture();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    for (int i = 0; i < chunks_per_sec; i++) {
        int read = audio_manager_read(chunk_buf, chunk_size, FIREVAD_I2S_READ_TIMEOUT_MS);
        for (int j = 0; j < read; j++) {
            float val = (float)chunk_buf[j];
            sum_sq += (val * val);
            float abs_val = fabsf(val);
            if (abs_val > max_peak) max_peak = abs_val;
            if (abs_val >= 32700.0f) total_clipping++;
        }
    }
    
    if (!was_running) {
        audio_manager_stop_capture();
    }
    
    free(chunk_buf);
    
    *rms_out = sqrtf((float)(sum_sq / (float)sample_rate));
    *peak_out = max_peak;
    *clipping_count = total_clipping;
    
    return ESP_OK;
}

esp_err_t audio_manager_start_playback(void) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (g_playback_running) return ESP_OK;
    esp_err_t ret = g_codec_if->enable(g_codec_if, true);
    if (ret != ESP_OK) return ret;
    audio_manager_set_speaker_vol(100);
    g_playback_running = true;
    return ESP_OK;
}

esp_err_t audio_manager_stop_playback(void) {
    if (!g_playback_running) return ESP_OK;
    uint32_t wait_ticks = 3000 / 10;
    while (xRingbufferGetCurFreeSize(g_tx_ringbuf) < TX_RINGBUF_SIZE && wait_ticks > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ticks--;
    }
    g_playback_running = false;
    if (!g_capture_running) {
        g_codec_if->enable(g_codec_if, false);
    }
    return ESP_OK;
}

int audio_manager_write(const int16_t* buffer, size_t samples, uint32_t timeout_ms) {
    if (!g_playback_running) return 0;
    size_t size = samples * sizeof(int16_t);
    BaseType_t res = xRingbufferSend(g_tx_ringbuf, buffer, size, pdMS_TO_TICKS(timeout_ms));
    if (res != pdTRUE) return -1;
    return samples;
}

void audio_manager_play_boot_sequence(void) {
    if (!g_initialized) return;
    const float freqs[] = {261.63f, 329.63f, 392.00f, 523.25f};
    const int duration_ms = 150; 
    const int sample_rate = FIREVAD_SAMPLE_RATE;
    const float amplitude = 8000.0f; 
    
    audio_manager_start_playback();
    for (int note = 0; note < 4; note++) {
        int total_samples = (sample_rate * duration_ms) / 1000;
        int16_t* buffer = (int16_t*)malloc(total_samples * sizeof(int16_t));
        if (!buffer) break;
        for (int i = 0; i < total_samples; i++) {
            float t = (float)i / sample_rate;
            buffer[i] = (int16_t)(amplitude * sin(2.0f * M_PI * freqs[note] * t)); 
        }
        audio_manager_write(buffer, total_samples, FIREVAD_I2S_READ_TIMEOUT_MS);
        free(buffer);
    }
    audio_manager_stop_playback();
}
