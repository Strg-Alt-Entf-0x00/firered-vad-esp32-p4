#include "cmd_vad_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/task.h"

#include "esp_sleep.h"
#include "esp_pm.h"

#include "config.h"
#include "audio_manager.h"
#include "vad_runner.h"
#include "metrics.h"
#include "benchmark.h"
#include "esp_timer.h"



static float g_threshold = DEFAULT_VAD_THRESHOLD;
static float g_sw_gain = DEFAULT_SW_GAIN;

#pragma pack(push, 1)
struct WavHeader {
    char riff_tag[4];
    uint32_t riff_length;
    char wave_tag[4];
    char fmt_tag[4];
    uint32_t fmt_length;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_tag[4];
    uint32_t data_length;
};
#pragma pack(pop)

static void write_wav_header(FILE* f, uint32_t data_size) {
    WavHeader header = {
        .riff_tag = {'R','I','F','F'},
        .riff_length = data_size + sizeof(WavHeader) - 8,
        .wave_tag = {'W','A','V','E'},
        .fmt_tag = {'f','m','t',' '},
        .fmt_length = 16,
        .audio_format = 1, // PCM
        .num_channels = FIREVAD_NUM_CHANNELS,
        .sample_rate = FIREVAD_SAMPLE_RATE,
        .byte_rate = FIREVAD_SAMPLE_RATE * FIREVAD_NUM_CHANNELS * FIREVAD_BYTES_PER_SAMPLE,
        .block_align = FIREVAD_NUM_CHANNELS * FIREVAD_BYTES_PER_SAMPLE,
        .bits_per_sample = FIREVAD_BITS_PER_SAMPLE,
        .data_tag = {'d','a','t','a'},
        .data_length = data_size
    };
    fseek(f, 0, SEEK_SET);
    fwrite(&header, 1, sizeof(header), f);
}

// ---------------------------------------------------------
// Existing Commands
// ---------------------------------------------------------

static int cmd_vad_model_load(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: vad_model_load <filename.frvd>\n");
        return 1;
    }
    return vad_runner_load_model(argv[1]);
}

static int cmd_vad_model_info(int argc, char **argv) {
    vad_runner_print_info();
    return 0;
}

static void list_directory(const char* base_path, int level) {
    if (level > 4) return; // Limit recursion depth
    
    DIR* dir = opendir(base_path);
    if (!dir) {
        if (level == 0) printf("Failed to open directory: %s\n", base_path);
        return;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        for (int i = 0; i < level; i++) printf("|   ");
        printf("|-- %s", ent->d_name);

        char path[MAX_FILE_PATH];
        snprintf(path, sizeof(path), "%s/%s", base_path, ent->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                printf("/\n");
                list_directory(path, level + 1);
            } else {
                if (st.st_size > 1024 * 1024) {
                    printf(" (%.1f MB)\n", (float)st.st_size / (1024.0f * 1024.0f));
                } else if (st.st_size > 1024) {
                    printf(" (%ld KB)\n", st.st_size / 1024);
                } else {
                    printf(" (%ld B)\n", st.st_size);
                }
            }
        } else {
            printf("\n");
        }
    }
    closedir(dir);
}

static int cmd_fs_ls(int argc, char **argv) {
    const char* path = FS_MOUNT_POINT; // Default to SD card root
    if (argc > 1) {
        path = argv[1];
    }
    
    printf("%s/\n", path);
    list_directory(path, 0);
    return 0;
}

static int cmd_vad_threshold(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: threshold <0.0-1.0>\nCurrent: %.2f\n", g_threshold);
        return (argc == 1) ? 0 : 1;
    }
    float val = atof(argv[1]);
    if (val < 0.0f || val > 1.0f) {
        printf("[ERROR] Threshold must be between 0.0 and 1.0\n");
        return 1;
    }
    g_threshold = val;
    printf("[OK] Threshold set to %.2f\n", g_threshold);
    return 0;
}

