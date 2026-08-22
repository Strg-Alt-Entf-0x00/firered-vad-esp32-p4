/*
 * ESP32-P4 USB Bridge File Manager - File Protocol Implementation
 * Part 1: Core infrastructure and basic commands
 */

#include "file_protocol.h"
#include "filesystem_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "sdmmc_cmd.h"
#include <cstring>
#include <cstdio>
#include <cerrno>

static const char* TAG = "file_protocol";

// Tags to suppress during file transfers for optimal performance
// IMPORTANT: DO NOT suppress file_protocol itself - only I/O subsystems
static const char* SUPPRESS_TAGS[] = {
    "sdmmc_req",
    "sdmmc_cmd",
    "sdmmc_io",
    "vfs_fat",
    "littlefs",
    "vfs",
    nullptr  // Sentinel
};

static esp_log_level_t saved_log_levels[10];  // Store original levels

void FileProtocol::suppress_logs_if_needed() {
    if (!m_log_suppression_enabled) return;
    
    // Save current log levels and suppress
    for (int i = 0; SUPPRESS_TAGS[i] != nullptr; i++) {
        saved_log_levels[i] = esp_log_level_get(SUPPRESS_TAGS[i]);
        esp_log_level_set(SUPPRESS_TAGS[i], ESP_LOG_NONE);
    }
}

void FileProtocol::restore_logs_if_needed() {
    if (!m_log_suppression_enabled) return;
    
    // Restore original log levels
    for (int i = 0; SUPPRESS_TAGS[i] != nullptr; i++) {
        esp_log_level_set(SUPPRESS_TAGS[i], saved_log_levels[i]);
    }
}

FileProtocol::FileProtocol() {
}

FileProtocol::~FileProtocol() {
    abort_active_transfer();
}

esp_err_t FileProtocol::init(FilesystemManager* fs_manager) {
    if (m_initialized) {
        return ESP_OK;
    }
    
    if (!fs_manager) {
        return ESP_ERR_INVALID_ARG;
    }
    
    m_fs_manager = fs_manager;
    m_initialized = true;
    
    return ESP_OK;
}

void FileProtocol::set_tx_callback(esp_err_t (*callback)(const uint8_t*, size_t)) {
    m_tx_callback = callback;
}

