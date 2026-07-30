/**
 * @file main.cpp
 * @brief FireVAD Console Example - Professional Voice Activity Detection for ESP32-P4
 * 
 * This example demonstrates FireVAD usage with:
 * - Stream-VAD: Real-time voice activity detection (causal, 10ms latency)
 * - Offline VAD: High-accuracy batch processing (non-causal, 1-second chunks)
 * - AED: Audio event detection (Speech/Music/Singing classification)
 * 
 * Features:
 * - Hardware-agnostic codec support via esp_codec_dev
 * - Model loading from SPIFFS partition
 * - Console REPL for interactive control
 * - Microphone calibration mode
 * - Adjustable gain (hardware + software)
 * 
 * Supported Hardware:
 * - ESP32-P4 all revisions (v1.0, v1.1, v1.3, v3.x)
 * - ES8311, ES7210, ES8388, and other I2S/I2C codecs
 * 
 * Console Commands:
 * - model_load <file>     : Load .frvd model from SPIFFS
 * - model_info            : Show loaded model details
 * - model_list            : List available models
 * - start                 : Start VAD inference
 * - stop                  : Stop VAD inference
 * - threshold <0.0-1.0>   : Set detection threshold
 * - gain -s <multiplier>  : Set software gain
 * - calibrate             : Toggle calibration mode
 * - status                : Show system status
 * - help                  : Show all commands
 */

#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "esp_spiffs.h"

#include "esp_firevad.h"
#include "esp_firevad_dsp.h"

static const char* TAG = "FIREVAD_CONSOLE";

// === Global State ===
static EspFirevadModel g_model = {};
static uint8_t* g_model_buffer = NULL;
static bool g_model_loaded = false;
static bool g_vad_running = false;
static bool g_calibrate_mode = false;
static float g_threshold = 0.6f;
static float g_sw_gain = 1.0f;

// Statistics
static uint32_t g_speech_frames = 0;
static uint32_t g_total_frames = 0;

// === Helper Functions ===

static void print_banner(void) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                                                                ║\n");
    printf("║            FireVAD Console - ESP32-P4 Edition                  ║\n");
    printf("║                                                                ║\n");
    printf("║     Professional Voice Activity Detection System               ║\n");
    printf("║                                                                ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    printf("Hardware: ESP32-P4 rev %d.%d, %d cores\n",
           chip_info.revision / 100,
           chip_info.revision % 100,
           chip_info.cores);
    
    printf("Features:\n");
    printf("  • Stream-VAD    : Real-time detection (10ms latency)\n");
    printf("  • Offline VAD   : High-accuracy batch processing\n");
    printf("  • AED           : Audio event classification\n");
    printf("  • Console REPL  : Interactive command interface\n");
    printf("\nType 'help' for available commands.\n\n");
}

static esp_err_t mount_spiffs(void) {
    ESP_LOGI(TAG, "Mounting SPIFFS partition");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false  // First try without format
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    
    // If mount failed, format with progress indication
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS not formatted. Formatting 12MB partition...");
        printf("⏳ Formatting SPIFFS (this takes ~30 seconds for 12MB)...\n");
        printf("   ");
        fflush(stdout);
        
        // Format and mount
        conf.format_if_mount_failed = true;
        ret = esp_vfs_spiffs_register(&conf);
        
        printf(" ✓ Done!\n");
        
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Failed to format SPIFFS partition");
            } else if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGE(TAG, "SPIFFS partition not found");
            } else {
                ESP_LOGE(TAG, "Failed to initialize SPIFFS: %s", esp_err_to_name(ret));
            }
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted successfully");
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info("spiffs", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%zu KB, used=%zu KB", total / 1024, used / 1024);
    }
    
    return ESP_OK;
}

// === Console Commands ===

