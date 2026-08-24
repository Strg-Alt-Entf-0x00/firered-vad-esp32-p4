/**
 * @file simple_file_manager.h
 * @brief Simple File Manager for ESP32-P4 ASR
 * 
 * Simplified version for autonomous file operations on SD card.
 * Uses Python tool from USB Bridge project for file transfer.
 */

#ifndef SIMPLE_FILE_MANAGER_H
#define SIMPLE_FILE_MANAGER_H

#include "esp_err.h"
#include "sd_card.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize simple file manager
 * 
 * @param sd_handle SD card handle
 * @return ESP_OK on success
 */
esp_err_t file_manager_init(sd_card_handle_t sd_handle);

/**
 * @brief List files in directory
 * 
 * @param path Directory path
 * @return ESP_OK on success
 */
esp_err_t file_manager_list(const char* path);

/**
 * @brief Check if file exists
 * 
 * @param path File path
 * @return true if exists
 */
bool file_manager_exists(const char* path);

/**
 * @brief Get file size
 * 
 * @param path File path
 * @param size Output size in bytes
 * @return ESP_OK on success
 */
esp_err_t file_manager_get_size(const char* path, size_t* size);

/**
 * @brief Delete file
 * 
 * @param path File path
 * @return ESP_OK on success
 */
esp_err_t file_manager_delete(const char* path);

#ifdef __cplusplus
}
#endif

#endif // SIMPLE_FILE_MANAGER_H
