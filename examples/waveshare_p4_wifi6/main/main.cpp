/**
 * @file main.cpp
 * @brief Console-based Minimal example for ESP32-P4 with FireVAD using ES8311 I2C codec + I2S.
 */

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_fat.h"
#include "argtable3/argtable3.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "audio_hal_i2s.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "es8311_codec.h"

#include "esp_firevad.h"
#include "esp_firevad_dsp.h"
#include "synth_fx.h"

static const char* TAG = "FIREVAD_EXAMPLE";

esp_codec_dev_handle_t g_play_dev = NULL;
EspFirevadModel g_model;
uint8_t* g_model_buf = NULL;
bool g_model_loaded = false;
bool g_vad_running = false;
TaskHandle_t g_vad_task_handle = NULL;

// Audio parameters
float g_sw_gain = 1.0f;
int g_hw_gain = 40;

// Weak function override for synth_fx
extern "C" void app_audio_write(int16_t* buffer, size_t num_bytes) {
    if (g_play_dev) {
        esp_codec_dev_write(g_play_dev, buffer, num_bytes);
    }
}

static esp_err_t mount_spiffs(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS");
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) ESP_LOGE(TAG, "Failed to mount or format filesystem");
        else if (ret == ESP_ERR_NOT_FOUND) ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        else ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

static bool load_model_from_spiffs(const char* model_name) {
    if (g_model_loaded) {
        esp_firevad_free(&g_model);
        if (g_model_buf) {
            heap_caps_free(g_model_buf);
            g_model_buf = NULL;
        }
        g_model_loaded = false;
    }

    char path[128];
    snprintf(path, sizeof(path), "/spiffs/%s", model_name);

    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open model file: %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    ESP_LOGI(TAG, "Allocating %u bytes for model...", file_size);
    g_model_buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!g_model_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory in PSRAM");
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(g_model_buf, 1, file_size, f);
    fclose(f);

    if (read_bytes != file_size) {
        ESP_LOGE(TAG, "Failed to read entire file");
        heap_caps_free(g_model_buf);
        g_model_buf = NULL;
        return false;
    }

    if (esp_firevad_load(g_model_buf, file_size, &g_model) != 0) {
        ESP_LOGE(TAG, "Failed to parse FireVAD model");
        heap_caps_free(g_model_buf);
        g_model_buf = NULL;
        return false;
    }

    esp_firevad_reset(&g_model);
    g_model_loaded = true;
    ESP_LOGI(TAG, "Model %s loaded successfully", model_name);
    return true;
}

#define CHUNK_FRAMES 100
#define SAMPLES_PER_FRAME 160