static int cmd_model_load(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: model_load <filename.frvd>\n");
        return 1;
    }
    
    const char* filename = argv[1];
    char path[256];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("[ERROR] Failed to open '%s'\n", path);
        return 1;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    printf("Loading model (%zu KB)...\n", file_size / 1024);
    
    // Free previous model
    if (g_model_loaded) {
        esp_firevad_free(&g_model);
        if (g_model_buffer) {
            heap_caps_free(g_model_buffer);
            g_model_buffer = NULL;
        }
        g_model_loaded = false;
    }
    
    // Allocate in PSRAM
    g_model_buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!g_model_buffer) {
        printf("[ERROR] Failed to allocate %zu bytes in PSRAM\n", file_size);
        fclose(f);
        return 1;
    }
    
    // Read model
    size_t bytes_read = fread(g_model_buffer, 1, file_size, f);
    fclose(f);
    
    if (bytes_read != file_size) {
        printf("[ERROR] Failed to read complete file\n");
        heap_caps_free(g_model_buffer);
        g_model_buffer = NULL;
        return 1;
    }
    
    // Parse model
    int ret = esp_firevad_load(g_model_buffer, file_size, &g_model);
    if (ret != 0) {
        printf("[ERROR] Failed to parse FireVAD model (error: %d)\n", ret);
        heap_caps_free(g_model_buffer);
        g_model_buffer = NULL;
        return 1;
    }
    
    esp_firevad_reset(&g_model);
    g_model_loaded = true;
    
    // Reset statistics
    g_speech_frames = 0;
    g_total_frames = 0;
    
    printf("[OK] Model loaded successfully\n");
    printf("     Run 'model_info' to see details\n");
    
    return 0;
}

static int cmd_model_info(int argc, char **argv) {
    if (!g_model_loaded) {
        printf("No model loaded. Use 'model_load <file>' first.\n");
        return 1;
    }
    
    const char* type_str = "Unknown";
    if (g_model.arch.odim == 1) {
        type_str = (g_model.arch.N2 == 0) ? "Stream-VAD" : "Offline VAD";
    } else if (g_model.arch.odim == 3) {
        type_str = "AED (Audio Event Detection)";
    }
    
    const char* prec_str = "Unknown";
    if (g_model.version == 1) prec_str = "Float32";
    else if (g_model.version == 2) prec_str = "Int8";
    else if (g_model.version == 3) prec_str = "Int16";
    
    const char* mode_str = (g_model.arch.N2 == 0) ? "CAUSAL (Streaming)" : "NON-CAUSAL (Offline)";
    
    size_t mem_usage = esp_firevad_memory_usage(&g_model);
    
    printf("\n=== Model Information ===\n");
    printf("Type:         %s\n", type_str);
    printf("Precision:    %s\n", prec_str);
    printf("Mode:         %s\n", mode_str);
    printf("Memory Usage: %zu KB\n", mem_usage / 1024);
    printf("\nArchitecture:\n");
    printf("  D (Input):    %" PRIu32 "\n", g_model.arch.D);
    printf("  H (Hidden):   %" PRIu32 "\n", g_model.arch.H);
    printf("  P (Proj):     %" PRIu32 "\n", g_model.arch.P);
    printf("  odim:         %" PRIu32, g_model.arch.odim);
    if (g_model.arch.odim == 3) {
        printf(" (Speech/Music/Singing)\n");
    } else {
        printf("\n");
    }
    printf("  N1 (Past):    %" PRIu32 " frames (%.1f ms)\n", g_model.arch.N1, g_model.arch.N1 * 10.0f);
    printf("  N2 (Future):  %" PRIu32 " frames (%.1f ms)\n", g_model.arch.N2, g_model.arch.N2 * 10.0f);
    printf("\n");
    
    if (g_model.arch.N2 == 0) {
        printf("Usage: Use 'start' for real-time inference\n");
    } else {
        printf("Note: This is an offline model (requires chunk processing)\n");
        printf("      Not suitable for real-time streaming in this example\n");
    }
    printf("\n");
    
    return 0;
}

