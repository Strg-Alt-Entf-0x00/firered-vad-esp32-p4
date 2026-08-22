/*
 * ESP32-P4 USB Bridge File Manager - Protocol Definitions
 * 
 * Binary protocol for reliable file transfer over USB CDC
 * Version: 1.0.0
 */

#pragma once

#include <cstdint>
#include <cstring>

// ============================================================================
// Protocol Constants
// ============================================================================

#define PROTO_VERSION_MAJOR     1
#define PROTO_VERSION_MINOR     0
#define PROTO_VERSION_PATCH     0

#define PROTO_MAGIC_0          0xF1
#define PROTO_MAGIC_1          0x1E

#define PROTO_MAX_PAYLOAD      32768  // Industrial-grade maximum payload for fast transfers
#define PROTO_FILE_CHUNK_SIZE  8192  // Optimized chunk size (8KB) for maximum throughput
#define PROTO_HEADER_SIZE      9  // FIX: 2+1+1+1+2+2=9 bytes (was incorrectly 8)
#define PROTO_CRC_SIZE         4

// ============================================================================
// Frame Structure
// ============================================================================
// [MAGIC_0] [MAGIC_1] [VERSION] [CMD] [FLAGS] [SEQ_LSB] [SEQ_MSB] [LEN_LSB] [LEN_MSB] 
// [RESERVED] [PAYLOAD...] [CRC32]

#pragma pack(push, 1)

struct ProtocolHeader {
    uint8_t  magic[2];      // 0xF1, 0x1E
    uint8_t  version;       // Protocol version
    uint8_t  cmd;           // Command ID
    uint8_t  flags;         // Command flags
    uint16_t sequence;      // Packet sequence number
    uint16_t length;        // Payload length (little endian)
};

// Device Information Response
struct DeviceInfo {
    char     device_name[32];       // "ESP32-P4-FILEBRIDGE"
    uint8_t  fw_version_major;
    uint8_t  fw_version_minor;
    uint8_t  fw_version_patch;
    uint8_t  hw_revision;           // Board revision
    uint32_t flash_size;            // Internal flash size (bytes)
    uint32_t flash_free;            // Free flash space (bytes)
    uint32_t psram_size;            // PSRAM size (bytes)
    uint32_t psram_free;            // Free PSRAM (bytes)
    uint8_t  sd_present;            // 0 = no SD, 1 = SD present
    uint64_t sd_size;               // SD card size (bytes)
    uint64_t sd_free;               // SD card free space (bytes)
    uint8_t  sd_type;               // 0=none, 1=MMC, 2=SDSC, 3=SDHC, 4=SDXC
    char     sd_fs_type[16];        // "FAT32", "exFAT", or "NONE"
    
    // Dynamic Capabilities (Negotiation)
    uint32_t max_payload_size;      // Max bytes the device can receive in one payload
    uint32_t optimal_chunk_size;    // Optimal chunk size for maximum SD card performance
};

// File/Directory Entry
struct FileEntry {
    char     name[256];             // File or directory name
    uint64_t size;                  // File size in bytes (0 for directories)
    uint32_t timestamp;             // Unix timestamp
    uint8_t  is_directory;          // 0 = file, 1 = directory
    uint8_t  attributes;            // Read-only, hidden, etc.
    uint8_t  reserved[2];
};

// File Statistics
struct FileStat {
    uint64_t size;                  // File size
    uint32_t timestamp;             // Last modified time
    uint8_t  is_directory;
    uint8_t  attributes;
    uint8_t  reserved[2];
};

// Filesystem Space Info
struct SpaceInfo {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
};

// Transfer Progress
struct TransferProgress {
    uint64_t bytes_transferred;
    uint64_t total_bytes;
    uint16_t percent;               // 0-10000 (0.01% resolution)
    uint8_t  reserved[2];
};

#pragma pack(pop)

// ============================================================================
// Command IDs
// ============================================================================

enum CommandId : uint8_t {
    // System Commands
    CMD_HELLO           = 0x01,     // Handshake / keepalive
    CMD_DEVICE_INFO     = 0x02,     // Get device information
    CMD_REBOOT          = 0x03,     // Reboot ESP32
    
    // Response Commands
    CMD_ACK             = 0x10,     // Acknowledge / success
    CMD_NACK            = 0x11,     // Error / failure
    CMD_PROGRESS        = 0x12,     // Transfer progress update
    
    // Directory/File Listing
    CMD_LIST_BEGIN      = 0x20,     // List directory (request)
    CMD_LIST_ENTRY      = 0x21,     // Directory entry (response)
    CMD_LIST_END        = 0x22,     // End of directory listing
    