static int cmd_mic_gain(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        float hw_gain = 0;
        audio_manager_get_mic_gain(&hw_gain);
        printf("Current hardware gain: %.1f dB\n", hw_gain);
        return 0;
    }
    
    if (argc < 3) {
        printf("Usage:\n");
        printf("  mic_gain -s <multiplier>   : Set software gain (0.1-10.0)\n");
        printf("  mic_gain -h <gain_db>      : Set hardware mic gain (0.0-42.0 dB)\n");
        printf("  mic_gain -h                : Show current hardware gain\n\n");
        
        printf("Current software gain: %.2fx\n", g_sw_gain);
        
        float hw_gain = 0;
        if (audio_manager_get_mic_gain(&hw_gain) == ESP_OK) {
            printf("Current hardware gain: %.1f dB\n", hw_gain);
        }
        return 0;
    }
    
    if (strcmp(argv[1], "-s") == 0) {
        float mult = atof(argv[2]);
        if (mult < 0.1f || mult > 10.0f) {
            printf("[ERROR] Software gain multiplier must be between 0.1 and 10.0\n");
            return 1;
        }
        g_sw_gain = mult;
        printf("[OK] Software gain set to %.2fx\n", mult);
        return 0;
    }
    
    if (strcmp(argv[1], "-h") == 0) {
        if (!audio_manager_is_initialized()) {
            printf("[ERROR] Audio not initialized\n");
            return 1;
        }
        float db = atof(argv[2]);
        if (db < 0.0f || db > 42.0f) {
            printf("[ERROR] Hardware gain must be between 0.0 and 42.0 dB\n");
            return 1;
        }
        if (audio_manager_set_mic_gain(db) == ESP_OK) {
            printf("[OK] Hardware gain set to %.1f dB\n", db);
        }
        return 0;
    }
    return 1;
}

static int cmd_mic_select(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: mic_select <inmp441|es8311>\n");
        return 0;
    }
    
    if (strcmp(argv[1], "inmp441") == 0) {
        audio_manager_set_mic(MIC_I2S1_INMP441);
        printf("[OK] Selected INMP441 (I2S1) as active microphone.\n");
    } else if (strcmp(argv[1], "es8311") == 0) {
        audio_manager_set_mic(MIC_I2S0_ES8311);
        printf("[OK] Selected ES8311 (I2S0) as active microphone.\n");
    } else {
        printf("[ERROR] Invalid mic type. Use 'inmp441' or 'es8311'.\n");
        return 1;
    }
    return 0;
}

static int cmd_mic_info(int argc, char **argv) {
    mic_type_t mic = audio_manager_get_mic();
    float hw_gain = 0.0f;
    audio_manager_get_mic_gain(&hw_gain);
    
    // Using extern reference for software gain since it's in vad_runner.cpp, 
    // or just display it as unknown if we can't easily grab it here.
    // We'll just show the mic selection and hardware gain.
    printf("\n=== Microphone Info ===\n");
    if (mic == MIC_I2S1_INMP441) {
        printf("Active Mic    : INMP441 (Digital I2S)\n");
        printf("Interface     : I2S1\n");
        printf("Pins          : BCLK=20, WS=21, DIN=22\n");
        printf("Hardware Gain : N/A (Digital mic)\n");
    } else {
        printf("Active Mic    : ES8311 Onboard (Analog)\n");
        printf("Interface     : I2S0 (via Codec)\n");
        printf("Pins          : I2C(SCL=8, SDA=7), I2S(MCLK=13, BCLK=12, WS=10, DIN=11, DOUT=9)\n");
        printf("Hardware Gain : %.1f dB\n", hw_gain);
    }
    printf("=======================\n\n");
    return 0;
}

static int cmd_mic_level(int argc, char **argv) {
    printf("Measuring mic levels for 1 second...\n");
    float rms = 0.0f;
    float peak = 0.0f;
    int clipping_count = 0;
    
    esp_err_t err = audio_manager_get_levels(&rms, &peak, &clipping_count);
    if (err != ESP_OK) {
        printf("[ERROR] Failed to measure levels: %s\n", esp_err_to_name(err));
        return 1;
    }
    
    printf("\n=== Level Diagnostics ===\n");
    printf("RMS Energy    : %.2f\n", rms);
    printf("Max Peak      : %.2f\n", peak);
    printf("Clipping      : %d samples\n", clipping_count);
    printf("=========================\n\n");
    
    return 0;
}