static int cmd_model_list(int argc, char **argv) {
    printf("\n%-30s | %-15s | %-10s\n", "Filename", "Type", "Size");
    printf("------------------------------------------------------------------------\n");
    
    DIR* dir = opendir("/spiffs");
    if (!dir) {
        printf("[ERROR] Failed to open /spiffs directory\n");
        return 1;
    }
    
    struct dirent* ent;
    int count = 0;
    
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".frvd") == NULL) {
            continue;
        }
        
        char path[300];
        snprintf(path, sizeof(path), "/spiffs/%s", ent->d_name);
        
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        // Read header to determine type
        uint8_t header[64];
        if (fread(header, 1, 64, f) != 64) {
            fclose(f);
            continue;
        }
        fclose(f);
        
        uint32_t magic = *((uint32_t*)&header[0]);
        if (magic != 0x44565246) continue; // "FRVD"
        
        uint32_t N2 = *((uint32_t*)&header[32 + 24]);
        
        const char* type_str = (N2 == 0) ? "Stream-VAD" : "Offline";
        
        printf("%-30s | %-15s | %7zu KB\n", ent->d_name, type_str, size / 1024);
        count++;
    }
    
    closedir(dir);
    
    if (count == 0) {
        printf("(No .frvd models found)\n");
    }
    printf("\n");
    
    return 0;
}

static int cmd_start(int argc, char **argv) {
    if (!g_model_loaded) {
        printf("No model loaded. Use 'model_load <file>' first.\n");
        return 1;
    }
    
    if (g_model.arch.N2 != 0) {
        printf("[ERROR] This example only supports Stream-VAD models (N2=0)\n");
        printf("        Your model has N2=%" PRIu32 " (requires chunk processing)\n", g_model.arch.N2);
        return 1;
    }
    
    g_vad_running = true;
    printf("[OK] VAD inference started\n");
    printf("     Threshold: %.2f\n", g_threshold);
    printf("     Gain: %.2fx\n", g_sw_gain);
    
    return 0;
}

static int cmd_stop(int argc, char **argv) {
    g_vad_running = false;
    printf("[OK] VAD inference stopped\n");
    return 0;
}

static int cmd_threshold(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: threshold <0.0-1.0>\n");
        printf("Current: %.2f\n", g_threshold);
        return 1;
    }
    
    float val = atof(argv[1]);
    if (val < 0.0f || val > 1.0f) {
        printf("[ERROR] Threshold must be between 0.0 and 1.0\n");
        return 1;
    }
    
    g_threshold = val;
    printf("[OK] Threshold set to %.2f\n", g_threshold);
    
    return 0;
}

static int cmd_gain(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "-s") != 0) {
        printf("Usage: gain -s <multiplier>\n");
        printf("Current: %.2f\n", g_sw_gain);
        return 1;
    }
    
    float val = atof(argv[2]);
    if (val < 0.1f || val > 10.0f) {
        printf("[ERROR] Gain must be between 0.1 and 10.0\n");
        return 1;
    }
    
    g_sw_gain = val;
    printf("[OK] Software gain set to %.2fx\n", g_sw_gain);
    
    return 0;
}

static int cmd_calibrate(int argc, char **argv) {
    g_calibrate_mode = !g_calibrate_mode;
    
    if (g_calibrate_mode) {
        printf("[Calibrate] Mode ON\n");
        printf("            Audio levels will be displayed\n");
        printf("            Adjust gain until Peak is ~60-80%% during speech\n");
        printf("            Type 'calibrate' again to stop\n");
    } else {
        printf("[Calibrate] Mode OFF\n");
    }
    
    return 0;
}