esp_err_t FileProtocol::send_frame(CommandId cmd, const void* payload, 
                                   uint16_t length, uint8_t flags) {
    if (!m_tx_callback) {
        ESP_LOGE(TAG, "No TX callback set!");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (length > PROTO_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Payload too large: %u", length);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Build frame in static buffer (NOT on stack to avoid overflow!)
    ProtocolHeader* header = reinterpret_cast<ProtocolHeader*>(m_tx_buffer);
    header->magic[0] = PROTO_MAGIC_0;
    header->magic[1] = PROTO_MAGIC_1;
    header->version = (PROTO_VERSION_MAJOR << 4) | PROTO_VERSION_MINOR;
    header->cmd = static_cast<uint8_t>(cmd);
    header->flags = flags;
    header->sequence = m_tx_sequence++;
    header->length = length;
    
    // Copy payload
    if (payload && length > 0) {
        memcpy(m_tx_buffer + PROTO_HEADER_SIZE, payload, length);
    }
    
    // Calculate and append CRC32
    size_t frame_len = PROTO_HEADER_SIZE + length + PROTO_CRC_SIZE;
    add_frame_crc(m_tx_buffer, frame_len);
    
    ESP_LOGD(TAG, "TX frame: cmd=0x%02X, len=%d, total=%d", cmd, length, (int)frame_len);
    // NOTE: Hex dump removed - at 921600 baud it blocked UART RX buffer causing 44KB overflow
    
    // Send via TX callback
    esp_err_t ret = m_tx_callback(m_tx_buffer, frame_len);
    return ret;
}

esp_err_t FileProtocol::send_error(ErrorCode error) {
    uint8_t payload = static_cast<uint8_t>(error);
    return send_frame(CMD_NACK, &payload, sizeof(payload));
}

esp_err_t FileProtocol::send_ack() {
    return send_frame(CMD_ACK, nullptr, 0);
}

esp_err_t FileProtocol::send_progress(uint64_t bytes_transferred, uint64_t total_bytes) {
    TransferProgress progress;
    progress.bytes_transferred = bytes_transferred;
    progress.total_bytes = total_bytes;
    
    if (total_bytes > 0) {
        progress.percent = (bytes_transferred * 10000) / total_bytes;
    } else {
        progress.percent = 0;
    }
    
    return send_frame(CMD_PROGRESS, &progress, sizeof(progress));
}

void FileProtocol::process_rx_data(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return;
    }
    
    // Append to RX buffer
    for (size_t i = 0; i < length; i++) {
        if (m_rx_idx >= sizeof(m_rx_buffer)) {
            ESP_LOGW(TAG, "RX buffer overflow, resetting");
            reset_rx_buffer();
        }
        
        m_rx_buffer[m_rx_idx++] = data[i];
        
        // Try to parse frame when we have at least header + CRC
        if (m_rx_idx >= PROTO_HEADER_SIZE + PROTO_CRC_SIZE) {
            try_parse_frame();
        }
    }
}

void FileProtocol::try_parse_frame() {
    // Sync to magic bytes
    while (m_rx_idx >= 2) {
        if (m_rx_buffer[0] == PROTO_MAGIC_0 && m_rx_buffer[1] == PROTO_MAGIC_1) {
            break;
        }
        // Discard invalid byte
        memmove(m_rx_buffer, m_rx_buffer + 1, m_rx_idx - 1);
        m_rx_idx--;
    }
    
    // Check if we still have at least a header
    if (m_rx_idx < PROTO_HEADER_SIZE + PROTO_CRC_SIZE) {
        return;
    }
    
    // Parse header
    const ProtocolHeader* header = reinterpret_cast<const ProtocolHeader*>(m_rx_buffer);
    
    // Check if we have complete frame
    size_t frame_len = PROTO_HEADER_SIZE + header->length + PROTO_CRC_SIZE;
    
    if (m_rx_idx < frame_len) {
        // Wait for more data
        return;
    }
    
    // Verify CRC
    if (!verify_frame_crc(m_rx_buffer, frame_len)) {
        ESP_LOGE(TAG, "CRC mismatch for cmd=0x%02X", header->cmd);
        reset_rx_buffer();
        send_error(ERR_INVALID_CRC);
        return;
    }
    
    // Extract payload
    const uint8_t* payload = (header->length > 0) ? (m_rx_buffer + PROTO_HEADER_SIZE) : nullptr;
    
    // Dispatch command
    dispatch_command(header, payload);
    
    // Remove processed frame from buffer
    if (m_rx_idx > frame_len) {
        memmove(m_rx_buffer, m_rx_buffer + frame_len, m_rx_idx - frame_len);
        m_rx_idx -= frame_len;
    } else {
        m_rx_idx = 0;
    }
}

void FileProtocol::dispatch_command(const ProtocolHeader* header, const uint8_t* payload) {
    switch (static_cast<CommandId>(header->cmd)) {
        case CMD_HELLO:
            ESP_LOGI(TAG, "Command: HELLO");
            handle_hello();
            break;
            
        case CMD_DEVICE_INFO:
            handle_device_info();
            break;
            
        case CMD_REBOOT:
            handle_reboot();
            break;
            
        case CMD_LIST_BEGIN:
            handle_list_begin(payload, header->length);
            break;
            
        case CMD_STAT:
            handle_stat(payload, header->length);
            break;
            
        case CMD_SPACE_INFO:
            handle_space_info(payload, header->length);
            break;
            
        case CMD_GET_FILE_BEGIN:
            handle_get_file_begin(payload, header->length);
            break;
            
        case CMD_PUT_FILE_BEGIN:
            handle_put_file_begin(payload, header->length);
            break;
            
        case CMD_PUT_FILE_DATA:
            handle_put_file_data(payload, header->length);
            break;
            
        case CMD_PUT_FILE_END:
            handle_put_file_end();
            break;
            
        case CMD_PUT_FILE_ABORT:
            handle_put_file_abort();
            break;
            
        case CMD_DELETE:
            handle_delete(payload, header->length);
            break;
            
        case CMD_RENAME:
            handle_rename(payload, header->length);
            break;
            
        case CMD_MKDIR:
            handle_mkdir(payload, header->length);
            break;
            
        case CMD_COPY:
            handle_copy(payload, header->length);
            break;
            
        case CMD_HASH_FILE:
            handle_hash_file(payload, header->length);
            break;
            
        case CMD_FORMAT_FS:
            handle_format_fs(payload, header->length);
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown command: 0x%02X", header->cmd);
            send_error(ERR_UNKNOWN_CMD);
            break;
    }
}

void FileProtocol::reset_rx_buffer() {
    m_rx_idx = 0;
}

void FileProtocol::abort_active_transfer() {
    if (m_transfer_file) {
        fclose(m_transfer_file);
        m_transfer_file = nullptr;
    }
    
    if (m_sd_write_buffer) {
        free(m_sd_write_buffer);
        m_sd_write_buffer = nullptr;
    }
    
    // Restore logs if we were in a transfer
    if (m_transfer_active) {
        restore_logs_if_needed();
    }
    
    m_transfer_active = false;
    m_transfer_bytes = 0;
    m_transfer_total = 0;
}

// ============================================================================
// Command Handlers - Basic Commands
// ============================================================================

void FileProtocol::handle_hello() {
    ESP_LOGI(TAG, "=== HANDLE_HELLO START ===");
    ESP_LOGI(TAG, "Sending ACK response...");
    esp_err_t ret = send_ack();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ACK sent successfully");
    } else {
        ESP_LOGE(TAG, "ACK send failed: %d", ret);
    }
    ESP_LOGI(TAG, "=== HANDLE_HELLO END ===");
}