static void vad_task(void* arg) {
    int16_t audio_frame[SAMPLES_PER_FRAME];
    float features[80];
    
    // Offline buffers
    int16_t* chunk_audio = (int16_t*)heap_caps_malloc(CHUNK_FRAMES * SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    float* chunk_features = (float*)heap_caps_malloc(CHUNK_FRAMES * 80 * sizeof(float), MALLOC_CAP_SPIRAM);
    int chunk_frame_idx = 0;

    while (1) {
        if (!g_vad_running || !g_model_loaded) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Read 160 samples (10ms at 16kHz)
        if (esp_codec_dev_read(g_play_dev, audio_frame, sizeof(audio_frame)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // Apply software gain
        if (g_sw_gain != 1.0f) {
            for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
                int32_t val = (int32_t)(audio_frame[i] * g_sw_gain);
                if (val > 32767) val = 32767;
                else if (val < -32768) val = -32768;
                audio_frame[i] = (int16_t)val;
            }
        }

        if (g_model.arch.N2 == 0) {
            // Streaming mode (Stream-VAD)
            float energy = 0.0f;
            esp_firevad_dsp_extract_features(audio_frame, features, &energy);

            float prob = 0.0f;
            esp_firevad_infer_frame(&g_model, features, true, &prob);
            
            if (prob > 0.6f) {
                ESP_LOGI(TAG, "[Stream-VAD] SPEECH DETECTED (Prob: %.4f | Energy: %.3f)", prob, energy);
                // synth_fx_play(SYNTH_FX_ACK);
            }
        } else {
            // Offline mode (VAD, AED)
            memcpy(chunk_audio + chunk_frame_idx * SAMPLES_PER_FRAME, audio_frame, sizeof(audio_frame));
            chunk_frame_idx++;

            if (chunk_frame_idx == CHUNK_FRAMES) {
                ESP_LOGI(TAG, "Processing 1-second chunk offline...");
                float energy = 0.0f;
                for (int f = 0; f < CHUNK_FRAMES; f++) {
                    esp_firevad_dsp_extract_features(chunk_audio + f * SAMPLES_PER_FRAME, chunk_features + f * 80, &energy);
                }

                uint32_t odim = g_model.arch.odim;
                float* out_probs = (float*)heap_caps_malloc(CHUNK_FRAMES * odim * sizeof(float), MALLOC_CAP_SPIRAM);

                esp_firevad_infer_chunk(&g_model, chunk_features, CHUNK_FRAMES, true, out_probs);

                for (int f = 0; f < CHUNK_FRAMES; f++) {
                    if (odim == 1) {
                        float prob = out_probs[f];
                        if (prob > 0.6f) {
                            ESP_LOGI(TAG, "[Offline VAD] Speech at frame %d (Prob: %.4f)", f, prob);
                        }
                    } else if (odim == 3) {
                        float p0 = out_probs[f * 3 + 0];
                        float p1 = out_probs[f * 3 + 1];
                        float p2 = out_probs[f * 3 + 2];
                        if (p0 > 0.6f || p1 > 0.6f || p2 > 0.6f) {
                            ESP_LOGI(TAG, "[AED] Event at frame %d (C0: %.2f, C1: %.2f, C2: %.2f)", f, p0, p1, p2);
                        }
                    }
                }
                
                heap_caps_free(out_probs);
                chunk_frame_idx = 0;
            }
        }
    }
}

// ---- Console Commands ----

static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_model(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    
    const char* m = model_args.model->sval[0];
    const char* file_name = "";
    if (strcmp(m, "vad") == 0) file_name = "firered_vad.frvd";
    else if (strcmp(m, "stream") == 0) file_name = "firered_stream_vad.frvd";
    else if (strcmp(m, "aed") == 0) file_name = "firered_aed.frvd";
    else {
        printf("Unknown model. Use: vad, stream, aed\n");
        return 1;
    }

    bool was_running = g_vad_running;
    g_vad_running = false; // Pause inference
    vTaskDelay(pdMS_TO_TICKS(50));

    if (load_model_from_spiffs(file_name)) {
        if (g_model.arch.N2 == 0) {
            printf("Mode: STREAMING (Live 10ms)\n");
        } else {
            printf("Mode: OFFLINE (Chunked 1-second)\n");
        }
        synth_fx_play(SYNTH_FX_READY);
    }

    g_vad_running = was_running;
    return 0;
}

static struct {
    struct arg_int *hw_gain;
    struct arg_dbl *sw_gain;
    struct arg_end *end;
} gain_args;

static int cmd_gain(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&gain_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, gain_args.end, argv[0]);
        return 1;
    }

    if (gain_args.hw_gain->count > 0) {
        g_hw_gain = gain_args.hw_gain->ival[0];
        if (g_play_dev) {
            esp_codec_dev_set_in_gain(g_play_dev, g_hw_gain);
            printf("Hardware in-gain set to %d\n", g_hw_gain);
        }
    }

    if (gain_args.sw_gain->count > 0) {
        g_sw_gain = (float)gain_args.sw_gain->dval[0];
        printf("Software multiplier set to %.2f\n", g_sw_gain);
    }

    return 0;
}

static int cmd_start(int argc, char **argv) {
    if (!g_model_loaded) {
        printf("No model loaded! Use 'model load <vad|stream|aed>' first.\n");
        return 1;
    }
    g_vad_running = true;
    printf("VAD started.\n");
    return 0;
}

static int cmd_stop(int argc, char **argv) {
    g_vad_running = false;
    printf("VAD stopped.\n");
    return 0;
}

