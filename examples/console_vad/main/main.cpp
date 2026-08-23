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

#include "esp_pm.h"

#include "esp_firevad.h"
#include "esp_firevad_dsp.h"
#include "config.h"
#include "cmd_vad_cli.h"
#include "audio_manager.h"
#include "vad_runner.h"
#include "esp_uart_filebridge.h"
#include "driver/uart.h"

static const char* TAG = "MAIN";

#include "esp_heap_caps.h"

static void print_banner(void) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t sram_size = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    
    int cpu_freq_mhz = 0;
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
    cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#endif
    
    printf("\n");
    printf("  FIREVAD CONSOLE\n");
    printf("  ESP32-P4 Edition\n");
    printf("  --------------------------------------------------\n");
    if (cpu_freq_mhz > 0) {
        printf("  Hardware : ESP32-P4 rev %d.%d (%d-Core @ %dMHz)\n", 
               chip_info.revision / 100, chip_info.revision % 100, chip_info.cores, cpu_freq_mhz);
    } else {
        printf("  Hardware : ESP32-P4 rev %d.%d (%d-Core)\n", 
               chip_info.revision / 100, chip_info.revision % 100, chip_info.cores);
    }
    printf("  Memory   : %zu MB PSRAM / %zu KB SRAM\n", 
           psram_size / (1024 * 1024), sram_size / 1024);
    printf("  Version  : v1.1.0\n\n");
}


static esp_err_t init_uart_file_protocol(void) {
    ESP_LOGI(TAG, "Initializing UART File Transfer Protocol...");
    
    // We use the new esp-uart-filebridge component
    esp_uart_filebridge_config_t cfg = ESP_UART_FILEBRIDGE_CONFIG_DEFAULT();
    // Default pins are correct for Waveshare ESP32-P4-WIFI6 (TX:30, RX:31, RTS:50, CTS:29)
    // Default baud is 3000000.
    
    return esp_uart_filebridge_init(&cfg);
}
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting FireVAD System");
    
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 360,
        .min_freq_mhz = 40,
        .light_sleep_enable = true
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Power Management Configured (max 360MHz, light sleep enabled)");
#endif
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    init_uart_file_protocol();
    esp_firevad_dsp_init();
    esp_firevad_dsp_init();
    audio_manager_init();
    
    // Auto-calibrate background noise for 1 second
    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, " PLEASE BE SILENT - CALIBRATING MICROPHONE...  ");
    ESP_LOGI(TAG, "===============================================");
    vad_runner_calibrate_noise_blocking(1);
    vad_runner_set_pre_vad_threshold(0.0f); // Disable Pre-VAD by default for pure NN output
    
    // Play startup chime AFTER calibration to signify system is ready and avoid interfering with the mic
    audio_manager_play_boot_sequence();
    
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
    
    printf("  Type 'help'  to see all commands.\n\n");
    
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