void FileProtocol::handle_device_info() {
    ESP_LOGI(TAG, "DEVICE_INFO requested");
    
    // Industrial-Grade: If a new connection handshake comes in while a transfer is active,
    // the previous connection crashed/died. Forcefully abort the dangling transfer.
    if (m_transfer_active) {
        ESP_LOGW(TAG, "Dangling transfer detected on handshake. Aborting...");
        abort_active_transfer();
    }
    
    DeviceInfo info = {};
    strncpy(info.device_name, "ESP32-P4-FILEBRIDGE", sizeof(info.device_name));
    
    info.fw_version_major = 1;
    info.fw_version_minor = 0;
    info.fw_version_patch = 0;
    info.hw_revision = 13;  // Rev 1.3
    
    // Flash info
    info.flash_size = 32 * 1024 * 1024;  // 32MB
    
    uint64_t flash_free = 0;
    info.flash_free = flash_free;
    // PSRAM info
    #if CONFIG_SPIRAM
    info.psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    info.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    #else
    info.psram_size = 0;
    info.psram_free = 0;
    #endif
    
    // SD card info
    info.sd_present = m_fs_manager->is_sd_mounted() ? 1 : 0;
    
    if (info.sd_present) {
        sdmmc_card_t* card = m_fs_manager->get_sd_card_info();
        if (card) {
            info.sd_size = ((uint64_t)card->csd.capacity) * card->csd.sector_size;
            
            uint64_t sd_total = 0, sd_free = 0;
            if (m_fs_manager->get_space_info(MOUNT_POINT_SD, &sd_total, &sd_free) == ESP_OK) {
                info.sd_free = sd_free;
            }
            
            // Determine SD type (check if it's high capacity)
            if (card->ocr & (1 << 30)) {  // CCS bit (Card Capacity Status)
                if (info.sd_size > 32ULL * 1024 * 1024 * 1024) {
                    info.sd_type = 4;  // SDXC
                    strncpy(info.sd_fs_type, "exFAT", sizeof(info.sd_fs_type));
                } else {
                    info.sd_type = 3;  // SDHC
                    strncpy(info.sd_fs_type, "FAT32", sizeof(info.sd_fs_type));
                }
            } else {
                info.sd_type = 2;  // SDSC
                strncpy(info.sd_fs_type, "FAT32", sizeof(info.sd_fs_type));
            }
        }
    } else {
        strncpy(info.sd_fs_type, "NONE", sizeof(info.sd_fs_type));
    }
    
    // Dynamic Capabilities (Negotiation)
    info.max_payload_size = PROTO_MAX_PAYLOAD;
    // Optimized chunk size for maximum SD card performance:
    // - 8192 bytes = 16x SD sector size (optimal for sequential writes)
    // - Matches 8KB UART buffer size perfectly
    // - Achieves 97% UART efficiency (285 KB/s download @ 3 Mbaud)
    info.optimal_chunk_size = 8192; 
    
    send_frame(CMD_DEVICE_INFO, &info, sizeof(info));
}