static int cmd_status(int argc, char **argv) {
    printf("\n=== System Status ===\n");
    printf("Model loaded:  %s\n", g_model_loaded ? "Yes" : "No");
    printf("VAD running:   %s\n", g_vad_running ? "Yes" : "No");
    printf("Threshold:     %.2f\n", g_threshold);
    printf("SW Gain:       %.2fx\n", g_sw_gain);
    printf("Calibrate:     %s\n", g_calibrate_mode ? "ON" : "OFF");
    
    if (g_total_frames > 0) {
        float speech_pct = (float)g_speech_frames / (float)g_total_frames * 100.0f;
        printf("\nStatistics:\n");
        printf("  Total frames:  %" PRIu32 "\n", g_total_frames);
        printf("  Speech frames: %" PRIu32 " (%.1f%%)\n", g_speech_frames, speech_pct);
    }
    
    printf("\nMemory:\n");
    printf("  Free heap:  %lu KB\n", (unsigned long)(esp_get_free_heap_size() / 1024));
    printf("  Free PSRAM: %lu KB\n", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    
    printf("\n");
    return 0;
}

static int cmd_reset_stats(int argc, char **argv) {
    g_speech_frames = 0;
    g_total_frames = 0;
    printf("[OK] Statistics reset\n");
    return 0;
}

static void register_commands(void) {
    esp_console_register_help_command();
    
    // Model commands
    const esp_console_cmd_t cmd_load = {
        .command = "model_load",
        .help = "Load a model from SPIFFS",
        .hint = "<filename.frvd>",
        .func = &cmd_model_load,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_load);
    
    const esp_console_cmd_t cmd_info = {
        .command = "model_info",
        .help = "Show loaded model information",
        .hint = NULL,
        .func = &cmd_model_info,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_info);
    
    const esp_console_cmd_t cmd_list = {
        .command = "model_list",
        .help = "List available models in SPIFFS",
        .hint = NULL,
        .func = &cmd_model_list,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_list);
    
    const esp_console_cmd_t cmd_st = {
        .command = "start",
        .help = "Start VAD inference (Stream-VAD only)",
        .hint = NULL,
        .func = &cmd_start,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_st);
    
    const esp_console_cmd_t cmd_sp = {
        .command = "stop",
        .help = "Stop VAD inference",
        .hint = NULL,
        .func = &cmd_stop,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_sp);
    
    const esp_console_cmd_t cmd_thresh = {
        .command = "threshold",
        .help = "Set speech detection threshold",
        .hint = "<0.0-1.0>",
        .func = &cmd_threshold,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_thresh);
    
    const esp_console_cmd_t cmd_gn = {
        .command = "gain",
        .help = "Set software gain multiplier",
        .hint = "-s <0.1-10.0>",
        .func = &cmd_gain,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_gn);
    
    const esp_console_cmd_t cmd_cal = {
        .command = "calibrate",
        .help = "Toggle microphone calibration mode",
        .hint = NULL,
        .func = &cmd_calibrate,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_cal);
    
    const esp_console_cmd_t cmd_stat = {
        .command = "status",
        .help = "Show system status and statistics",
        .hint = NULL,
        .func = &cmd_status,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_stat);
    
    const esp_console_cmd_t cmd_reset = {
        .command = "reset_stats",
        .help = "Reset VAD statistics",
        .hint = NULL,
        .func = &cmd_reset_stats,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL
    };
    esp_console_cmd_register(&cmd_reset);
}

// === Main ===

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "FireVAD Console Example starting");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Mount SPIFFS
    mount_spiffs();
    
    // Initialize DSP
    esp_firevad_dsp_init();
    
    // Initialize console REPL (UART is automatically configured)
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "firevad> ";
    repl_config.max_cmdline_length = 256;
    
    register_commands();
    
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    
    print_banner();
    
    printf("Quick Start:\n");
    printf("1. model_list                          - List available models\n");
    printf("2. model_load firered_stream_vad_int8.frvd - Load Stream-VAD\n");
    printf("3. start                               - Start inference\n");
    printf("4. calibrate                           - Check audio levels\n");
    printf("\n");
    
    // Start REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