static int cmd_speaker_vol(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: speaker_vol <0-100>\n");
        if (audio_manager_is_initialized()) {
            // Note: es8311 driver does not have a get_voice_volume function easily accessible,
            // but we at least shouldn't return an error just for asking usage.
        }
        return 0;
    }
    int vol = atoi(argv[1]);
    if (vol < 0 || vol > 100) {
        printf("[ERROR] Volume must be 0-100\n");
        return 1;
    }
    if (audio_manager_set_speaker_vol(vol) == ESP_OK) {
        printf("[OK] Speaker volume set to %d%%\n", vol);
    } else {
        printf("[ERROR] Failed to set speaker volume\n");
        return 1;
    }
    return 0;
}

#include "dsp_pipeline.h"

static int cmd_agc_info(int argc, char **argv) {
    agc_config_t* agc = dsp_pipeline_get_agc_config();
    printf("\n=== Automatic Gain Control (AGC) ===\n");
    printf("Status      : %s\n", agc->enabled ? "ENABLED" : "DISABLED");
    printf("Target RMS  : %.1f\n", agc->target_rms);
    printf("Max Gain    : %.1fx\n", agc->max_gain);
    printf("Min Gain    : %.1fx\n", agc->min_gain);
    printf("Noise Gate  : %.1f RMS\n", agc->noise_gate_rms);
    printf("Current Gain: %.2fx\n", agc->current_gain);
    printf("Current RMS : %.1f\n", agc->smoothed_rms);
    printf("====================================\n\n");
    return 0;
}

static int cmd_agc_enable(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: agc_enable <0|1>\n");
        return 0;
    }
    agc_config_t* agc = dsp_pipeline_get_agc_config();
    agc->enabled = (atoi(argv[1]) != 0);
    printf("[OK] AGC is now %s\n", agc->enabled ? "ENABLED" : "DISABLED");
    return 0;
}

static int cmd_agc_config(int argc, char **argv) {
    if (argc != 5) {
        printf("Usage: agc_config <target_rms> <max_gain> <min_gain> <noise_gate_rms>\n");
        printf("Example: agc_config 1500 25.0 1.0 150.0\n");
        return 0;
    }
    agc_config_t* agc = dsp_pipeline_get_agc_config();
    agc->target_rms = atof(argv[1]);
    agc->max_gain = atof(argv[2]);
    agc->min_gain = atof(argv[3]);
    agc->noise_gate_rms = atof(argv[4]);
    
    printf("[OK] AGC configuration updated.\n");
    return 0;
}

// ---------------------------------------------------------
// Recording & Playback Commands
// ---------------------------------------------------------

static int cmd_record_mic(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: record_mic <filename.wav> <duration_sec>\n");
        return 1;
    }
    const char* filename = argv[1];
    float duration_sec = atof(argv[2]);
    if (duration_sec <= 0 || duration_sec > 60) {
        printf("[ERROR] Duration must be 1-60 seconds\n");
        return 1;
    }
    
    if (!audio_manager_is_initialized()) {
        printf("Initializing audio...\n");
        if (audio_manager_init() != ESP_OK) return 1;
    }
    
    char path[MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/%s", FS_MOUNT_POINT, filename);
    
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("[ERROR] Failed to open %s for writing\n", path);
        return 1;
    }
    
    // Write placeholder header
    WavHeader dummy_header = {};
    fwrite(&dummy_header, 1, sizeof(WavHeader), f);
    
    printf("Starting recording for %.1f seconds...\n", duration_sec);
    audio_manager_start_capture();
    
    uint32_t total_samples = (uint32_t)(duration_sec * FIREVAD_SAMPLE_RATE);
    uint32_t samples_read = 0;
    const size_t chunk_size = FIREVAD_CHUNK_SAMPLES_100MS;
    int16_t* buffer = (int16_t*)malloc(chunk_size * sizeof(int16_t));
    if (!buffer) {
        printf("[ERROR] Failed to allocate memory for recording\n");
        fclose(f);
        return 1;
    }
    
    while (samples_read < total_samples) {
        size_t to_read = chunk_size;
        if (samples_read + to_read > total_samples) {
            to_read = total_samples - samples_read;
        }
        int read = audio_manager_read(buffer, to_read, FIREVAD_I2S_READ_TIMEOUT_MS);
        if (read > 0) {
            fwrite(buffer, sizeof(int16_t), read, f);
            samples_read += read;
            if (samples_read % (FIREVAD_SAMPLE_RATE) == 0) {
                printf("."); fflush(stdout);
            }
        } else {
            printf("\n[ERROR] I2S read failed\n");
            break;
        }
    }
    
    audio_manager_stop_capture();
    free(buffer);
    
    uint32_t data_size = samples_read * sizeof(int16_t);
    write_wav_header(f, data_size);
    fclose(f);
    
    printf("\n[OK] Recording saved to %s (%.1f seconds)\n", filename, (float)samples_read / FIREVAD_SAMPLE_RATE);
    return 0;
}

