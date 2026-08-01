/**
 * @file main.cpp
 * @brief FireVAD Console Example - Professional Voice Activity Detection
 */

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "esp_spiffs.h"

#include "esp_firevad.h"
#include "esp_firevad_dsp.h"
#include "config.h"
#include "cmd_vad_cli.h"
#include "audio_manager.h"
#include "vad_runner.h"

static const char* TAG = "MAIN";

static void print_banner(void) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    printf("\n");
    printf("  FIREVAD CONSOLE\n");
    printf("  ESP32-P4 Edition\n");
    printf("  --------------------------------------------------\n");
    printf("  Hardware : ESP32-P4 rev %d.%d (%d-Core @ 400MHz)\n", 
           chip_info.revision / 100, chip_info.revision % 100, chip_info.cores);
    printf("  Memory   : 32MB PSRAM / 768KB SRAM\n");
    printf("  Version  : v1.1.0\n\n");
    printf("  Features:\n");
    printf("  - Stream-VAD    : Real-time detection\n");
    printf("  - Console REPL  : Interactive command interface\n");
    printf("  - Record & Play : Full audio testing loop\n");
    printf("  - Metrics       : Latency and CPU profiling\n\n");
}

static esp_err_t mount_spiffs(void) {
    ESP_LOGI(TAG, "Mounting SPIFFS partition");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS not formatted. Formatting (takes ~30s)...");
        conf.format_if_mount_failed = true;
        ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS");
            return ret;
        }
    }
    
    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%zu KB, used=%zu KB", total / 1024, used / 1024);
    
    return ESP_OK;
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting FireVAD System");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    mount_spiffs();
    esp_firevad_dsp_init();
    audio_manager_init();
    
    // Test the speaker with a boot chime sequence
    audio_manager_set_speaker_vol(100);
    audio_manager_play_boot_sequence();
    
    // Auto-calibrate background noise for 1 second and enable Pre-VAD (1.5x)
    ESP_LOGI(TAG, "Running 1-second auto-calibration on boot...");
    vad_runner_calibrate_noise_blocking(1);
    vad_runner_set_pre_vad_threshold(1.5f);
    
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = CONSOLE_PROMPT;
    repl_config.max_cmdline_length = CONSOLE_MAX_COMMAND_LINE;
    repl_config.task_stack_size = 8192;  // Default 4096 is too small for VAD inference chain
    // Register VAD console commands
    cmd_vad_cli_register();
    
    // Start console REPL
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    
    print_banner();
    
    printf("  Type 'help'  to see all commands.\n");
    printf("  Type 'guide' to see the Quick Start workflow.\n\n");
    
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
