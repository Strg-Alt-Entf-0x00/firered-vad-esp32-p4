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
#include "esp_spiffs.h"

#include "config.h"
#include "audio_manager.h"
#include "vad_runner.h"
#include "metrics.h"



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

static int cmd_vad_model_list(int argc, char **argv) {
    printf("\n%-30s | %-10s\n", "Filename", "Size");
    printf("-------------------------------------------------\n");
    
    DIR* dir = opendir(SPIFFS_MOUNT_POINT);
    if (!dir) return 1;
    
    struct dirent* ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".frvd") == NULL) continue;
        char path[MAX_FILE_PATH];
        snprintf(path, sizeof(path), "%s/%s", SPIFFS_MOUNT_POINT, ent->d_name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fclose(f);
        printf("%-30s | %7zu KB\n", ent->d_name, size / 1024);
        count++;
    }
    closedir(dir);
    if (count == 0) printf("(No .frvd models found)\n");
    printf("\n");
    return 0;
}

static int cmd_vad_threshold(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: threshold <0.0-1.0>\nCurrent: %.2f\n", g_threshold);
        return 1;
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
    if (argc < 2) {
        printf("Usage:\n  mic_gain -s <multiplier>   : Set software gain (0.1-10.0)\n");
        printf("  mic_gain -h <level>        : Set hardware mic gain (0-11)\n");
        printf("  mic_gain -h                : Show current hardware gain\n");
        printf("\nCurrent software gain: %.2fx\n", g_sw_gain);
        if (audio_manager_is_initialized()) {
            uint8_t hw_gain = 0;
            if (audio_manager_get_mic_gain(&hw_gain) == ESP_OK) {
                printf("Current hardware gain: Level %d (%ddB)\n", hw_gain, hw_gain * 2);
            }
        }
        return 1;
    }
    if (strcmp(argv[1], "-s") == 0) {
        if (argc != 3) return 1;
        float val = atof(argv[2]);
        if (val < 0.1f || val > 10.0f) return 1;
        g_sw_gain = val;
        printf("[OK] Software gain set to %.2fx\n", g_sw_gain);
        return 0;
    }
    if (strcmp(argv[1], "-h") == 0) {
        if (!audio_manager_is_initialized()) {
            printf("[ERROR] Audio not initialized\n");
            return 1;
        }
        if (argc == 2) {
            uint8_t gain = 0;
            if (audio_manager_get_mic_gain(&gain) == ESP_OK)
                printf("Hardware gain: Level %d (%ddB)\n", gain, gain * 2);
            return 0;
        }
        if (argc != 3) return 1;
        int level = atoi(argv[2]);
        if (level < 0 || level > 11) return 1;
        if (audio_manager_set_mic_gain(level) == ESP_OK) {
            printf("[OK] Hardware gain set to level %d (%ddB)\n", level, level * 2);
        }
        return 0;
    }
    return 1;
}

static int cmd_speaker_vol(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: speaker_vol <0-100>\n");
        return 1;
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

static int cmd_vad_metrics(int argc, char **argv) {
    metrics_print_summary();
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
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_MOUNT_POINT, filename);
    
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
    const size_t chunk_size = 1600; // 100ms
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
        int read = audio_manager_read(buffer, to_read, 1000);
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

static int cmd_play_wav(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: play_wav <filename.wav>\n");
        return 1;
    }
    const char* filename = argv[1];
    
    if (!audio_manager_is_initialized()) {
        if (audio_manager_init() != ESP_OK) return 1;
    }
    
    char path[MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_MOUNT_POINT, filename);
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("[ERROR] Failed to open %s\n", path);
        return 1;
    }
    
    // Read RIFF header
    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff+8, "WAVE", 4) != 0) {
        printf("[ERROR] Not a valid WAV file\n");
        fclose(f);
        return 1;
    }
    
    bool data_found = false;
    uint32_t data_length = 0;
    while (!feof(f)) {
        char tag[4];
        uint32_t size;
        if (fread(tag, 1, 4, f) != 4) break;
        if (fread(&size, 1, 4, f) != 4) break;
        
        if (memcmp(tag, "data", 4) == 0) {
            data_found = true;
            data_length = size;
            break;
        }
        fseek(f, size, SEEK_CUR);
    }
    
    if (!data_found) {
        printf("[ERROR] 'data' chunk not found\n");
        fclose(f);
        return 1;
    }
    
    printf("Playing %s...\n", filename);
    audio_manager_start_playback();
    
    const size_t chunk_size = 1600;
    int16_t* buffer = (int16_t*)malloc(chunk_size * sizeof(int16_t));
    if (!buffer) {
        printf("[ERROR] Failed to allocate memory for playback\n");
        fclose(f);
        return 1;
    }
    
    while (true) {
        size_t bytes_read = fread(buffer, 1, chunk_size * sizeof(int16_t), f);
        if (bytes_read == 0) break;
        
        size_t samples = bytes_read / sizeof(int16_t);
        int written = audio_manager_write(buffer, samples, 1000);
        if (written < 0) {
            printf("[ERROR] Playback failed\n");
            break;
        }
    }
    
    audio_manager_stop_playback();
    free(buffer);
    fclose(f);
    printf("[OK] Playback finished\n");
    return 0;
}

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

