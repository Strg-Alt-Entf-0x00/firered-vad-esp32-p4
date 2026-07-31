/**
 * @file config.h
 * @brief FireVAD Console Configuration Constants
 * 
 * Centralized configuration for audio requirements, processing parameters,
 * and system defaults. Makes the codebase more maintainable and easier to
 * understand.
 */

#ifndef FIREVAD_CONSOLE_CONFIG_H
#define FIREVAD_CONSOLE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// FireRedVAD Audio Requirements
// ============================================================================
// These are FIXED requirements from the FireRedVAD model specification.
// Audio files must match these parameters exactly.

#define FIREVAD_SAMPLE_RATE         16000  ///< Required sample rate (Hz)
#define FIREVAD_BITS_PER_SAMPLE     16     ///< Required bit depth
#define FIREVAD_NUM_CHANNELS        1      ///< Required channels (mono)
#define FIREVAD_AUDIO_FORMAT_PCM    1      ///< Required format (PCM)
#define FIREVAD_BYTES_PER_SAMPLE    2      ///< 16-bit = 2 bytes

// ============================================================================
// Processing Constants
// ============================================================================

#define FRAME_SIZE_SAMPLES          160    ///< Frame size: 10ms @ 16kHz
#define FRAME_SIZE_MS               10     ///< Frame duration in milliseconds
#define FRAME_SIZE_BYTES            (FRAME_SIZE_SAMPLES * FIREVAD_BYTES_PER_SAMPLE)

#define FEATURES_PER_FRAME          80     ///< Log-mel features per frame
#define FEATURE_SIZE_BYTES          (FEATURES_PER_FRAME * sizeof(float))

#define FRAMES_PER_SECOND           100    ///< 16000 Hz / 160 samples = 100 fps
#define FRAMES_PER_CHUNK_OFFLINE    100    ///< 1 second chunks for offline VAD
#define SAMPLES_PER_CHUNK_OFFLINE   (FRAMES_PER_CHUNK_OFFLINE * FRAME_SIZE_SAMPLES)

// ============================================================================
// Default Settings
// ============================================================================

#define DEFAULT_VAD_THRESHOLD       0.6f   ///< Default speech detection threshold
#define DEFAULT_SW_GAIN             1.0f   ///< Default software gain multiplier

#define MIN_VAD_THRESHOLD           0.0f   ///< Minimum allowed threshold
#define MAX_VAD_THRESHOLD           1.0f   ///< Maximum allowed threshold

#define MIN_SW_GAIN                 0.1f   ///< Minimum software gain
#define MAX_SW_GAIN                 10.0f  ///< Maximum software gain

// ============================================================================
// File System Configuration
// ============================================================================

#define SPIFFS_MOUNT_POINT          "/spiffs"         ///< SPIFFS mount path
#define MODEL_FILE_EXTENSION        ".frvd"           ///< Model file extension
#define WAV_FILE_EXTENSION          ".wav"            ///< WAV file extension

#define MAX_FILE_PATH               300    ///< Maximum file path length
#define MAX_FILENAME                64     ///< Maximum filename length

// ============================================================================
// Memory and Safety Limits
// ============================================================================

#define MAX_AUDIO_DURATION_SEC      300    ///< Max audio length: 5 minutes
#define MAX_AUDIO_SAMPLES           (MAX_AUDIO_DURATION_SEC * FIREVAD_SAMPLE_RATE)
#define MAX_AUDIO_BYTES             (MAX_AUDIO_SAMPLES * FIREVAD_BYTES_PER_SAMPLE)

#define MAX_WAV_FILE_SIZE           (100 * 1024 * 1024)  ///< 100MB safety limit
#define MAX_MODEL_FILE_SIZE         (10 * 1024 * 1024)   ///< 10MB model limit

// ============================================================================
// Console Configuration
// ============================================================================

#define CONSOLE_MAX_COMMAND_LINE    256    ///< Maximum console input length
#define CONSOLE_PROMPT              "firevad> "  ///< Console prompt string
#define CONSOLE_HISTORY_SIZE        30     ///< Number of commands to remember

// ============================================================================
// Audio Input Configuration (when enabled)
// ============================================================================

#ifdef CONFIG_FIREVAD_ENABLE_AUDIO

#define AUDIO_BUFFER_SAMPLES        (FIREVAD_SAMPLE_RATE * 2)  ///< 2-second buffer
#define AUDIO_BUFFER_BYTES          (AUDIO_BUFFER_SAMPLES * FIREVAD_BYTES_PER_SAMPLE)

#define AUDIO_MIN_GAIN              0      ///< Minimum hardware gain
#define AUDIO_MAX_GAIN              11     ///< Maximum hardware gain (0-24dB)
#define AUDIO_DEFAULT_GAIN          CONFIG_FIREVAD_MIC_GAIN

#endif // CONFIG_FIREVAD_ENABLE_AUDIO

#ifdef __cplusplus
}
#endif

#endif // FIREVAD_CONSOLE_CONFIG_H