static int cmd_status(int argc, char **argv) {
    printf("--- FireVAD Status ---\n");
    printf("Model loaded: %s\n", g_model_loaded ? "Yes" : "No");
    printf("VAD running: %s\n", g_vad_running ? "Yes" : "No");
    printf("Hardware Gain: %d\n", g_hw_gain);
    printf("Software Gain: %.2f\n", g_sw_gain);
    
    if (g_model_loaded) {
        size_t mem = esp_firevad_memory_usage(&g_model);
        printf("Model Memory Usage: %u bytes\n", mem);
    }
    printf("Free Internal Heap: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("Free PSRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static void register_console_commands() {
    model_args.model = arg_str1(NULL, NULL, "<vad|stream|aed>", "Model to load");
    model_args.end = arg_end(2);
    const esp_console_cmd_t model_cmd = {
        .command = "model",
        .help = "Load a model from SPIFFS",
        .hint = NULL,
        .func = &cmd_model,
        .argtable = &model_args
    };
    esp_console_cmd_register(&model_cmd);

    gain_args.hw_gain = arg_int0("h", "hw", "<db>", "Hardware codec gain (0-100)");
    gain_args.sw_gain = arg_dbl0("s", "sw", "<mul>", "Software digital gain multiplier (1.0 = normal)");
    gain_args.end = arg_end(2);
    const esp_console_cmd_t gain_cmd = {
        .command = "gain",
        .help = "Adjust microphone gain",
        .hint = NULL,
        .func = &cmd_gain,
        .argtable = &gain_args
    };
    esp_console_cmd_register(&gain_cmd);

    const esp_console_cmd_t start_cmd = {
        .command = "start", .help = "Start VAD inference", .hint = NULL, .func = &cmd_start, .argtable = NULL
    };
    esp_console_cmd_register(&start_cmd);

    const esp_console_cmd_t stop_cmd = {
        .command = "stop", .help = "Stop VAD inference", .hint = NULL, .func = &cmd_stop, .argtable = NULL
    };
    esp_console_cmd_register(&stop_cmd);

    const esp_console_cmd_t status_cmd = {
        .command = "status", .help = "Show system status", .hint = NULL, .func = &cmd_status, .argtable = NULL
    };
    esp_console_cmd_register(&status_cmd);
}

// ---- Main Application ----

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting FireVAD Advanced Console Example on ESP32-P4");

    mount_spiffs();

    // 1. Initialize Audio HAL (I2S)
    audio_hal_i2s_init(16000, 0, 13, 12, 10, 9, 11);
    
    // 2. Initialize I2C for Codec
    i2c_master_bus_handle_t i2c_bus_handle;
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = (i2c_port_t)0,
        .sda_io_num = (gpio_num_t)7,
        .scl_io_num = (gpio_num_t)8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = true, .allow_pd = false }
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_handle));

    // 3. Setup ES8311 Codec Device
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = 0;
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = i2c_bus_handle;
    const audio_codec_ctrl_if_t *out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.use_mclk = true;
    const audio_codec_if_t *out_codec_if = es8311_codec_new(&es8311_cfg);

    audio_codec_i2s_cfg_t i2s_data_cfg = {};
    i2s_data_cfg.port = 0;
    i2s_data_cfg.tx_handle = (i2s_chan_handle_t)audio_hal_i2s_get_tx_handle();
    i2s_data_cfg.rx_handle = (i2s_chan_handle_t)audio_hal_i2s_get_rx_handle();
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = out_codec_if;
    dev_cfg.data_if = data_if;
    
    g_play_dev = esp_codec_dev_new(&dev_cfg);
    if (g_play_dev) {
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate = 16000;
        fs.channel = 1;
        fs.bits_per_sample = 16;
        esp_codec_dev_open(g_play_dev, &fs);
        esp_codec_dev_set_out_vol(g_play_dev, 70);
        esp_codec_dev_set_in_gain(g_play_dev, g_hw_gain);
    }

    esp_firevad_dsp_init();
    synth_fx_init();

    // Start VAD Task
    xTaskCreate(vad_task, "vad_task", 8192, NULL, 5, &g_vad_task_handle);

    // 4. Initialize Console REPL
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "firevad>";
    repl_config.max_cmdline_length = 256;

    esp_console_register_help_command();
    register_console_commands();

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

    ESP_LOGI(TAG, "Console REPL started. Type 'help' for commands.");
    printf("\nWelcome to FireVAD Console!\n");
    printf("1. Load a model: model stream\n");
    printf("2. Start inference: start\n");
    printf("3. Check status: status\n\n");

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