static int cmd_fs_wav_list(int argc, char **argv) {
    DIR *dir = opendir(SPIFFS_MOUNT_POINT);
    if (!dir) {
        printf("[ERROR] Failed to open directory\n");
        return 1;
    }

    printf("\nFilename                       | Size\n");
    printf("-------------------------------------------------\n");
    
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".wav")) {
            char filepath[MAX_FILE_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s", SPIFFS_MOUNT_POINT, entry->d_name);
            struct stat st;
            if (stat(filepath, &st) == 0) {
                printf("%-30s | %7ld KB\n", entry->d_name, st.st_size / 1024);
                found = true;
            }
        }
    }
    if (!found) {
        printf("No WAV files found in SPIFFS.\n");
    }
    printf("\n");
    closedir(dir);
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
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_MOUNT_POINT, filename);
    
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
        return 1;
    }
    float multiplier = atof(argv[1]);
    vad_runner_set_pre_vad_threshold(multiplier);
    return 0;
}

// ---------------------------------------------------------
// Registration
// ---------------------------------------------------------

static int cmd_guide(int argc, char **argv) {
    printf("\n--- Quick Start Workflow ---\n");
    printf("1. vad_model_list         - Show available VAD models\n");
    printf("2. vad_model_load <model> - Load a VAD model from the list\n");
    printf("3. fs_wav_list            - Show available audio files\n");
    printf("4. play_wav <file>        - Play an audio file to test speaker\n");
    printf("5. record_mic test.wav 5  - Record 5s from mic to test hardware\n");
    printf("6. vad_infer_mic 10       - Test VAD on live mic for 10s\n");
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
    esp_console_cmd_t cmd_list = {}; cmd_list.command = "vad_model_list"; cmd_list.help = "List models"; cmd_list.func = &cmd_vad_model_list;
    esp_console_cmd_t cmd_wav = {}; cmd_wav.command = "fs_wav_list"; cmd_wav.help = "List WAV files"; cmd_wav.func = &cmd_fs_wav_list;
    esp_console_cmd_t cmd_thr = {}; cmd_thr.command = "vad_threshold"; cmd_thr.help = "Set threshold"; cmd_thr.hint = "<0.0-1.0>"; cmd_thr.func = &cmd_vad_threshold;
    esp_console_cmd_t cmd_gn = {}; cmd_gn.command = "mic_gain"; cmd_gn.help = "Set mic gain (use -s for software, -h for hardware ES8311 PGA)"; cmd_gn.hint = "[-s <mult>] [-h <0-11>]"; cmd_gn.func = &cmd_mic_gain;
    esp_console_cmd_t cmd_sv = {}; cmd_sv.command = "speaker_vol"; cmd_sv.help = "Set speaker volume"; cmd_sv.hint = "<0-100>"; cmd_sv.func = &cmd_speaker_vol;
    esp_console_cmd_t cmd_met = {}; cmd_met.command = "vad_metrics"; cmd_met.help = "Show performance metrics"; cmd_met.func = &cmd_vad_metrics;
    esp_console_cmd_t cmd_rec = {}; cmd_rec.command = "record_mic"; cmd_rec.help = "Record mic to WAV"; cmd_rec.hint = "<filename.wav> <seconds>"; cmd_rec.func = &cmd_record_mic;
    esp_console_cmd_t cmd_pl = {}; cmd_pl.command = "play_wav"; cmd_pl.help = "Play WAV file"; cmd_pl.hint = "<filename.wav>"; cmd_pl.func = &cmd_play_wav;
    esp_console_cmd_t cmd_tm = {}; cmd_tm.command = "vad_infer_mic"; cmd_tm.help = "Test VAD on mic"; cmd_tm.hint = "[seconds]"; cmd_tm.func = &cmd_vad_infer_mic;
    esp_console_cmd_t cmd_tw = {}; cmd_tw.command = "vad_infer_wav"; cmd_tw.help = "Test VAD on WAV file"; cmd_tw.hint = "<filename.wav>"; cmd_tw.func = &cmd_vad_infer_wav;
    esp_console_cmd_t cmd_gd = {}; cmd_gd.command = "guide"; cmd_gd.help = "Show Quick Start workflow"; cmd_gd.func = &cmd_guide;
    
    esp_console_cmd_t cmd_cal = {}; cmd_cal.command = "vad_calibrate"; cmd_cal.help = "Calibrate Pre-VAD"; cmd_cal.hint = "[seconds]"; cmd_cal.func = &cmd_vad_calibrate;
    esp_console_cmd_t cmd_pvd = {}; cmd_pvd.command = "vad_pre_vad"; cmd_pvd.help = "Set Pre-VAD multiplier"; cmd_pvd.hint = "<multiplier>"; cmd_pvd.func = &cmd_vad_pre_vad;
    
    esp_console_cmd_t cmd_tpie = {}; cmd_tpie.command = "vad_test_pie"; cmd_tpie.help = "Test PIE assembly"; cmd_tpie.func = &cmd_vad_test_pie;
    esp_console_cmd_register(&cmd_tpie);
    
    esp_console_cmd_register(&cmd_load);
    esp_console_cmd_register(&cmd_info);
    esp_console_cmd_register(&cmd_list);
    esp_console_cmd_register(&cmd_wav);
    esp_console_cmd_register(&cmd_thr);
    esp_console_cmd_register(&cmd_gn);
    esp_console_cmd_register(&cmd_sv);
    esp_console_cmd_register(&cmd_met);
    esp_console_cmd_register(&cmd_rec);
    esp_console_cmd_register(&cmd_pl);
    esp_console_cmd_register(&cmd_tm);
    esp_console_cmd_register(&cmd_tw);
    esp_console_cmd_register(&cmd_gd);
    esp_console_cmd_register(&cmd_cal);
    esp_console_cmd_register(&cmd_pvd);
}
