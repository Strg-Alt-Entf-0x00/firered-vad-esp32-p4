/*
 * Unified interface for FATFS (SD card)
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include "esp_err.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// Mount points
#define MOUNT_POINT_SD      "/sd"

class FilesystemManager {
public:
    FilesystemManager();
    ~FilesystemManager();

    // Non-copyable
    FilesystemManager(const FilesystemManager&) = delete;
    FilesystemManager& operator=(const FilesystemManager&) = delete;

    /**
     * Initialize filesystem manager
     * @return ESP_OK on success
     */
    esp_err_t init();


    /**
     * Mount SD card (FAT32/exFAT)
     * @return ESP_OK on success
     */
    esp_err_t mount_sd();

    /**
     * Unmount SD card
     */
    void unmount_sd();


    /**
     * Check if SD card is mounted
     */
    bool is_sd_mounted() const { return m_sd_mounted; }

    /**
     * Get SD card information
     */
    sdmmc_card_t* get_sd_card_info() { return m_sd_card; }

    /**
     * @param path Mount point ("/sd")
     * @param total_bytes Output: total bytes
     * @param free_bytes Output: free bytes
     * @return ESP_OK on success
     */
    esp_err_t get_space_info(const char* path, uint64_t* total_bytes, uint64_t* free_bytes);

    /**
     * List directory contents
     * @param path Directory path
     * @param callback Function called for each entry
     * @param user_data User data passed to callback
     * @return ESP_OK on success
     */
    esp_err_t list_directory(const char* path, 
                             void (*callback)(const char* name, uint64_t size, 
                                            bool is_dir, uint32_t timestamp, void* user_data),
                             void* user_data);

    /**
     * Get file statistics
     */
    esp_err_t stat_file(const char* path, uint64_t* size, uint32_t* timestamp, bool* is_dir);

    /**
     * Delete file or directory (recursive for directories)
     */
    esp_err_t delete_file(const char* path);

    /**
     * Rename/move file
     */
    esp_err_t rename_file(const char* old_path, const char* new_path);

    /**
     * Create directory
     */
    esp_err_t create_directory(const char* path);

    /**
     * Copy file (within same or between filesystems)
     */
    esp_err_t copy_file(const char* src_path, const char* dst_path);

    /**
     * Calculate CRC32 hash of file
     */
    esp_err_t hash_file(const char* path, uint32_t* hash);


    /**
     * Format SD card (WARNING: Destroys all data!)
     */
    esp_err_t format_sd();

private:
    bool m_initialized = false;
    bool m_sd_mounted = false;
    
    // SD card handle
    sdmmc_card_t* m_sd_card = nullptr;
    void* m_sd_pwr_ctrl = nullptr;  // SD power control handle for ESP32-P4
    
    // Helper functions
    bool is_sd_path(const char* path);
    esp_err_t validate_path(const char* path);
    
    // [FIX] Recursive directory deletion helper
    esp_err_t delete_directory_recursive(const char* path);
};