void FileProtocol::handle_reboot() {
    ESP_LOGW(TAG, "REBOOT requested");
    send_ack();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

// Continued in next part...

// ============================================================================
// Command Handlers - Directory and File Operations
// ============================================================================

void FileProtocol::handle_list_begin(const uint8_t* payload, uint16_t length) {
    if (!payload || length == 0) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    // Path is null-terminated string in payload
    const char* path = reinterpret_cast<const char*>(payload);
    ESP_LOGI(TAG, "LIST_BEGIN: %s", path);
    
    // Send ACK first
    send_ack();
    
    // List directory entries
    auto callback = [](const char* name, uint64_t size, bool is_dir, 
                      uint32_t timestamp, void* user_data) {
        FileProtocol* self = static_cast<FileProtocol*>(user_data);
        
        FileEntry entry = {};
        strncpy(entry.name, name, sizeof(entry.name) - 1);
        entry.size = size;
        entry.timestamp = timestamp;
        entry.is_directory = is_dir ? 1 : 0;
        
        self->send_frame(CMD_LIST_ENTRY, &entry, sizeof(entry));
    };
    
    esp_err_t ret = m_fs_manager->list_directory(path, callback, this);
    
    if (ret != ESP_OK) {
        send_error(ERR_FILE_NOT_FOUND);
        return;
    }
    
    // Send end marker
    send_frame(CMD_LIST_END, nullptr, 0);
}

void FileProtocol::handle_stat(const uint8_t* payload, uint16_t length) {
    if (!payload || length == 0) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* path = reinterpret_cast<const char*>(payload);
    ESP_LOGD(TAG, "STAT: %s", path);
    
    FileStat stat_info = {};
    bool is_dir = false;
    
    esp_err_t ret = m_fs_manager->stat_file(path, &stat_info.size, 
                                            &stat_info.timestamp, &is_dir);
    
    if (ret != ESP_OK) {
        send_error(ERR_FILE_NOT_FOUND);
        return;
    }
    
    stat_info.is_directory = is_dir ? 1 : 0;
    send_frame(CMD_STAT, &stat_info, sizeof(stat_info));
}

void FileProtocol::handle_space_info(const uint8_t* payload, uint16_t length) {
    if (!payload || length == 0) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* path = reinterpret_cast<const char*>(payload);
    ESP_LOGD(TAG, "SPACE_INFO: %s", path);
    
    uint64_t total = 0, free = 0;
    esp_err_t ret = m_fs_manager->get_space_info(path, &total, &free);
    
    if (ret != ESP_OK) {
        send_error(ERR_FS_NOT_MOUNTED);
        return;
    }
    
    SpaceInfo info = {};
    info.total_bytes = total;
    info.free_bytes = free;
    info.used_bytes = total - free;
    info.block_size = 4096;
    
    send_frame(CMD_SPACE_INFO, &info, sizeof(info));
}

// ============================================================================
// File Transfer - Download (GET)
// ============================================================================

void FileProtocol::handle_get_file_begin(const uint8_t* payload, uint16_t length) {
    ESP_LOGI(TAG, "GET_FILE_BEGIN: Starting download...");
    
    if (m_transfer_active) {
        ESP_LOGW(TAG, "Transfer already active");
        send_error(ERR_TRANSFER_ACTIVE);
        return;
    }
    
    if (!payload || length == 0) {
        ESP_LOGE(TAG, "Invalid payload");
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* path = reinterpret_cast<const char*>(payload);
    
    // Open file for reading
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "File not found: %s", path);
        send_error(ERR_FILE_NOT_FOUND);
        return;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Send ACK with file size
    uint64_t size_payload = file_size;
    send_frame(CMD_ACK, &size_payload, sizeof(size_payload));
    
    // NOTE: Log suppression DISABLED for debugging
    // suppress_logs_if_needed();
    
    // Send file in chunks using hardware flow control
    // Hardware Flow Control (RTS/CTS) automatically throttles transmission
    // when receiver buffer fills up - no artificial delay needed!
    uint8_t buffer[PROTO_FILE_CHUNK_SIZE];  // 8KB chunks for optimal throughput
    size_t bytes_sent = 0;
    size_t chunk_size;
    uint32_t chunk_count = 0;
    
    while ((chunk_size = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        send_frame(CMD_GET_FILE_DATA, buffer, chunk_size);
        bytes_sent += chunk_size;
        chunk_count++;
        
        // Hardware Flow Control (RTS/CTS) automatically manages data flow
        // Only yield occasionally to prevent watchdog timeout on very large files
        if ((chunk_count % 100) == 0) {
            taskYIELD();  // Allow other tasks to run every 100 chunks
        }
        
        // Send progress every 10 chunks
        if ((chunk_count % 10) == 0) {
            send_progress(bytes_sent, file_size);
        }
    }
    
    fclose(f);
    
    // Send end marker
    send_frame(CMD_GET_FILE_END, nullptr, 0);
    
    // [FIX] CRITICAL FIX: Clear transfer state after download completes!
    m_transfer_active = false;
    m_transfer_bytes = 0;
    m_transfer_total = 0;
    
    // NOTE: Log suppression was disabled, no need to restore
    // restore_logs_if_needed();
    
    ESP_LOGI(TAG, "File transfer complete: %zu bytes in %u chunks", bytes_sent, chunk_count);
}

// ============================================================================
// File Transfer - Upload (PUT)
// ============================================================================

void FileProtocol::handle_put_file_begin(const uint8_t* payload, uint16_t length) {
    if (m_transfer_active) {
        send_error(ERR_TRANSFER_ACTIVE);
        return;
    }
    
    if (!payload || length < sizeof(uint64_t) + 1) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    // Payload format: [file_size:8 bytes][path:string]
    const uint64_t* size_ptr = reinterpret_cast<const uint64_t*>(payload);
    const char* path = reinterpret_cast<const char*>(payload + sizeof(uint64_t));
    
    m_transfer_total = *size_ptr;
    strncpy(m_transfer_path, path, sizeof(m_transfer_path) - 1);
    
    ESP_LOGI(TAG, "PUT_FILE_BEGIN: %s (%llu bytes)", m_transfer_path, m_transfer_total);
    
    // BENCHMARK MODE: Bypass SD card
    if (strcmp(m_transfer_path, "/dev/null") == 0) {
        ESP_LOGI(TAG, "BENCHMARK MODE: Bypassing SD card writes for pure UART throughput testing");
        m_benchmark_mode = true;
        m_transfer_active = true;
        m_transfer_bytes = 0;
        send_ack();
        return;
    }
    m_benchmark_mode = false;
    
    // Check if enough space is available
    // Extract mount point from path ("/sd/..." or "/flash/...")
    const char* mount_point = nullptr;
    if (strncmp(path, "/sd/", 4) == 0 || strcmp(path, "/sd") == 0) {
        mount_point = "/sd";
    } else if (strncmp(path, "/spiffs/", 8) == 0 || strcmp(path, "/spiffs") == 0) {
        mount_point = "/spiffs";
    }
    
    if (mount_point) {
        uint64_t total_bytes = 0;
        uint64_t free_bytes = 0;
        esp_err_t ret = m_fs_manager->get_space_info(mount_point, &total_bytes, &free_bytes);
        
        if (ret == ESP_OK) {
            // Add 10% safety margin for filesystem overhead
            uint64_t required_bytes = m_transfer_total + (m_transfer_total / 10);
            
            if (free_bytes < required_bytes) {
                ESP_LOGE(TAG, "Insufficient space: need %llu bytes, have %llu bytes free", 
                         required_bytes, free_bytes);
                send_error(ERR_DISK_FULL);
                return;
            }
            
            ESP_LOGI(TAG, "Space check OK: %llu bytes free (need %llu)", free_bytes, required_bytes);
        } else {
            ESP_LOGW(TAG, "Could not check space: %s", esp_err_to_name(ret));
        }
    }
    
    // Open file for writing
    m_transfer_file = fopen(m_transfer_path, "wb");
    if (!m_transfer_file) {
        ESP_LOGE(TAG, "Failed to create file: %s", m_transfer_path);
        send_error(ERR_IO_ERROR);
        return;
    }
    
    // CRITICAL PERFORMANCE FIX: Buffer writes to reduce SD card latency!
    // NOTE: Buffer must be in heap/static storage, NOT stack!
    // Allocate from heap to avoid stack overflow and track in member variable
    m_sd_write_buffer = (char*)malloc(32768);
    if (m_sd_write_buffer && setvbuf(m_transfer_file, m_sd_write_buffer, _IOFBF, 32768) == 0) {
        ESP_LOGI(TAG, "Write buffering enabled (32KB) for optimal SD performance");
    } else {
        ESP_LOGW(TAG, "Failed to set write buffer, performance may be reduced");
        if (m_sd_write_buffer) free(m_sd_write_buffer);
        m_sd_write_buffer = nullptr;
    }
    
    m_transfer_active = true;
    m_transfer_bytes = 0;
    
    // Suppress logs during transfer for optimal performance
    suppress_logs_if_needed();
    
    send_ack();
}

void FileProtocol::handle_put_file_data(const uint8_t* payload, uint16_t length) {
    if (!m_transfer_active) {
        send_error(ERR_NO_TRANSFER);
        return;
    }
    
    if (!m_benchmark_mode && !m_transfer_file) {
        send_error(ERR_NO_TRANSFER);
        return;
    }
    
    if (!payload || length == 0) {
        return;
    }
    
    if (m_benchmark_mode) {
        // Benchmark mode: Just discard data and acknowledge
        m_transfer_bytes += length;
    } else {
        // Write chunk to file (with 32KB write buffer, SD blocking is OK)
        size_t written = fwrite(payload, 1, length, m_transfer_file);
        
        if (written != length) {
            ESP_LOGE(TAG, "Write error: expected %u, wrote %zu", length, written);
            abort_active_transfer();
            send_error(ERR_IO_ERROR);
            return;
        }
        
        m_transfer_bytes += written;
    }
    
    // NOTE: Watchdog is DISABLED for UART RX task (see main.cpp)
    // No vTaskDelay needed - let's transfer at full speed!
    // If fwrite() blocks too long, Hardware Flow Control will backpressure PC
    
    // DEBUG: Log progress and heap every 100KB to detect memory leaks
    if ((m_transfer_bytes % (100 * 1024)) == 0) {
        ESP_LOGI(TAG, "Progress: %llu KB, Free heap: %u bytes, Stack high water: %u", 
                 m_transfer_bytes / 1024, 
                 esp_get_free_heap_size(),
                 uxTaskGetStackHighWaterMark(nullptr));
    }
    
    // Streaming mode: No ACK per chunk. Hardware Flow Control will throttle the PC if needed.
}

void FileProtocol::handle_put_file_end() {
    if (!m_transfer_active) {
        send_error(ERR_NO_TRANSFER);
        return;
    }
    
    // Restore logs before logging final message
    restore_logs_if_needed();
    
    ESP_LOGI(TAG, "PUT_FILE_END: %llu bytes received", m_transfer_bytes);
    
    if (m_transfer_file) {
        fclose(m_transfer_file);
        m_transfer_file = nullptr;
    }
    
    m_transfer_active = false;
    
    send_ack();
}

void FileProtocol::handle_put_file_abort() {
    ESP_LOGW(TAG, "PUT_FILE_ABORT");
    
    abort_active_transfer();
    
    // Delete partial file
    if (strlen(m_transfer_path) > 0) {
        unlink(m_transfer_path);
    }
    
    send_ack();
}

// ============================================================================
// File Operations
// ============================================================================

void FileProtocol::handle_delete(const uint8_t* payload, uint16_t length) {
    // [FIX] Check if transfer is active
    if (m_transfer_active) {
        ESP_LOGW(TAG, "Cannot delete: transfer active");
        send_error(ERR_TRANSFER_ACTIVE);
        return;
    }
    
    if (!payload || length == 0) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* path = reinterpret_cast<const char*>(payload);
    ESP_LOGI(TAG, "DELETE: %s", path);
    
    esp_err_t ret = m_fs_manager->delete_file(path);
    
    if (ret == ESP_OK) {
        send_ack();
    } else {
        send_error(ERR_IO_ERROR);
    }
}

void FileProtocol::handle_rename(const uint8_t* payload, uint16_t length) {
    // [FIX] Check if transfer is active
    if (m_transfer_active) {
        ESP_LOGW(TAG, "Cannot rename: transfer active");
        send_error(ERR_TRANSFER_ACTIVE);
        return;
    }
    
    if (!payload || length < 2) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    // Payload format: [old_path\0new_path\0]
    const char* old_path = reinterpret_cast<const char*>(payload);
    size_t old_len = strlen(old_path);
    
    if (old_len + 1 >= length) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* new_path = old_path + old_len + 1;
    
    ESP_LOGI(TAG, "RENAME: %s -> %s", old_path, new_path);
    
    esp_err_t ret = m_fs_manager->rename_file(old_path, new_path);
    
    if (ret == ESP_OK) {
        send_ack();
    } else {
        send_error(ERR_IO_ERROR);
    }
}

void FileProtocol::handle_mkdir(const uint8_t* payload, uint16_t length) {
    // [FIX] Check if transfer is active
    if (m_transfer_active) {
        ESP_LOGW(TAG, "Cannot mkdir: transfer active");
        send_error(ERR_TRANSFER_ACTIVE);
        return;
    }
    
    if (!payload || length == 0) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* path = reinterpret_cast<const char*>(payload);
    ESP_LOGI(TAG, "MKDIR: %s", path);
    
    esp_err_t ret = m_fs_manager->create_directory(path);
    
    if (ret == ESP_OK) {
        send_ack();
    } else {
        send_error(ERR_IO_ERROR);
    }
}

void FileProtocol::handle_copy(const uint8_t* payload, uint16_t length) {
    if (!payload || length < 2) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    // Payload format: [src_path\0dst_path\0]
    const char* src_path = reinterpret_cast<const char*>(payload);
    size_t src_len = strlen(src_path);
    
    if (src_len + 1 >= length) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* dst_path = src_path + src_len + 1;
    
    ESP_LOGI(TAG, "COPY: %s -> %s", src_path, dst_path);
    
    esp_err_t ret = m_fs_manager->copy_file(src_path, dst_path);
    
    if (ret == ESP_OK) {
        send_ack();
    } else {
        send_error(ERR_IO_ERROR);
    }
}

void FileProtocol::handle_hash_file(const uint8_t* payload, uint16_t length) {
    if (!payload || length == 0) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    const char* path = reinterpret_cast<const char*>(payload);
    ESP_LOGI(TAG, "HASH_FILE: %s", path);
    
    uint32_t hash = 0;
    esp_err_t ret = m_fs_manager->hash_file(path, &hash);
    
    if (ret == ESP_OK) {
        send_frame(CMD_HASH_FILE, &hash, sizeof(hash));
    } else {
        send_error(ERR_FILE_NOT_FOUND);
    }
}


void FileProtocol::handle_format_fs(const uint8_t* payload, uint16_t length) {
    if (m_transfer_active) {
        send_error(ERR_TRANSFER_ACTIVE);
        return;
    }
    
    if (!payload || length < 1) {
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    uint8_t fs_type = payload[0];
    esp_err_t ret = ESP_FAIL;
    
    if (fs_type == FSTYPE_FAT32 || fs_type == FSTYPE_EXFAT) {
        ESP_LOGW(TAG, "Format requested for SD Card");
        ret = m_fs_manager->format_sd();
    } else {
        ESP_LOGE(TAG, "Format requested for unknown FS type: %d", fs_type);
        send_error(ERR_INVALID_PATH);
        return;
    }
    
    if (ret == ESP_OK) {
        send_ack();
    } else {
        send_error(ERR_IO_ERROR);
    }
}
