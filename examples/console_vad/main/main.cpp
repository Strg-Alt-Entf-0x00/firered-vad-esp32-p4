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
#include "file_protocol.h"
#include "filesystem_manager.h"
#include "driver/uart.h"

// Global handles for UART File Manager
FileProtocol* g_file_protocol = nullptr;
FilesystemManager* g_fs_manager = nullptr;

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


// UART Configuration for File Protocol
#define FILE_UART_NUM      UART_NUM_1
#define FILE_UART_TX_PIN   30  // GPIO30
#define FILE_UART_RX_PIN   31  // GPIO31
#define FILE_UART_RTS_PIN  50  // GPIO50
#define FILE_UART_CTS_PIN  29  // GPIO29
#define FILE_UART_BAUD     3000000
#define FILE_UART_BUF_SIZE 8192

static esp_err_t uart_tx_callback(const uint8_t* data, size_t len) {
    int written = uart_write_bytes(FILE_UART_NUM, data, len);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

static void uart_rx_task(void* arg) {
    uint8_t* rx_buf = (uint8_t*)malloc(FILE_UART_BUF_SIZE);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate UART RX buffer");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "UART RX task started - waiting for data on UART%d (GPIO%d/GPIO%d @ %d baud)", 
             FILE_UART_NUM, FILE_UART_TX_PIN, FILE_UART_RX_PIN, FILE_UART_BAUD);
             
    while (1) {
        int len = uart_read_bytes(FILE_UART_NUM, rx_buf, FILE_UART_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            if (g_file_protocol) {
                g_file_protocol->process_rx_data(rx_buf, len);
            }
        }
    }
}

static esp_err_t init_uart_file_protocol(void) {
    ESP_LOGI(TAG, "Initializing UART File Transfer Protocol...");
    
    uart_config_t uart_config = {
        .baud_rate = FILE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = 96,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    
    ESP_ERROR_CHECK(uart_param_config(FILE_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(FILE_UART_NUM, FILE_UART_TX_PIN, FILE_UART_RX_PIN, FILE_UART_RTS_PIN, FILE_UART_CTS_PIN));
    ESP_ERROR_CHECK(uart_driver_install(FILE_UART_NUM, FILE_UART_BUF_SIZE, FILE_UART_BUF_SIZE, 0, NULL, 0));
    
    g_fs_manager = new FilesystemManager();
    if (g_fs_manager) g_fs_manager->init();
    
    g_file_protocol = new FileProtocol();
    if (g_file_protocol) {
        g_file_protocol->init(g_fs_manager);
        g_file_protocol->set_tx_callback(uart_tx_callback);
    }
    
    xTaskCreate(uart_rx_task, "uart_rx", 16384, NULL, 5, NULL);
    
    return ESP_OK;
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
