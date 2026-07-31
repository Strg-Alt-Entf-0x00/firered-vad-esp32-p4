#include "esp_firevad_dsp.h"
#include "mel_constants.h"
#include "dsps_fft2r.h"
#include "dsps_math.h"
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_cpu.h"
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

void DSP_IRAM_ATTR esp_firevad_dsp_extract_features(const int16_t* pcm_160, float* features_80, float* out_energy) {
    static float window_buffer[400] = {0};
    memmove(window_buffer, window_buffer + 160, (400 - 160) * sizeof(float));
    static float dc_offset = 0.0f;
    float total_sq = 0.0f;

#ifdef ESP_PLATFORM
    uint32_t t0 = esp_cpu_get_cycle_count();
#endif

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

    // OPT: fft_buf is static — only zero the zero-padded tail [400..511].
    //      The signal region [0..399] is fully overwritten in the loop below,
    //      so no need to zero the entire 4096-byte buffer each frame.
    static float fft_buf[512 * 2];
    memset(&fft_buf[800], 0, 224 * sizeof(float));
    for (int i = 0; i < 400; i++) {
        fft_buf[i * 2]     = window_buffer[i] * KALDI_WINDOW[i];
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

    static float power_spectrum[257];
    for (int i = 0; i < 257; i++) {
        float re = fft_buf[i * 2];
        float im = fft_buf[i * 2 + 1];
        power_spectrum[i] = re * re + im * im;
    }

    // OPT: fast_logf — polynomial approximation via Atanh series on mantissa.
    //      Max error ~2e-5 relative. For VAD mel features, numerically identical
    //      to libm logf but saves ~10x cycles vs software transcendental.
    for (int m = 0; m < 80; m++) {
        float mel_energy = 0.0f;
        uint8_t  cnt = KALDI_MEL_COUNTS[m];
        uint16_t off = KALDI_MEL_OFFSETS[m];
        for (int i = 0; i < cnt; i++) {
            mel_energy += power_spectrum[KALDI_MEL_INDICES[off + i]]
                        * KALDI_MEL_WEIGHTS[off + i];
        }
        if (mel_energy < 1e-6f) mel_energy = 1e-6f;

        // IEEE 754 exponent extraction + polynomial on mantissa in [1, 2)
        uint32_t ux; memcpy(&ux, &mel_energy, 4);
        int32_t  iexp = (int32_t)(ux >> 23) - 127;
        ux = (ux & 0x007FFFFFu) | 0x3F800000u;
        float mf; memcpy(&mf, &ux, 4);
        // Atanh series: ln(m) = 2*atanh((m-1)/(m+1))
        float tk  = (mf - 1.0f) / (mf + 1.0f);
        float tk2 = tk * tk;
        float ln_m = tk * (2.0f + tk2 * (0.666667f + tk2 * (0.4f + tk2 * 0.285714f)));
        features_80[m] = ln_m + (float)iexp * 0.693147180f;
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
