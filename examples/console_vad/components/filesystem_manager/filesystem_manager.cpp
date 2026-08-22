/*
 * ESP32-P4 USB Bridge File Manager - Filesystem Manager Implementation
 */

#include "filesystem_manager.h"
#include "esp_log.h"

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"  // SD power control for ESP32-P4
#include <sys/stat.h>
#include <sys/unistd.h>
#include <cstring>
#include <dirent.h>

static const char* TAG = "fs_manager";

// SD card pins for ESP32-P4 Waveshare ESP32-P4-WIFI6
// Source: Zephyr Project Documentation (official Waveshare board definition)
// MicroSD card slot: 4-bit SDHC at 40 MHz
#define SD_PIN_CLK      (gpio_num_t)43
#define SD_PIN_CMD      (gpio_num_t)44
#define SD_PIN_D0       (gpio_num_t)39
#define SD_PIN_D1       (gpio_num_t)40
#define SD_PIN_D2       (gpio_num_t)41
#define SD_PIN_D3       (gpio_num_t)42

FilesystemManager::FilesystemManager() {
}

FilesystemManager::~FilesystemManager() {
    unmount_sd();

}

esp_err_t FilesystemManager::init() {
    if (m_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    

    // Try to mount SD card (primary filesystem for our use case)
    esp_err_t ret = mount_sd();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "File transfer protocol will not work without SD card");
        // Don't return error - allow system to continue
    } else {
        ESP_LOGI(TAG, "SD card mounted successfully");
    }
    
    m_initialized = true;
    return ESP_OK;  // Always return OK - we can work with just SD card
}



esp_err_t FilesystemManager::mount_sd() {
    if (m_sd_mounted) {
        return ESP_OK;
    }
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };
    
    // Initialize SD power control (ESP32-P4 requires LDO)
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD power ctrl failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure SDMMC host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40 MHz for 2x faster model loading
    host.pwr_ctrl_handle = pwr_ctrl_handle;
    
    // Configure slot (Waveshare ESP32-P4-WIFI6)
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.d1 = SD_PIN_D1;
    slot_config.d2 = SD_PIN_D2;
    slot_config.d3 = SD_PIN_D3;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT_SD, &host, 
                                   &slot_config, &mount_config, &m_sd_card);
    
    if (ret != ESP_OK) {
        sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle);
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Log SD card speed for verification
    if (m_sd_card) {
        uint32_t actual_freq_khz = m_sd_card->max_freq_khz;
        ESP_LOGI(TAG, "SD card mounted successfully");
        ESP_LOGI(TAG, "  Name: %s", m_sd_card->cid.name);
        ESP_LOGI(TAG, "  Frequency: %u kHz (%u MHz)", actual_freq_khz, actual_freq_khz / 1000);
        ESP_LOGI(TAG, "  Capacity: %llu MB", ((uint64_t)m_sd_card->csd.capacity * m_sd_card->csd.sector_size) / (1024 * 1024));
    } else {
        ESP_LOGI(TAG, "SD card mounted successfully");
    }
    
    m_sd_pwr_ctrl = pwr_ctrl_handle;
    m_sd_mounted = true;
    return ESP_OK;
}

void FilesystemManager::unmount_sd() {
    if (!m_sd_mounted) {
        return;
    }
    
    ESP_LOGI(TAG, "Unmounting SD card...");
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT_SD, m_sd_card);
    m_sd_card = nullptr;
    
    // Release SD power control
    if (m_sd_pwr_ctrl) {
        sd_pwr_ctrl_del_on_chip_ldo((sd_pwr_ctrl_handle_t)m_sd_pwr_ctrl);
        m_sd_pwr_ctrl = nullptr;
    }
    
    m_sd_mounted = false;
}

esp_err_t FilesystemManager::get_space_info(const char* path, uint64_t* total_bytes, uint64_t* free_bytes) {
    if (!path || !total_bytes || !free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!m_sd_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    
    FATFS* fs;
    DWORD fre_clust;
    
    // Get free clusters - use drive 0 for first mounted volume
    FRESULT res = f_getfree("0:", &fre_clust, &fs);
    if (res == FR_OK) {
        uint64_t tot_sect = (fs->n_fatent - 2) * fs->csize;
        uint64_t fre_sect = fre_clust * fs->csize;
        
        *total_bytes = tot_sect * fs->ssize;
        *free_bytes = fre_sect * fs->ssize;
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "f_getfree failed: %d", res);
    return ESP_FAIL;
}

esp_err_t FilesystemManager::list_directory(const char* path,
                                            void (*callback)(const char*, uint64_t, bool, uint32_t, void*),
                                            void* user_data) {
    if (!path || !callback) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Listing directory: %s", path);
    
    DIR* dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Build full path
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            callback(entry->d_name, st.st_size, S_ISDIR(st.st_mode), 
                    st.st_mtime, user_data);
        }
    }
    
    closedir(dir);
    return ESP_OK;
}

