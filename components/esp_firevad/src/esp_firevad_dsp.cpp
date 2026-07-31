#include "esp_firevad_dsp.h"
#include "mel_constants.h"
#include "dsps_fft2r.h"
#include "dsps_math.h"
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char* TAG = "ESP_FIREVAD_DSP";
#endif

esp_err_t esp_firevad_dsp_init(void) {
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, 512);
#ifdef ESP_PLATFORM
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize FFT: %d", ret);
    }
#endif
    return ret;
}

void esp_firevad_dsp_extract_features(const int16_t* pcm_160, float* features_80, float* out_energy) {
    static float window_buffer[400] = {0};
    memmove(window_buffer, window_buffer + 160, (400 - 160) * sizeof(float));
    static float dc_offset = 0.0f;
    float total_sq = 0.0f;
    
    for (int i = 0; i < 160; i++) {
        float raw = (float)pcm_160[i];
        dc_offset = dc_offset * 0.995f + raw * 0.005f;
        float s = raw - dc_offset;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        window_buffer[240 + i] = s;
        total_sq += s * s;
    }
    
    if (out_energy) {
        *out_energy = sqrtf(total_sq / 160.0f);
    }
    
    static float fft_buf[512 * 2];
    memset(fft_buf, 0, sizeof(fft_buf));
    for (int i = 0; i < 400; i++) {
        fft_buf[i * 2] = window_buffer[i] * KALDI_WINDOW[i];
        fft_buf[i * 2 + 1] = 0.0f;
    }
    
    dsps_fft2r_fc32(fft_buf, 512);
    dsps_bit_rev_fc32(fft_buf, 512);
    
    static float power_spectrum[257];
    for (int i = 0; i < 257; i++) {
        float re = fft_buf[i * 2];
        float im = fft_buf[i * 2 + 1];
        power_spectrum[i] = re * re + im * im;
    }
    
    for (int m = 0; m < 80; m++) {
        float mel_energy = 0.0f;
        uint8_t count = KALDI_MEL_COUNTS[m];
        uint16_t offset = KALDI_MEL_OFFSETS[m];
        
        for (int i = 0; i < count; i++) {
            uint16_t idx = KALDI_MEL_INDICES[offset + i];
            mel_energy += power_spectrum[idx] * KALDI_MEL_WEIGHTS[offset + i];
        }
        
        if (mel_energy < 1e-6f) mel_energy = 1e-6f;
        features_80[m] = logf(mel_energy);
    }
}
