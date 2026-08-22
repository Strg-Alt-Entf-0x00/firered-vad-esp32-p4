/*
 * ESP32-P4 USB Bridge File Manager - File Protocol Handler
 * 
 * Handles protocol frame parsing and command execution
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "esp_log.h"
#include "protocol_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Forward declarations
class FilesystemManager;

/**
 * RAII class for temporarily suppressing logs during critical operations
 * Industrial-grade approach: logs are disabled during transfers, re-enabled after
 */
class LogSuppressor {
public:
    LogSuppressor(const char* tag) : m_tag(tag) {
        if (m_tag) {
            m_original_level = esp_log_level_get(m_tag);
            esp_log_level_set(m_tag, ESP_LOG_NONE);
        }
    }
    
    ~LogSuppressor() {
        if (m_tag) {
            esp_log_level_set(m_tag, m_original_level);
        }
    }
    
private:
    const char* m_tag;
    esp_log_level_t m_original_level;
};

class FileProtocol {
public:
    FileProtocol();
    ~FileProtocol();

    // Non-copyable
    FileProtocol(const FileProtocol&) = delete;
    FileProtocol& operator=(const FileProtocol&) = delete;

    /**
     * Initialize protocol handler
     * @param fs_manager Pointer to filesystem manager instance
     * @return ESP_OK on success
     */
    esp_err_t init(FilesystemManager* fs_manager);

    /**
     * Process incoming data from USB CDC
     * @param data Incoming byte stream
     * @param length Number of bytes
     */
    void process_rx_data(const uint8_t* data, size_t length);

    /**
     * Send protocol frame to PC
     * @param cmd Command ID
     * @param payload Payload data (can be nullptr)
     * @param length Payload length
     * @param flags Command flags
     * @return ESP_OK on success
     */
    esp_err_t send_frame(CommandId cmd, const void* payload, uint16_t length, 
                         uint8_t flags = FLAG_NONE);

    /**
     * Send error response
     */
    esp_err_t send_error(ErrorCode error);

    /**
     * Send ACK response
     */
    esp_err_t send_ack();

    /**
     * Send transfer progress update
     */
    esp_err_t send_progress(uint64_t bytes_transferred, uint64_t total_bytes);

    /**
     * Set TX callback for sending data via USB CDC
     */
    void set_tx_callback(esp_err_t (*callback)(const uint8_t*, size_t));
    
    /**
     * Enable/disable dynamic log suppression during transfers
     * Professional approach: suppress logs only during active transfers
     */
    void set_log_suppression(bool enable) { m_log_suppression_enabled = enable; }

private:
    // State
    bool m_initialized = false;
    FilesystemManager* m_fs_manager = nullptr;
    bool m_log_suppression_enabled = true;  // Enabled by default for production
    
    // TX Callback
    esp_err_t (*m_tx_callback)(const uint8_t*, size_t) = nullptr;
    
    // RX Buffer for frame assembly
    uint8_t m_rx_buffer[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_CRC_SIZE];
    size_t m_rx_idx = 0;
    
    // TX Buffer for frame transmission (static to avoid stack overflow)
    uint8_t m_tx_buffer[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_CRC_SIZE];
    
    // Sequence counter
    uint16_t m_tx_sequence = 0;
    uint16_t m_rx_sequence = 0;
    
    // Transfer state
    bool m_transfer_active = false;
    FILE* m_transfer_file = nullptr;
    char* m_sd_write_buffer = nullptr;  // Heap-allocated buffer for setvbuf()
    uint64_t m_transfer_bytes = 0;
    uint64_t m_transfer_total = 0;
    char m_transfer_path[256];
    bool m_benchmark_mode = false;
    
    // Command handlers
    void handle_hello();
    void handle_device_info();
    void handle_reboot();
    
    void handle_list_begin(const uint8_t* payload, uint16_t length);
    void handle_stat(const uint8_t* payload, uint16_t length);
    void handle_space_info(const uint8_t* payload, uint16_t length);
    
    void handle_get_file_begin(const uint8_t* payload, uint16_t length);
    void handle_put_file_begin(const uint8_t* payload, uint16_t length);
    void handle_put_file_data(const uint8_t* payload, uint16_t length);
    void handle_put_file_end();
    void handle_put_file_abort();
    
    void handle_delete(const uint8_t* payload, uint16_t length);
    void handle_rename(const uint8_t* payload, uint16_t length);
    void handle_mkdir(const uint8_t* payload, uint16_t length);
    void handle_copy(const uint8_t* payload, uint16_t length);
    
    void handle_hash_file(const uint8_t* payload, uint16_t length);
    void handle_format_fs(const uint8_t* payload, uint16_t length);
    
    // Frame parsing
    void try_parse_frame();
    void dispatch_command(const ProtocolHeader* header, const uint8_t* payload);
    
    // Helper functions
    void reset_rx_buffer();
    void abort_active_transfer();
    
    // Helper to suppress logs during transfers
    void suppress_logs_if_needed();
    void restore_logs_if_needed();
};