    // File Operations
    CMD_STAT            = 0x30,     // Get file statistics
    CMD_GET_FILE_BEGIN  = 0x31,     // Start download
    CMD_GET_FILE_DATA   = 0x32,     // File data chunk
    CMD_GET_FILE_END    = 0x33,     // End download
    
    CMD_PUT_FILE_BEGIN  = 0x34,     // Start upload
    CMD_PUT_FILE_DATA   = 0x35,     // Upload data chunk
    CMD_PUT_FILE_END    = 0x36,     // End upload
    CMD_PUT_FILE_ABORT  = 0x37,     // Abort upload
    
    CMD_DELETE          = 0x40,     // Delete file/directory
    CMD_RENAME          = 0x41,     // Rename/move file
    CMD_MKDIR           = 0x42,     // Create directory
    CMD_COPY            = 0x43,     // Copy file
    
    // Filesystem Commands
    CMD_SPACE_INFO      = 0x50,     // Get filesystem space info
    CMD_HASH_FILE       = 0x51,     // Calculate file hash (CRC32)
    
    // Advanced Features
    CMD_FORMAT_FS       = 0x60,     // Format filesystem (dangerous!)
    CMD_MOUNT           = 0x61,     // Mount filesystem
    CMD_UNMOUNT         = 0x62,     // Unmount filesystem
};

// ============================================================================
// Command Flags
// ============================================================================

enum CommandFlags : uint8_t {
    FLAG_NONE           = 0x00,
    FLAG_COMPRESSED     = 0x01,     // Payload is compressed
    FLAG_ENCRYPTED      = 0x02,     // Payload is encrypted
    FLAG_CONTINUATION   = 0x04,     // More packets follow
    FLAG_REQUIRES_ACK   = 0x08,     // Sender expects ACK
    FLAG_URGENT         = 0x10,     // High priority
};

// ============================================================================
// Error Codes
// ============================================================================

enum ErrorCode : uint8_t {
    ERR_NONE                = 0x00,
    ERR_INVALID_MAGIC       = 0x01,
    ERR_INVALID_CRC         = 0x02,
    ERR_INVALID_VERSION     = 0x03,
    ERR_UNKNOWN_CMD         = 0x04,
    ERR_PAYLOAD_TOO_LARGE   = 0x05,
    ERR_OUT_OF_MEMORY       = 0x06,
    ERR_TIMEOUT             = 0x07,
    
    // Filesystem Errors
    ERR_FS_NOT_MOUNTED      = 0x10,
    ERR_FS_MOUNT_FAILED     = 0x11,
    ERR_FILE_NOT_FOUND      = 0x12,
    ERR_FILE_EXISTS         = 0x13,
    ERR_DIR_NOT_EMPTY       = 0x14,
    ERR_DISK_FULL           = 0x15,
    ERR_READ_ONLY           = 0x16,
    ERR_INVALID_PATH        = 0x17,
    ERR_PATH_TOO_LONG       = 0x18,
    ERR_IO_ERROR            = 0x19,
    
    // Transfer Errors
    ERR_TRANSFER_ACTIVE     = 0x20,
    ERR_NO_TRANSFER         = 0x21,
    ERR_TRANSFER_ABORTED    = 0x22,
    ERR_SEQUENCE_ERROR      = 0x23,
    
    // Permission Errors
    ERR_ACCESS_DENIED       = 0x30,
    ERR_OPERATION_NOT_PERMITTED = 0x31,
};

// ============================================================================
// Filesystem Types
// ============================================================================

enum FilesystemType : uint8_t {
    FSTYPE_NONE             = 0,

    FSTYPE_FAT32            = 2,        // SD card FAT32
    FSTYPE_EXFAT            = 3,        // SD card exFAT
};

// ============================================================================
// Utility Functions
// ============================================================================

// CRC32 calculation (IEEE 802.3 polynomial)
inline uint32_t crc32_calculate(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    
    return ~crc;
}

// Verify frame CRC
inline bool verify_frame_crc(const uint8_t* frame, size_t frame_len) {
    if (frame_len < PROTO_CRC_SIZE) return false;
    
    uint32_t received_crc = *reinterpret_cast<const uint32_t*>(frame + frame_len - PROTO_CRC_SIZE);
    uint32_t calculated_crc = crc32_calculate(frame, frame_len - PROTO_CRC_SIZE);
    
    return received_crc == calculated_crc;
}

// Add CRC to frame
inline void add_frame_crc(uint8_t* frame, size_t frame_len) {
    uint32_t crc = crc32_calculate(frame, frame_len - PROTO_CRC_SIZE);
    *reinterpret_cast<uint32_t*>(frame + frame_len - PROTO_CRC_SIZE) = crc;
}