esp_err_t FilesystemManager::stat_file(const char* path, uint64_t* size, 
                                       uint32_t* timestamp, bool* is_dir) {
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (size) *size = st.st_size;
    if (timestamp) *timestamp = st.st_mtime;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    
    return ESP_OK;
}

esp_err_t FilesystemManager::delete_file(const char* path) {
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Deleting: %s", path);
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (S_ISDIR(st.st_mode)) {
        // ✅ FIX: Recursive delete for directories
        return delete_directory_recursive(path);
    } else {
        if (unlink(path) != 0) {
            return ESP_FAIL;
        }
    }
    
    return ESP_OK;
}

esp_err_t FilesystemManager::delete_directory_recursive(const char* path) {
    /**
     * Recursively delete directory and all contents
     * This is necessary because rmdir() only works on empty directories
     */
    DIR* dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    struct dirent* entry;
    esp_err_t result = ESP_OK;
    
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Build full path
        char full_path[512];
        int len = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (len >= (int)sizeof(full_path)) {
            ESP_LOGE(TAG, "Path too long: %s/%s", path, entry->d_name);
            result = ESP_ERR_INVALID_SIZE;
            continue;
        }
        
        // Check if directory or file
        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Recursive delete subdirectory
                esp_err_t ret = delete_directory_recursive(full_path);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to delete subdirectory: %s", full_path);
                    result = ret;
                }
            } else {
                // Delete file
                if (unlink(full_path) != 0) {
                    ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
                    result = ESP_FAIL;
                }
            }
        }
    }
    
    closedir(dir);
    
    // Now directory should be empty - delete it
    if (rmdir(path) != 0) {
        ESP_LOGE(TAG, "Failed to remove directory: %s", path);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Successfully deleted directory: %s", path);
    return result;
}

esp_err_t FilesystemManager::rename_file(const char* old_path, const char* new_path) {
    if (!old_path || !new_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Renaming: %s -> %s", old_path, new_path);
    
    if (rename(old_path, new_path) != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t FilesystemManager::create_directory(const char* path) {
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Creating directory: %s", path);
    
    if (mkdir(path, 0755) != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t FilesystemManager::copy_file(const char* src_path, const char* dst_path) {
    if (!src_path || !dst_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Copying: %s -> %s", src_path, dst_path);
    
    FILE* src = fopen(src_path, "rb");
    if (!src) {
        ESP_LOGE(TAG, "Failed to open source file");
        return ESP_ERR_NOT_FOUND;
    }
    
    FILE* dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        ESP_LOGE(TAG, "Failed to create destination file");
        return ESP_FAIL;
    }
    
    uint8_t buffer[4096];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        size_t bytes_written = fwrite(buffer, 1, bytes_read, dst);
        if (bytes_written != bytes_read) {
            fclose(src);
            fclose(dst);
            unlink(dst_path);
            return ESP_FAIL;
        }
    }
    
    fclose(src);
    fclose(dst);
    
    return ESP_OK;
}

esp_err_t FilesystemManager::hash_file(const char* path, uint32_t* hash) {
    if (!path || !hash) {
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buffer[4096];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        for (size_t i = 0; i < bytes_read; i++) {
            crc ^= buffer[i];
            for (int j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
    }
    
    fclose(f);
    *hash = ~crc;
    
    return ESP_OK;
}



esp_err_t FilesystemManager::format_sd() {
    if (!m_sd_card || !m_sd_mounted) {
        ESP_LOGE(TAG, "No SD card available to format");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGW(TAG, "Formatting SD card...");
    
    // esp_vfs_fat_sdcard_format requires the card to be initialized
    // In IDF v5/6, it will unmount, format, and remount internally if needed.
    // Or it formats it while mounted (it handles the logic).
    esp_err_t ret = esp_vfs_fat_sdcard_format(MOUNT_POINT_SD, m_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD format failed: %s", esp_err_to_name(ret));
        // Remount just in case it got stuck in an unmounted state
        mount_sd();
        return ret;
    }
    
    ESP_LOGI(TAG, "SD card formatted successfully");
    return ESP_OK;
}



bool FilesystemManager::is_sd_path(const char* path) {
    return strncmp(path, MOUNT_POINT_SD, strlen(MOUNT_POINT_SD)) == 0;
}

esp_err_t FilesystemManager::validate_path(const char* path) {
    if (!path || strlen(path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!is_sd_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}