// cmd_play_wav removed (Dead Code)

static int cmd_vad_infer_mic(int argc, char **argv) {
    if (!vad_runner_is_model_loaded()) {
        printf("[ERROR] No model loaded.\n");
        return 1;
    }
    
    if (!vad_runner_is_causal()) {
        printf("[ERROR] You loaded an OFFLINE model (Non-Causal).\n");
        printf("        Live streaming (vad_infer_mic) is only supported on Stream-VAD models!\n");
        printf("        Please load a 'firered-stream-vad-*.frvd' model.\n");
        return 1;
    }
    
    float duration_sec = 10.0f;
    if (argc >= 2) duration_sec = atof(argv[1]);
    
    if (!audio_manager_is_initialized()) {
        if (audio_manager_init() != ESP_OK) return 1;
    }
    
    audio_manager_start_capture();
    vad_runner_reset();
    metrics_reset();
    printf("\n=== Real-time VAD Test ===\n");
    float pre_vad = vad_runner_get_pre_vad_multiplier();
    if (pre_vad > 0.0f) {
        printf("Pre-VAD:   Enabled (Multiplier: %.2f)\n", pre_vad);
    } else {
        printf("Pre-VAD:   Disabled (Raw NN Output)\n");
    }
    printf("Threshold: %.2f\n", g_threshold);
    printf("Listening for %.1f seconds...\n", duration_sec);
    
    const size_t frame_size = 160;
    int16_t pcm_frame[frame_size];
    uint32_t total_frames = 0;
    uint32_t speech_count = 0;
    uint32_t max_frames = duration_sec * 100;
    
    while (total_frames < max_frames) {
        int read = audio_manager_read(pcm_frame, frame_size, 100);
        if (read != frame_size) break;
        
        if (g_sw_gain != 1.0f) {
            for (size_t i = 0; i < frame_size; i++) {
                float sample = pcm_frame[i] * g_sw_gain;
                if (sample > 32767.0f) sample = 32767.0f;
                if (sample < -32768.0f) sample = -32768.0f;
                pcm_frame[i] = (int16_t)sample;
            }
        }
        
        float prob = vad_runner_infer_frame(pcm_frame);
        bool is_speech = (prob >= g_threshold);
        if (is_speech) speech_count++;
        total_frames++;
        
        // Prevent Task Watchdog starvation and allow IDLE/WiFi tasks to run
        if (total_frames % 10 == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        if (total_frames % 100 == 0) {
            printf("%7.2f | %11.3f | %s\n", (float)total_frames / 100.0f, prob, is_speech ? "SPEECH" : "silence");
        }
    }
    
    audio_manager_stop_capture();
    printf("\n=== Results ===\n");
    printf("Speech frames: %" PRIu32 " / %" PRIu32 " (%.1f%%)\n", speech_count, total_frames, (float)speech_count/total_frames*100.0f);
    
    return 0;
}

static int cmd_vad_infer_wav(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: vad_infer_wav <filename.wav>\n");
        return 1;
    }
    const char* filename = argv[1];
    
    if (!vad_runner_is_model_loaded()) {
        printf("[ERROR] No model loaded. Use 'model_load' first.\n");
        return 1;
    }
    
    char path[MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/%s", FS_MOUNT_POINT, filename);
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("[ERROR] Failed to open %s\n", path);
        return 1;
    }
    
    WavHeader header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header) || memcmp(header.riff_tag, "RIFF", 4) != 0) {
        printf("[ERROR] Not a valid WAV file\n");
        fclose(f);
        return 1;
    }
    
    printf("\n=== WAV File VAD Analysis ===\n");
    printf("File: %s (%.1f seconds)\n", filename, (float)header.data_length / (FIREVAD_SAMPLE_RATE * 2));
    printf("Threshold: %.2f\n", g_threshold);
    
    vad_runner_reset();
    metrics_reset();
    
    const size_t frame_size = 160; // 10ms
    int16_t pcm_frame[frame_size];
    uint32_t total_frames = 0;
    uint32_t speech_count = 0;
    
    // We process the file frame-by-frame. 
    // This perfectly simulates streaming for Stream-VAD, 
    // and evaluates the causal performance of Offline-VAD/AED models.
    while (fread(pcm_frame, sizeof(int16_t), frame_size, f) == frame_size) {
        float prob = vad_runner_infer_frame(pcm_frame);
        bool is_speech = (prob >= g_threshold);
        if (is_speech) speech_count++;
        total_frames++;
        
        // Prevent Task Watchdog starvation and Cache errors by yielding
        if (total_frames % 10 == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        // Print progress every 0.5s (50 frames) to keep console output clean
        if (total_frames % 50 == 0) {
            printf("Time: %5.1fs | Prob: %5.3f | %s\n", (float)total_frames * 0.01f, prob, is_speech ? "SPEECH" : "silence");
        }
    }
    
    fclose(f);
    
    printf("\n=== Analysis Results ===\n");
    if (total_frames > 0) {
        printf("Speech frames: %" PRIu32 " / %" PRIu32 " (%.1f%%)\n", speech_count, total_frames, (float)speech_count/total_frames*100.0f);
    }
    return 0;
}

