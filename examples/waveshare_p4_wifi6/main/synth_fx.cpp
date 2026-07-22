#include "synth_fx.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
// #include "audio_hal_i2s.h" // We'll implement a stub or proper I2S write in main.cpp

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// We define a weak function so the main app can override how audio is actually written to I2S/codec
__attribute__((weak)) void app_audio_write(int16_t* buffer, size_t num_bytes) {
    // Stub
}

void synth_fx_init(void) {
}

static void generate_tone(float freq_start, float freq_end, int duration_ms, float volume) {
    int sample_rate = 16000;
    int samples = sample_rate * duration_ms / 1000;
    int16_t buffer[160];
    int buf_idx = 0;
    
    for (int i = 0; i < samples; i++) {
        float progress = (float)i / samples;
        float current_freq = freq_start + (freq_end - freq_start) * progress;
        float amplitude = 32767.0f * volume;
        
        // Envelope generator (ASR)
        float env = 1.0f;
        if (progress < 0.1f) env = progress / 0.1f;
        else if (progress > 0.8f) env = (1.0f - progress) / 0.2f;
        
        int16_t sample = (int16_t)(amplitude * env * sinf(2.0f * M_PI * current_freq * i / sample_rate));
        buffer[buf_idx++] = sample; // Left
        buffer[buf_idx++] = sample; // Right
        
        if (buf_idx == 160) {
            app_audio_write(buffer, sizeof(buffer));
            buf_idx = 0;
        }
    }
    
    if (buf_idx > 0) {
        app_audio_write(buffer, buf_idx * sizeof(int16_t));
    }
}

void synth_fx_play(SYNTH_FX_type_t type) {
    switch (type) {
        case SYNTH_FX_READY:
            // Ascending triad (Sci-Fi Boot sequence)
            generate_tone(440.0f, 440.0f, 100, 0.2f);
            generate_tone(554.0f, 554.0f, 100, 0.2f);
            generate_tone(659.0f, 659.0f, 200, 0.2f);
            break;
            
        case SYNTH_FX_WAKEWORD:
            // Star Trek style short comm chirp
            generate_tone(800.0f, 1200.0f, 150, 0.3f);
            break;
            
        case SYNTH_FX_ACK:
            // Short high blip
            generate_tone(1500.0f, 1500.0f, 50, 0.2f);
            break;
            
        case SYNTH_FX_ERROR:
            // Low descending buzz
            generate_tone(300.0f, 150.0f, 300, 0.3f);
            break;
    }
}
