#include "esp_firevad_dsp.h"
#include "mel_constants.h"
#include "dsps_fft2r.h"
#include "dsps_math.h"
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_attr.h"
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

#ifdef ESP_PLATFORM
#define DSP_IRAM_ATTR IRAM_ATTR
#else
#define DSP_IRAM_ATTR
#endif

static inline float fast_log_mel_energy(float energy) {
    if (energy < 1e-6f) return -13.815510557964274f;

    uint32_t ux;
    memcpy(&ux, &energy, sizeof(ux));
    int32_t iexp = (int32_t)(ux >> 23) - 127;
    ux = (ux & 0x007FFFFFu) | 0x3F800000u;
    float mf;
    memcpy(&mf, &ux, sizeof(mf));

    float tk = (mf - 1.0f) / (mf + 1.0f);
    float tk2 = tk * tk;
    float ln_m = tk * (2.0f + tk2 * (0.666667f + tk2 * (0.4f + tk2 * 0.285714f)));
    return ln_m + (float)iexp * 0.693147180f;
}

static DRAM_ATTR float window_buffer[400] = {0};
static DRAM_ATTR float fft_buf[512 * 2] = {0};
static float prev_samp = 0.0f;

void esp_firevad_dsp_reset(void) {
    memset(window_buffer, 0, sizeof(window_buffer));
    memset(fft_buf, 0, sizeof(fft_buf));
    prev_samp = 0.0f;
}

void DSP_IRAM_ATTR esp_firevad_dsp_extract_features(const int16_t* pcm_160, float* features_80, float* out_energy) {
    memmove(window_buffer, window_buffer + 160, (400 - 160) * sizeof(float));
    float total_sq = 0.0f;

#ifdef ESP_PLATFORM
    uint32_t t0 = esp_cpu_get_cycle_count();
#endif

    // 1. Append raw PCM samples to window buffer
    for (int i = 0; i < 160; i++) {
        float raw = (float)pcm_160[i];
        if (raw > 32767.0f) raw = 32767.0f;
        if (raw < -32768.0f) raw = -32768.0f;
        window_buffer[240 + i] = raw;
        total_sq += raw * raw;
    }

    if (out_energy) {
        *out_energy = sqrtf(total_sq / 160.0f);
    }

    // 2. Compute frame mean (Kaldi remove_dc_offset)
    float frame_mean = 0.0f;
    for (int i = 0; i < 400; i++) {
        frame_mean += window_buffer[i];
    }
    frame_mean /= 400.0f;

    memset(&fft_buf[800], 0, 224 * sizeof(float));
    
    // For pre-emphasis, we need the (i-1) sample. 
    // For i=0, we should ideally use the sample before the window, 
    // but in Kaldi, preemphasis is often applied after DC removal,
    // and the first sample uses itself or 0.
    // Torchaudio kaldi.fbank uses: signal[i] = signal[i] - preemph * signal[i-1]
    // where signal[0] = signal[0] - preemph * signal[0] 
    
    // prev_samp is maintained across chunks for continuous preemphasis
    if (prev_samp == 0.0f) {
        prev_samp = window_buffer[0] - frame_mean;
    }
    for (int i = 0; i < 400; i++) {
        float curr_samp = window_buffer[i] - frame_mean;
        float preemph_samp = curr_samp - 0.97f * prev_samp;
        prev_samp = curr_samp;
        
        fft_buf[i * 2]     = preemph_samp * KALDI_WINDOW[i];
        fft_buf[i * 2 + 1] = 0.0f;
    }

#ifdef ESP_PLATFORM
    uint32_t t1 = esp_cpu_get_cycle_count();
#endif

    dsps_fft2r_fc32(fft_buf, 512);
    dsps_bit_rev_fc32(fft_buf, 512);

#ifdef ESP_PLATFORM
    uint32_t t2 = esp_cpu_get_cycle_count();
#endif

    static DRAM_ATTR float power_spectrum[257];
    for (int i = 0; i < 257; i++) {
        float re = fft_buf[i * 2];
        float im = fft_buf[i * 2 + 1];
        power_spectrum[i] = re * re + im * im;
    }

    for (int m = 0; m < 80; m++) {
        const uint8_t cnt = KALDI_MEL_COUNTS[m];
        const uint16_t off = KALDI_MEL_OFFSETS[m];
        const uint16_t* idx_ptr = KALDI_MEL_INDICES + off;
        const float* weight_ptr = KALDI_MEL_WEIGHTS + off;

        float mel_energy = 0.0f;
        for (int i = 0; i < cnt; i++) {
            mel_energy += power_spectrum[idx_ptr[i]] * weight_ptr[i];
        }

        features_80[m] = fast_log_mel_energy(mel_energy);
    }

#ifdef ESP_PLATFORM
    uint32_t t3 = esp_cpu_get_cycle_count();
    static uint32_t s_cnt = 0;
    static uint64_t s_win = 0, s_fft = 0, s_mel = 0;
    s_win += t1 - t0;
    s_fft += t2 - t1;
    s_mel += t3 - t2;
    if (++s_cnt % 500 == 0) {
        printf("DSP/500: win=%u fft=%u mel=%u\n",
               (unsigned)(s_win/500), (unsigned)(s_fft/500), (unsigned)(s_mel/500));
        s_win = s_fft = s_mel = 0;
    }
#endif
}