static int cmd_vad_dump_golden(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: vad_dump_golden <filename.wav>\n");
        return 1;
    }
    const char* filename = argv[1];
    
    if (!vad_runner_is_model_loaded()) {
        printf("[ERROR] No model loaded. Use 'model_load' first.\n");
        return 1;
    }
    
    return vad_runner_dump_golden(filename);
}

static int cmd_vad_calibrate(int argc, char **argv) {
    int seconds = 2;
    if (argc >= 2) seconds = atoi(argv[1]);
    
    if (!audio_manager_is_initialized()) {
        if (audio_manager_init() != ESP_OK) return 1;
    }
    
    audio_manager_start_capture();
    vad_runner_calibrate_noise(seconds);
    printf("Calibrating background noise for %d seconds. Please stay silent...\n", seconds);
    
    const size_t frame_size = 160;
    int16_t pcm_frame[frame_size];
    uint32_t total_frames = 0;
    uint32_t max_frames = seconds * 100;
    
    while (total_frames < max_frames) {
        int read = audio_manager_read(pcm_frame, frame_size, 100);
        if (read != frame_size) break;
        
        if (g_sw_gain != 1.0f) {
            for (size_t i = 0; i < frame_size; i++) {
                float sample = pcm_frame[i] * g_sw_gain;
                if (sample > 32767.0f) sample = 32767.0f;
                if (sample < -32768.0f) sample = -32768.0f;
                pcm_frame[i] = (int16_t)sample;
            }
        }
        
        vad_runner_infer_frame(pcm_frame);
        total_frames++;
        
        if (total_frames % 10 == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            printf("."); fflush(stdout);
        }
    }
    printf("\n");
    
    audio_manager_stop_capture();
    return 0;
}

static int cmd_vad_pre_vad(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: vad_pre_vad <multiplier>\n");
        printf("  0.0 = disable Pre-VAD\n");
        printf("  1.5 = trigger if energy > 1.5x baseline noise\n");
        printf("\nCurrent multiplier: %.2f\n", vad_runner_get_pre_vad_multiplier());
        return (argc == 1) ? 0 : 1;
    }
    float multiplier = atof(argv[1]);
    vad_runner_set_pre_vad_threshold(multiplier);
    return 0;
}

static int cmd_vad_sleep_mode(int argc, char **argv) {
    if (!vad_runner_is_model_loaded()) {
        printf("[ERROR] No model loaded.\n");
        return 1;
    }
    
    if (!audio_manager_is_initialized()) {
        if (audio_manager_init() != ESP_OK) return 1;
    }
    
    int sleep_ms = 500;
    if (argc >= 2) sleep_ms = atoi(argv[1]);
    
    printf("\n=== Software Duty-Cycling VAD (Light Sleep) ===\n");
    printf("Waking up every %d ms to check for speech.\n", sleep_ms);
    printf("Note: Press Ctrl+T Ctrl+C to exit if console becomes unresponsive.\n");
    
    const size_t frame_size = 160;
    int16_t pcm_frame[frame_size];

    while (true) {
        // Let the OS automatic power management handle light sleep
        printf("z"); fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        printf("w"); fflush(stdout);

        // Reset RNN state: stream-VAD has internal memory that drifts during sleep gaps
        vad_runner_reset();

        audio_manager_start_capture();

        // Flush stale I2S DMA data (5 frames = 50ms worth of old buffers)
        for (int i = 0; i < 5; i++) {
            audio_manager_read(pcm_frame, frame_size, 50);
        }

        // Capture 200ms (20 frames) for burst detection.
        // Pre-VAD is designed for continuous streams; bypass it here for single-burst wakeup checks.
        float max_prob = 0.0f;
        float saved_multiplier = vad_runner_get_pre_vad_multiplier();
        vad_runner_set_pre_vad_threshold_silent(0.0f); // Bypass Pre-VAD temporarily (silent)
        for (int i = 0; i < 20; i++) {
            int read = audio_manager_read(pcm_frame, frame_size, 100);
            if (read == frame_size) {
                float prob = vad_runner_infer_frame(pcm_frame);
                if (prob > max_prob) max_prob = prob;
            }
        }
        vad_runner_set_pre_vad_threshold_silent(saved_multiplier); // Restore Pre-VAD (silent)

        if (max_prob >= g_threshold) {
            printf("\n[SPEECH DETECTED!] Prob: %.2f\n", max_prob);

            // Stay awake and track speech until 500ms of silence
            int silence_frames = 0;
            while (silence_frames < 50) {
                int read = audio_manager_read(pcm_frame, frame_size, 100);
                if (read == frame_size) {
                    float prob = vad_runner_infer_frame(pcm_frame);
                    if (prob < g_threshold) {
                        silence_frames++;
                    } else {
                        silence_frames = 0;
                    }
                    if (silence_frames % 10 == 0) {
                        printf("."); fflush(stdout);
                    }
                } else {
                    break;
                }
            }
            printf("\n[SILENCE] Going back to sleep.\n");
            vad_runner_reset();
        }

        audio_manager_stop_capture();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return 0;
}

// ---------------------------------------------------------
// Benchmark Command
// ---------------------------------------------------------

static int cmd_benchmark(int argc, char **argv) {
    if (!vad_runner_is_model_loaded()) {
        printf("[ERROR] No model loaded. Load a model first to benchmark it.\n");
        return 1;
    }
    
    int num_frames = 1000; // 10 seconds of audio
    if (argc >= 2) {
        num_frames = atoi(argv[1]);
    }
    
    printf("\n=== VAD Benchmark Mode ===\n");
    printf("Frames to run: %d (%.1f seconds of audio)\n", num_frames, num_frames * 0.01f);
    
    // RAM Measurement
    uint32_t free_heap_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_spiram_start = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    printf("Free Internal RAM: %" PRIu32 " bytes\n", free_heap_start);
    printf("Free SPIRAM: %" PRIu32 " bytes\n", free_spiram_start);
    
    const size_t frame_size = 160;
    int16_t dummy_frame[frame_size];
    for (size_t i = 0; i < frame_size; i++) {
        dummy_frame[i] = (int16_t)((i % 100) * 10);
    }
    
    vad_runner_reset();
    benchmark_reset();
    
    int64_t start_time = esp_timer_get_time();
    
    for (int i = 0; i < num_frames; i++) {
        uint32_t b_id = benchmark_start("Infer Frame");
        vad_runner_infer_frame(dummy_frame);
        benchmark_end(b_id);
    }
    
    int64_t end_time = esp_timer_get_time();
    int64_t total_us = end_time - start_time;
    
    printf("\n--- Results ---\n");
    printf("Total Time: %lld us\n", total_us);
    printf("Average Time per Frame: %lld us\n", total_us / num_frames);
    printf("Real-time Factor (RTF): %.4f\n", (float)(total_us / num_frames) / 10000.0f);
    
    benchmark_print_results();
    
    return 0;
}

// ---------------------------------------------------------
// Registration
// ---------------------------------------------------------

static int cmd_guide(int argc, char **argv) {
    printf("\n--- Quick Start Workflow ---\n");
    printf("1. ls [path]              - Navigate the SD card and find files\n");
    printf("2. vad_model_load <model> - Load a VAD model from the list\n");
    printf("5. record_mic test.wav 5  - Record 5s from mic to test hardware\n");
    printf("7. vad_metrics            - View latency metrics\n\n");
    return 0;
}

extern "C" int32_t fc_dot_s8_pie(const int8_t *input, const int8_t *filter, int32_t row_len);
static int cmd_vad_test_pie(int argc, char **argv) {
    int32_t len = 80;
    if (argc > 1) len = atoi(argv[1]);
    
    int8_t* in = (int8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM);
    int8_t* flt = (int8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM);
    for (int i=0; i<len; i++) {
        in[i] = i % 10;
        flt[i] = (i % 5) - 2;
    }
    
    int32_t sum_scalar = 0;
    for (int i=0; i<len; i++) sum_scalar += (int32_t)in[i] * (int32_t)flt[i];
    
    int32_t sum_pie = fc_dot_s8_pie(in, flt, len);
    
    printf("Test len: %ld\n", len);
    printf("Scalar: %ld\n", sum_scalar);
    printf("PIE   : %ld\n", sum_pie);
    if (sum_scalar != sum_pie) printf("MISMATCH!\n");
    else printf("MATCH!\n");
    
    heap_caps_free(in);
    heap_caps_free(flt);
    return 0;
}

void cmd_vad_cli_register(void) {
    esp_console_register_help_command();
    
    esp_console_cmd_t cmd_load = {}; cmd_load.command = "vad_model_load"; cmd_load.help = "Load model"; cmd_load.hint = "<filename.frvd>"; cmd_load.func = &cmd_vad_model_load;
    esp_console_cmd_t cmd_info = {}; cmd_info.command = "vad_model_info"; cmd_info.help = "Model info"; cmd_info.func = &cmd_vad_model_info;

    esp_console_cmd_t cmd_thr = {}; cmd_thr.command = "vad_threshold"; cmd_thr.help = "Set threshold"; cmd_thr.hint = "<0.0-1.0>"; cmd_thr.func = &cmd_vad_threshold;
    esp_console_cmd_t cmd_gn = {}; cmd_gn.command = "mic_gain"; cmd_gn.help = "Set mic gain (use -s for software, -h for hardware ES8311 PGA)"; cmd_gn.hint = "[-s <mult>] [-h <0-11>]"; cmd_gn.func = &cmd_mic_gain;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_gn));
    
    esp_console_cmd_t cmd_sl = {}; cmd_sl.command = "vad_sleep_mode"; cmd_sl.help = "Enter Software Duty-Cycling VAD mode using Light Sleep"; cmd_sl.hint = "[<sleep_interval_ms>]"; cmd_sl.func = &cmd_vad_sleep_mode;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sl));
    esp_console_cmd_t cmd_sv = {}; cmd_sv.command = "speaker_vol"; cmd_sv.help = "Set speaker volume"; cmd_sv.hint = "<0-100>"; cmd_sv.func = &cmd_speaker_vol;
    
    esp_console_cmd_t cmd_agc_i = {}; cmd_agc_i.command = "agc_info"; cmd_agc_i.help = "Show AGC status"; cmd_agc_i.func = &cmd_agc_info;
    esp_console_cmd_register(&cmd_agc_i);
    esp_console_cmd_t cmd_agc_e = {}; cmd_agc_e.command = "agc_enable"; cmd_agc_e.help = "Enable/Disable AGC"; cmd_agc_e.hint = "<0|1>"; cmd_agc_e.func = &cmd_agc_enable;
    esp_console_cmd_register(&cmd_agc_e);
    esp_console_cmd_t cmd_agc_c = {}; cmd_agc_c.command = "agc_config"; cmd_agc_c.help = "Configure AGC parameters"; cmd_agc_c.hint = "<target> <max_gain> <min_gain> <gate>"; cmd_agc_c.func = &cmd_agc_config;
    esp_console_cmd_register(&cmd_agc_c);
    
    esp_console_cmd_t cmd_rec = {}; cmd_rec.command = "record_mic"; cmd_rec.help = "Record mic to WAV"; cmd_rec.hint = "<filename.wav> <seconds>"; cmd_rec.func = &cmd_record_mic;
    
    esp_console_cmd_t cmd_tm = {}; cmd_tm.command = "vad_infer_mic"; cmd_tm.help = "Test VAD on mic"; cmd_tm.hint = "[seconds]"; cmd_tm.func = &cmd_vad_infer_mic;
    esp_console_cmd_t cmd_tw = {}; cmd_tw.command = "vad_infer_wav"; cmd_tw.help = "Test VAD on WAV file"; cmd_tw.hint = "<filename.wav>"; cmd_tw.func = &cmd_vad_infer_wav;
    esp_console_cmd_t cmd_gd = {}; cmd_gd.command = "guide"; cmd_gd.help = "Show Quick Start workflow"; cmd_gd.func = &cmd_guide;
    esp_console_cmd_t cmd_dump = {}; cmd_dump.command = "vad_dump_golden"; cmd_dump.help = "Dump Golden Test data to SD"; cmd_dump.hint = "<filename.wav>"; cmd_dump.func = &cmd_vad_dump_golden;
    
    esp_console_cmd_t cmd_cal = {}; cmd_cal.command = "vad_calibrate"; cmd_cal.help = "Calibrate Pre-VAD"; cmd_cal.hint = "[seconds]"; cmd_cal.func = &cmd_vad_calibrate;
    esp_console_cmd_t cmd_pvd = {}; cmd_pvd.command = "vad_pre_vad"; cmd_pvd.help = "Set Pre-VAD multiplier"; cmd_pvd.hint = "<multiplier>"; cmd_pvd.func = &cmd_vad_pre_vad;
    
    esp_console_cmd_t cmd_tpie = {}; cmd_tpie.command = "vad_test_pie"; cmd_tpie.help = "Test PIE assembly"; cmd_tpie.func = &cmd_vad_test_pie;
    esp_console_cmd_t cmd_ls = {}; cmd_ls.command = "ls"; cmd_ls.help = "Admin: List directory contents"; cmd_ls.hint = "[path]"; cmd_ls.func = &cmd_fs_ls;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_ls));
    esp_console_cmd_register(&cmd_tpie);
    
    esp_console_cmd_t cmd_bm = {}; cmd_bm.command = "benchmark"; cmd_bm.help = "Benchmark CPU/RAM usage of current model"; cmd_bm.hint = "[num_frames]"; cmd_bm.func = &cmd_benchmark;
    esp_console_cmd_register(&cmd_bm);
    
    esp_console_cmd_register(&cmd_load);
    esp_console_cmd_register(&cmd_info);
    esp_console_cmd_register(&cmd_thr);
    esp_console_cmd_register(&cmd_gn);
    
    esp_console_cmd_t cmd_msel = {}; cmd_msel.command = "mic_select"; cmd_msel.help = "Select active microphone"; cmd_msel.hint = "<inmp441|es8311>"; cmd_msel.func = &cmd_mic_select;
    esp_console_cmd_register(&cmd_msel);
    
    esp_console_cmd_t cmd_minf = {}; cmd_minf.command = "mic_info"; cmd_minf.help = "Show microphone info"; cmd_minf.func = &cmd_mic_info;
    esp_console_cmd_register(&cmd_minf);
    
    esp_console_cmd_t cmd_mlev = {}; cmd_mlev.command = "mic_level"; cmd_mlev.help = "Measure raw RMS/Peak levels (1s)"; cmd_mlev.func = &cmd_mic_level;
    esp_console_cmd_register(&cmd_mlev);
    
    esp_console_cmd_register(&cmd_sv);
    esp_console_cmd_register(&cmd_rec);
    esp_console_cmd_register(&cmd_tm);
    esp_console_cmd_register(&cmd_tw);
    esp_console_cmd_register(&cmd_gd);
    esp_console_cmd_register(&cmd_cal);
    esp_console_cmd_register(&cmd_pvd);
    esp_console_cmd_register(&cmd_dump);
}
