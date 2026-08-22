/**
 * FireRedVAD Native C++ Inference Engine for ESP32-P4
 * Implementation of DFSMN forward pass with Int8 Quantization.
 */

#include "esp_firevad.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "dsps_dotprod.h"  // Only for FP32 dense_dot_f32_esp_dsp()

// NOTE: dsps_mul, dsps_add, dsps_sub were removed (Session 13)
// Reason: Function call overhead caused INT8 regression (4.47ms -> 6.49ms)
// Solution: Use inline loops instead - compiler auto-vectorizes them!

#ifndef TCM_BSS_ATTR
#define TCM_BSS_ATTR DRAM_ATTR
#endif

// Zero-wait-state memory for DSP hot path
static TCM_BSS_ATTR __attribute__((aligned(16))) int16_t tcm_scratch_in_q16[256];
static TCM_BSS_ATTR __attribute__((aligned(16))) int8_t tcm_scratch_in_q[256];

static const char* TAG = "EspFirevad";

// Performance logging configuration with flexible rate control
// Per-frame logs occur every 10ms (~100 logs/second) which can cause UART overflow
// Use menuconfig to select logging mode: DISABLED, RATE_LIMITED, or FULL

#if defined(CONFIG_FIREVAD_LOG_RATE_LIMITED)
    // Rate-limited logging: Configurable via menuconfig
    static uint32_t _log_frame_counter = 0;
    #ifndef CONFIG_FIREVAD_LOG_RATE_DIVIDER
        #define CONFIG_FIREVAD_LOG_RATE_DIVIDER 100  // Fallback default
    #endif
    #define esp_firevad_LOGI(fmt, ...) \
        do { \
            if (++_log_frame_counter >= CONFIG_FIREVAD_LOG_RATE_DIVIDER) { \
                ESP_LOGI(TAG, fmt, ##__VA_ARGS__); \
                _log_frame_counter = 0; \
            } \
        } while(0)
#elif defined(CONFIG_FIREVAD_LOG_FULL)
    // Full per-frame logging: DANGEROUS - only for short tests!
    #define esp_firevad_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
    #warning "FIREVAD_LOG_FULL enabled: High risk of watchdog timeout! Use high baudrate (921600+) and short test duration."
#else
    // Disabled (production default): No performance logs
    #define esp_firevad_LOGI(fmt, ...) // Disabled for production stability
#endif

#define esp_firevad_LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)

static void* esp_firevad_malloc(int version, size_t size) {
    if (version == 2 || version == 3) {
        void* ptr = heap_caps_aligned_alloc(16, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!ptr) ptr = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
        return ptr;
    }
    return heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
}
#define esp_firevad_free_ptr(ptr) heap_caps_free(ptr)

// Enable PIE (Processor Intelligence Extension) for SIMD operations
static inline void IRAM_ATTR enable_pie_once() {
    static bool pie_enabled = false;
    if (!pie_enabled) {
        asm volatile (
            "csrsi  0x7f2, 0b01        \n\t"  // Enable PIE CSR
            "li     x29, 0b10          \n\t"  // Configure XACC mode
            "esp.movx.w.cfg x29        \n\t"
            ::: "x29"
        );
        pie_enabled = true;
    }
}

// Hardware accelerated Int8 MAC using ESP32-P4 PIE
extern "C"
int32_t fc_dot_s8_pie(const int8_t *input, const int8_t *filter, int32_t row_len)
{
    int32_t result = 0;
    int32_t idx = 0;

    if (row_len >= 32) {
        asm volatile (
            "esp.zero.xacc                          \n\t"
            "mv     x30, %[in]                      \n\t"
            "mv     x31, %[flt]                     \n\t"
            "li     %[idx], 32                      \n\t"
            "addi   s7, %[len], -31                 \n\t"
            "esp.vld.128.ip  q0, x30, 16            \n\t"
            "esp.vld.128.ip  q2, x30, 16            \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "esp.vld.128.ip  q3, x31, 16            \n\t"
            "j      2f                              \n\t"
            "1:                                     \n\t"
            "esp.vmulas.s8.xacc.ld.ip q0, x30, 16, q0, q1 \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "esp.vmulas.s8.xacc.ld.ip q2, x30, 16, q2, q3 \n\t"
            "esp.vld.128.ip  q3, x31, 16            \n\t"
            "addi   %[idx], %[idx], 32              \n\t"
            "2:                                     \n\t"
            "blt    %[idx], s7, 1b                  \n\t"
            "esp.vmulas.s8.xacc  q0, q1             \n\t"
            "esp.vmulas.s8.xacc  q2, q3             \n\t"
            "addi   s7, %[len], -15                 \n\t"
            "bge    %[idx], s7, 3f                  \n\t"
            "esp.vld.128.ip  q0, x30, 16            \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "esp.vmulas.s8.xacc  q0, q1             \n\t"
            "addi   %[idx], %[idx], 16              \n\t"
            "3:                                     \n\t"
            "esp.movx.r.xacc.l   x30                \n\t"
            "mv     %[res], x30                     \n\t"
            : [idx] "+r"(idx), [res] "=r"(result)
            : [in] "r"(input), [flt] "r"(filter), [len] "r"(row_len)
            : "x30", "x31", "s7"
        );
    } else if (row_len >= 16) {
        asm volatile (
            "esp.zero.xacc                          \n\t"
            "mv     x30, %[in]                      \n\t"
            "mv     x31, %[flt]                     \n\t"
            "li     %[idx], 16                      \n\t"
            "addi   s7, %[len], -15                 \n\t"
            "esp.vld.128.ip  q0, x30, 16            \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "j      5f                              \n\t"
            "4:                                     \n\t"
            "esp.vmulas.s8.xacc.ld.ip q0, x30, 16, q0, q1 \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "addi   %[idx], %[idx], 16              \n\t"
            "5:                                     \n\t"
            "blt    %[idx], s7, 4b                  \n\t"
            "esp.vmulas.s8.xacc  q0, q1             \n\t"
            "esp.movx.r.xacc.l   x30                \n\t"
            "mv     %[res], x30                     \n\t"
            : [idx] "+r"(idx), [res] "=r"(result)
            : [in] "r"(input), [flt] "r"(filter), [len] "r"(row_len)
            : "x30", "x31", "s7"
        );
    }

    for (; idx < row_len; idx++) {
        result += (int32_t)input[idx] * (int32_t)filter[idx];
    }

    return result;
}

// Hardware accelerated Int16 MAC using ESP32-P4 PIE
// Processes 8 INT16 per 128-bit vector (vs 16 INT8)
extern "C"
int32_t fc_dot_s16_pie(const int16_t *input, const int16_t *filter, int32_t row_len)
{
    int32_t result = 0;
    int32_t idx = 0;

    if (row_len >= 16) {
        // Double-pumped: process 16 INT16 per iteration (2x 8-element vectors)
        asm volatile (
            "esp.zero.xacc                          \n\t"
            "mv     x30, %[in]                      \n\t"
            "mv     x31, %[flt]                     \n\t"
            "li     %[idx], 16                      \n\t"
            "addi   s7, %[len], -15                 \n\t"

            /* Prime the pipeline: load first 16 INT16 (32 bytes) */
            "esp.vld.128.ip  q0, x30, 16            \n\t"  // input[0:7]
            "esp.vld.128.ip  q2, x30, 16            \n\t"  // input[8:15]
            "esp.vld.128.ip  q1, x31, 16            \n\t"  // filter[0:7]
            "esp.vld.128.ip  q3, x31, 16            \n\t"  // filter[8:15]
            "j      2f                              \n\t"

            "1:                                     \n\t"
            /* MAC + load next input/filter pair */
            "esp.vmulas.s16.xacc.ld.ip q0, x30, 16, q0, q1 \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "esp.vmulas.s16.xacc.ld.ip q2, x30, 16, q2, q3 \n\t"
            "esp.vld.128.ip  q3, x31, 16            \n\t"
            "addi   %[idx], %[idx], 16              \n\t"

            "2:                                     \n\t"
            "blt    %[idx], s7, 1b                  \n\t"

            /* Drain pipeline: final two MACs */
            "esp.vmulas.s16.xacc  q0, q1            \n\t"
            "esp.vmulas.s16.xacc  q2, q3            \n\t"

            /* Handle 8-element remainder if any */
            "addi   s7, %[len], -7                  \n\t"
            "bge    %[idx], s7, 3f                  \n\t"
            "esp.vld.128.ip  q0, x30, 16            \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "esp.vmulas.s16.xacc  q0, q1            \n\t"
            "addi   %[idx], %[idx], 8               \n\t"

            "3:                                     \n\t"
            "esp.movx.r.xacc.l   x30                \n\t"
            "mv     %[res], x30                     \n\t"
            : [idx] "+r"(idx), [res] "=r"(result)
            : [in] "r"(input), [flt] "r"(filter), [len] "r"(row_len)
            : "x30", "x31", "s7"
        );
    } else if (row_len >= 8) {
        // Single-pumped for 8-15 element rows
        asm volatile (
            "esp.zero.xacc                          \n\t"
            "mv     x30, %[in]                      \n\t"
            "mv     x31, %[flt]                     \n\t"
            "li     %[idx], 8                       \n\t"
            "addi   s7, %[len], -7                  \n\t"
            "esp.vld.128.ip  q0, x30, 16            \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "j      5f                              \n\t"
            "4:                                     \n\t"
            "esp.vmulas.s16.xacc.ld.ip q0, x30, 16, q0, q1 \n\t"
            "esp.vld.128.ip  q1, x31, 16            \n\t"
            "addi   %[idx], %[idx], 8               \n\t"
            "5:                                     \n\t"
            "blt    %[idx], s7, 4b                  \n\t"
            "esp.vmulas.s16.xacc  q0, q1            \n\t"
            "esp.movx.r.xacc.l   x30                \n\t"
            "mv     %[res], x30                     \n\t"
            : [idx] "+r"(idx), [res] "=r"(result)
            : [in] "r"(input), [flt] "r"(filter), [len] "r"(row_len)
            : "x30", "x31", "s7"
        );
    }

    /* Scalar remainder */
    for (; idx < row_len; idx++) {
        result += (int32_t)input[idx] * (int32_t)filter[idx];
    }

    return result;
}
#else
#define esp_firevad_LOGI(fmt, ...) printf("[FRVD INFO] " fmt "\n", ##__VA_ARGS__)
#define esp_firevad_LOGE(fmt, ...) fprintf(stderr, "[FRVD ERROR] " fmt "\n", ##__VA_ARGS__)
static void* esp_firevad_malloc(int version, size_t size) { return malloc(size); }
#define esp_firevad_free_ptr(ptr) free(ptr)
#endif

// ---- Math primitives using ESP-DSP ----

// Hardware-accelerated INT16 dot product using ESP32-P4 PIE SIMD
// Replaces slow chunked ESP-DSP approach with fast PIE assembly
static inline int32_t IRAM_ATTR dense_dot_s16_esp_dsp(const int16_t* a, const int16_t* b, uint32_t len) {
#ifdef ESP_PLATFORM
    enable_pie_once();  // Ensure PIE is enabled
    return fc_dot_s16_pie(a, b, len);
#else
    int32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum += (int32_t)a[i] * (int32_t)b[i];
    }
    return sum;
#endif
}

// ESP-DSP FP32 dot product wrapper
static inline float IRAM_ATTR dense_dot_f32_esp_dsp(const float* a, const float* b, uint32_t len) {
#ifdef ESP_PLATFORM
    float sum = 0.0f;
    dsps_dotprod_f32(a, b, &sum, len);
    return sum;
#else
    float sum = 0.0f;
    for (uint32_t i = 0; i < len; i++) {
        sum += a[i] * b[i];
    }
    return sum;
#endif
}

static void IRAM_ATTR dense_forward(const DenseLayer* layer, const float* input, float* output, const EspFirevadModel* model) {
    const uint32_t in_dim = layer->in_dim;
    const uint32_t out_dim = layer->out_dim;

    if (model->is_int8) {
        float max_in = 0.0f;
        for (uint32_t i = 0; i < in_dim; i++) {
            float a = std::abs(input[i]);
            if (a > max_in) max_in = a;
        }
        float in_scale = (max_in > 0.0f) ? (max_in / 127.0f) : 1.0f;
        float inv_scale = 1.0f / in_scale;

        int8_t* in_q = tcm_scratch_in_q;
        const int8_t* W = (const int8_t*)layer->weight;

        // Quantize input ONCE (not per output!)
        for (uint32_t i = 0; i < in_dim; i++) {
            float val = input[i] * inv_scale;
            int32_t q = (int32_t)(val + (val >= 0.0f ? 0.5f : -0.5f));
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            in_q[i] = (int8_t)q;
        }

        // SESSION 17: PHASE 1 - Memory prefetching optimization
        for (uint32_t o = 0; o < out_dim; o++) {
            const int8_t* __restrict__ row = W + o * in_dim;
            
            // OPTIMIZATION: Prefetch next weight row (hide PSRAM latency!)
            #ifdef ESP_PLATFORM
            #define PREFETCH_DISTANCE 4
            if (o + PREFETCH_DISTANCE < out_dim) {
                __builtin_prefetch(W + (o + PREFETCH_DISTANCE) * in_dim, 0, 3);
            }
            #endif
            
            int32_t sum = 0;
            // SESSION 19: Disabled buggy PIE assembly (fc_dot_s8_pie) because it only reads 1/4th of the accumulator (xacc.l)!
            // Using standard C loop for 100% mathematical precision.
            for (uint32_t i = 0; i < in_dim; i++) {
                sum += (int32_t)row[i] * (int32_t)in_q[i];
            }

            float out_scale = in_scale;
            if (layer->channel_scales) {
                out_scale *= layer->channel_scales[o];
            } else {
                out_scale *= layer->weight_scale;
            }

            float out_f = (float)sum * out_scale;
            if (layer->bias != nullptr) {
                if (model->is_int8_per_ch) {
                    const float* B = (const float*)layer->bias;
                    out_f += B[o];
                } else {
                    const int8_t* B = (const int8_t*)layer->bias;
                    out_f += (float)B[o] * layer->bias_scale;
                }
            }
            output[o] = out_f;
        }
    } else if (model->is_int16) {
        // SESSION 17: PHASE 1 - Memory prefetching for INT16 path
        float max_in = 0.0f;
        for (uint32_t i = 0; i < in_dim; i++) {
            float a = std::abs(input[i]);
            if (a > max_in) max_in = a;
        }
        float in_scale = (max_in > 0.0f) ? (max_in / 1023.0f) : 1.0f;
        float inv_scale = 1.0f / in_scale;

#ifdef ESP_PLATFORM
        int16_t* in_q = tcm_scratch_in_q16;
#else
        int16_t* in_q = model->scratch_in_q16;
#endif
        // NOTE: INT16 quantization kept simple (Session 12 analysis)
        for (uint32_t i = 0; i < in_dim; i++) {
            float val = input[i] * inv_scale;
            int32_t q = (int32_t)(val + (val >= 0.0f ? 0.5f : -0.5f));
            if (q > 1023) q = 1023;
            else if (q < -1023) q = -1023;
            in_q[i] = (int16_t)q;
        }

        const int16_t* W = (const int16_t*)layer->weight;
        const int16_t* B = (const int16_t*)layer->bias;
        const float out_scale = in_scale * layer->weight_scale;

        for (uint32_t o = 0; o < out_dim; o++) {
            // OPTIMIZATION: Memory prefetch for INT16 weights
            #ifdef ESP_PLATFORM
            if (o + PREFETCH_DISTANCE < out_dim) {
                __builtin_prefetch(W + (o + PREFETCH_DISTANCE) * in_dim, 0, 3);
            }
            #endif
            
            const int16_t* __restrict__ row = W + o * in_dim;
            int32_t sum = dense_dot_s16_esp_dsp(row, in_q, in_dim);
            float sum_f = (float)sum * out_scale;
            if (B != nullptr) sum_f += (float)B[o] * layer->bias_scale;
            output[o] = sum_f;
        }
    } else {
        // SESSION 17: PHASE 1 - Memory prefetching for FP32 path
        const float* W = (const float*)layer->weight;
        const float* B = (const float*)layer->bias;
        for (uint32_t o = 0; o < out_dim; o++) {
            // OPTIMIZATION: Memory prefetch for FP32 weights
            #ifdef ESP_PLATFORM
            if (o + PREFETCH_DISTANCE < out_dim) {
                __builtin_prefetch(W + (o + PREFETCH_DISTANCE) * in_dim, 0, 3);
            }
            #endif
            
            const float* row = W + o * in_dim;
            float sum = dense_dot_f32_esp_dsp(row, input, in_dim);
            if (B != nullptr) sum += B[o];
            output[o] = sum;
        }
    }
}

// OPTIMIZATION: Fused Dense + ReLU operation
// Eliminates intermediate memory write/read by applying ReLU during dense computation
static void IRAM_ATTR dense_relu_forward(const DenseLayer* layer, const float* input, float* output, const EspFirevadModel* model) {
    const uint32_t in_dim = layer->in_dim;
    const uint32_t out_dim = layer->out_dim;

    if (model->is_int8) {
        // INT8 path - compute with ReLU fusion
        float max_in = 0.0f;
        for (uint32_t i = 0; i < in_dim; i++) {
            float a = std::abs(input[i]);
            if (a > max_in) max_in = a;
        }
        float in_scale = (max_in > 0.0f) ? (max_in / 127.0f) : 1.0f;
        float inv_scale = 1.0f / in_scale;

        int8_t* in_q = tcm_scratch_in_q;
        const int8_t* W = (const int8_t*)layer->weight;

        for (uint32_t i = 0; i < in_dim; i++) {
            float val = input[i] * inv_scale;
            int32_t q = (int32_t)(val + (val >= 0.0f ? 0.5f : -0.5f));
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            in_q[i] = (int8_t)q;
        }

        for (uint32_t o = 0; o < out_dim; o++) {
            const int8_t* __restrict__ row = W + o * in_dim;
            int32_t sum = 0;
            // SESSION 19: Disabled buggy PIE assembly (fc_dot_s8_pie) because it only reads 1/4th of the accumulator (xacc.l)!
            // Using standard C loop for 100% mathematical precision.
            for (uint32_t i = 0; i < in_dim; i++) {
                sum += (int32_t)row[i] * (int32_t)in_q[i];
            }

            float out_scale = in_scale;
            if (layer->channel_scales) {
                out_scale *= layer->channel_scales[o];
            } else {
                out_scale *= layer->weight_scale;
            }

            float out_f = (float)sum * out_scale;
            if (layer->bias != nullptr) {
                if (model->is_int8_per_ch) {
                    const float* B = (const float*)layer->bias;
                    out_f += B[o];
                } else {
                    const int8_t* B = (const int8_t*)layer->bias;
                    out_f += (float)B[o] * layer->bias_scale;
                }
            }
            // FUSED RELU: Apply immediately without storing negative values
            output[o] = (out_f > 0.0f) ? out_f : 0.0f;
        }
    } else if (model->is_int16) {
        // INT16 path - compute with ReLU fusion
        float max_in = 0.0f;
        for (uint32_t i = 0; i < in_dim; i++) {
            float a = std::abs(input[i]);
            if (a > max_in) max_in = a;
        }
        float in_scale = (max_in > 0.0f) ? (max_in / 1023.0f) : 1.0f;
        float inv_scale = 1.0f / in_scale;

#ifdef ESP_PLATFORM
        int16_t* in_q = tcm_scratch_in_q16;
#else
        int16_t* in_q = model->scratch_in_q16;
#endif
        for (uint32_t i = 0; i < in_dim; i++) {
            float val = input[i] * inv_scale;
            int32_t q = (int32_t)(val + (val >= 0.0f ? 0.5f : -0.5f));
            if (q > 1023) q = 1023;
            if (q < -1024) q = -1024;
            in_q[i] = (int16_t)q;
        }

        const int16_t* W = (const int16_t*)layer->weight;
        const float* B = (layer->bias != nullptr) ? (const float*)layer->bias : nullptr;
        float out_scale = in_scale * layer->weight_scale;

        for (uint32_t o = 0; o < out_dim; o++) {
            const int16_t* __restrict__ row = W + o * in_dim;
            int32_t sum = dense_dot_s16_esp_dsp(row, in_q, in_dim);
            float sum_f = (float)sum * out_scale;
            if (B != nullptr) sum_f += (float)B[o] * layer->bias_scale;
            // FUSED RELU: Apply immediately
            output[o] = (sum_f > 0.0f) ? sum_f : 0.0f;
        }
    } else {
        // FP32 path - compute with ReLU fusion
        const float* W = (const float*)layer->weight;
        const float* B = (layer->bias != nullptr) ? (const float*)layer->bias : nullptr;

        for (uint32_t o = 0; o < out_dim; o++) {
            const float* row = W + o * in_dim;
            float sum = dense_dot_f32_esp_dsp(row, input, in_dim);
            if (B != nullptr) sum += B[o];
            // FUSED RELU: Apply immediately
            output[o] = (sum > 0.0f) ? sum : 0.0f;
        }
    }
}

static float IRAM_ATTR sigmoid(float x) {
    if (x >= 0.0f) {
        float ez = expf(-x);
        return 1.0f / (1.0f + ez);
    }
    float ez = expf(x);
    return ez / (1.0f + ez);
}

// SESSION 18: SLIDING WINDOW CACHE OPTIMIZATION
// Original cache used ring buffers, now uses sequential access.
// We keep full window size (N1) for 100% mathematical equivalence to PyTorch.

static void IRAM_ATTR fsmn_memory_frame(
    const float* input, float* output, const FsmnFilter* filter, float* cache, uint32_t* cache_head_ptr,
    uint32_t P, uint32_t N1, uint32_t S1, uint32_t N2, uint32_t S2, uint32_t cache_len, int version) 
{
    float scale = filter->lookback_scale;
    if (version == 1) scale = 1.0f; // Float32 has scale baked into weights

    if (S1 == 1 && cache_len == (N1 > 1 ? N1 - 1 : 0) + N2 && cache_head_ptr != nullptr) {
        // SESSION 18: SLIDING WINDOW CACHE OPTIMIZATION + LOOKAHEAD
        
        float* __restrict__ out = output;
        const float* __restrict__ in = input;
        
        uint32_t lookback_len = (N1 > 1) ? N1 - 1 : 0;
        const float* current_frame = (N2 == 0) ? in : (cache + lookback_len * P);

        if (version == 1) {
            const float* __restrict__ w_lb = (const float*)filter->lookback_weight;
            
            for (uint32_t p = 0; p < P; p += 4) {
                out[p+0] = current_frame[p+0] + w_lb[p+0] * current_frame[p+0];
                out[p+1] = current_frame[p+1] + w_lb[p+1] * current_frame[p+1];
                out[p+2] = current_frame[p+2] + w_lb[p+2] * current_frame[p+2];
                out[p+3] = current_frame[p+3] + w_lb[p+3] * current_frame[p+3];
            }
            
            // Lookback
            for (uint32_t t = 1; t <= lookback_len; t++) {
                const float* __restrict__ wt = w_lb + t * P;
                const float* __restrict__ ct = cache + (lookback_len - t) * P;
                for (uint32_t p = 0; p < P; p += 4) {
                    out[p+0] += wt[p+0] * ct[p+0];
                    out[p+1] += wt[p+1] * ct[p+1];
                    out[p+2] += wt[p+2] * ct[p+2];
                    out[p+3] += wt[p+3] * ct[p+3];
                }
            }
            
            // Lookahead
            if (N2 > 0 && filter->lookahead_weight != nullptr) {
                const float* __restrict__ w_la = (const float*)filter->lookahead_weight;
                for (uint32_t j = 1; j <= N2; j++) {
                    const float* __restrict__ wj = w_la + (j - 1) * P;
                    const float* __restrict__ ct = (lookback_len + j == cache_len) ? in : (cache + (lookback_len + j) * P);
                    for (uint32_t p = 0; p < P; p += 4) {
                        out[p+0] += wj[p+0] * ct[p+0];
                        out[p+1] += wj[p+1] * ct[p+1];
                        out[p+2] += wj[p+2] * ct[p+2];
                        out[p+3] += wj[p+3] * ct[p+3];
                    }
                }
            }
        } else if (version == 2 || version == 4) { // Int8
            const int8_t* __restrict__ w_lb = (const int8_t*)filter->lookback_weight;
            for (uint32_t p = 0; p < P; p += 4) {
                out[p+0] = current_frame[p+0] + (float)w_lb[p+0] * filter->lookback_scale * current_frame[p+0];
                out[p+1] = current_frame[p+1] + (float)w_lb[p+1] * filter->lookback_scale * current_frame[p+1];
                out[p+2] = current_frame[p+2] + (float)w_lb[p+2] * filter->lookback_scale * current_frame[p+2];
                out[p+3] = current_frame[p+3] + (float)w_lb[p+3] * filter->lookback_scale * current_frame[p+3];
            }
            
            // Lookback
            for (uint32_t t = 1; t <= lookback_len; t++) {
                const int8_t* __restrict__ wt = w_lb + t * P;
                const float* __restrict__ ct = cache + (lookback_len - t) * P;
                for (uint32_t p = 0; p < P; p += 4) {
                    out[p+0] += (float)wt[p+0] * filter->lookback_scale * ct[p+0];
                    out[p+1] += (float)wt[p+1] * filter->lookback_scale * ct[p+1];
                    out[p+2] += (float)wt[p+2] * filter->lookback_scale * ct[p+2];
                    out[p+3] += (float)wt[p+3] * filter->lookback_scale * ct[p+3];
                }
            }
            
            // Lookahead
            if (N2 > 0 && filter->lookahead_weight != nullptr) {
                const int8_t* __restrict__ w_la = (const int8_t*)filter->lookahead_weight;
                for (uint32_t j = 1; j <= N2; j++) {
                    const int8_t* __restrict__ wj = w_la + (j - 1) * P;
                    const float* __restrict__ ct = (lookback_len + j == cache_len) ? in : (cache + (lookback_len + j) * P);
                    for (uint32_t p = 0; p < P; p += 4) {
                        out[p+0] += (float)wj[p+0] * filter->lookahead_scale * ct[p+0];
                        out[p+1] += (float)wj[p+1] * filter->lookahead_scale * ct[p+1];
                        out[p+2] += (float)wj[p+2] * filter->lookahead_scale * ct[p+2];
                        out[p+3] += (float)wj[p+3] * filter->lookahead_scale * ct[p+3];
                    }
                }
            }
        } else { // Int16
            const int16_t* __restrict__ w_lb = (const int16_t*)filter->lookback_weight;
            for (uint32_t p = 0; p < P; p += 4) {
                out[p+0] = current_frame[p+0] + (float)w_lb[p+0] * filter->lookback_scale * current_frame[p+0];
                out[p+1] = current_frame[p+1] + (float)w_lb[p+1] * filter->lookback_scale * current_frame[p+1];
                out[p+2] = current_frame[p+2] + (float)w_lb[p+2] * filter->lookback_scale * current_frame[p+2];
                out[p+3] = current_frame[p+3] + (float)w_lb[p+3] * filter->lookback_scale * current_frame[p+3];
            }
            
            // Lookback
            for (uint32_t t = 1; t <= lookback_len; t++) {
                const int16_t* __restrict__ wt = w_lb + t * P;
                const float* __restrict__ ct = cache + (lookback_len - t) * P;
                for (uint32_t p = 0; p < P; p += 4) {
                    out[p+0] += (float)wt[p+0] * filter->lookback_scale * ct[p+0];
                    out[p+1] += (float)wt[p+1] * filter->lookback_scale * ct[p+1];
                    out[p+2] += (float)wt[p+2] * filter->lookback_scale * ct[p+2];
                    out[p+3] += (float)wt[p+3] * filter->lookback_scale * ct[p+3];
                }
            }
            
            // Lookahead
            if (N2 > 0 && filter->lookahead_weight != nullptr) {
                const int16_t* __restrict__ w_la = (const int16_t*)filter->lookahead_weight;
                for (uint32_t j = 1; j <= N2; j++) {
                    const int16_t* __restrict__ wj = w_la + (j - 1) * P;
                    const float* __restrict__ ct = (lookback_len + j == cache_len) ? in : (cache + (lookback_len + j) * P);
                    for (uint32_t p = 0; p < P; p += 4) {
                        out[p+0] += (float)wj[p+0] * filter->lookahead_scale * ct[p+0];
                        out[p+1] += (float)wj[p+1] * filter->lookahead_scale * ct[p+1];
                        out[p+2] += (float)wj[p+2] * filter->lookahead_scale * ct[p+2];
                        out[p+3] += (float)wj[p+3] * filter->lookahead_scale * ct[p+3];
                    }
                }
            }
        }
        
        // SESSION 18: SLIDING WINDOW UPDATE
        if (cache_len > 0) {
            if (cache_len > 1) {
                memmove(cache, cache + P, (cache_len - 1) * P * sizeof(float));
            }
            memcpy(cache + (cache_len - 1) * P, in, P * sizeof(float));
        }
        *cache_head_ptr = 0;  // Always reset to 0
    } else {
        const float* w_f  = (version == 1) ? (const float*)filter->lookback_weight  : nullptr;
        const int8_t*  w_i8 = (version == 2 || version == 4) ? (const int8_t*)filter->lookback_weight  : nullptr;
        const int16_t* w_i16 = (version == 3) ? (const int16_t*)filter->lookback_weight : nullptr;
        for (uint32_t p = 0; p < P; p++) {
            float sum = 0.0f;
            if (w_f)  sum = w_f[p]  * input[p];
            else if (w_i8)  sum = (float)w_i8[p]  * input[p];
            else if (w_i16) sum = (float)w_i16[p] * input[p];
            for (uint32_t t = 1; t < N1; t++) {
                int32_t off = (int32_t)cache_len - (int32_t)(t * S1);
                if (off >= 0 && off < (int32_t)cache_len) {
                    const float* ch = cache + p * cache_len;
                    if (w_f)  sum += w_f[t*P+p]  * ch[off];
                    else if (w_i8)  sum += (float)w_i8[t*P+p]  * ch[off];
                    else if (w_i16) sum += (float)w_i16[t*P+p] * ch[off];
                }
            }
            output[p] = input[p] + sum * scale;
        }
        for (uint32_t p = 0; p < P; p++) {
            float* ch = cache + p * cache_len;
            if (cache_len > 0) {
                memmove(ch, ch + 1, (cache_len - 1) * sizeof(float));
                ch[cache_len - 1] = input[p];
            }
        }
    }
}

static void IRAM_ATTR apply_cmvn(const float* means, const float* istd, const float* input,
                        float* output, uint32_t dim) {
    // SESSION 14: Inline normalization - compiler auto-vectorizes
    // Removed ESP-DSP to fix INT8 regression: (input - mean) * inv_std
    for (uint32_t d = 0; d < dim; d++) {
        output[d] = (input[d] - means[d]) * istd[d];
    }
}

static uint32_t read_u32(const uint8_t* data, size_t offset) {
    return (uint32_t)data[offset]
         | ((uint32_t)data[offset + 1] << 8)
         | ((uint32_t)data[offset + 2] << 16)
         | ((uint32_t)data[offset + 3] << 24);
}

static void register_tensor_ptr(EspFirevadModel* model, void* ptr) {
    if (!ptr || !model) return;
    if (model->num_tensors >= model->max_tensors) {
        uint32_t new_max = model->max_tensors + 128;
        void** new_ptrs = nullptr;
#ifdef ESP_PLATFORM
        new_ptrs = (void**)heap_caps_realloc(model->tensor_ptrs, new_max * sizeof(void*), MALLOC_CAP_SPIRAM);
        if (!new_ptrs) {
            new_ptrs = (void**)heap_caps_realloc(model->tensor_ptrs, new_max * sizeof(void*), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
#else
        new_ptrs = (void**)realloc(model->tensor_ptrs, new_max * sizeof(void*));
#endif
        if (new_ptrs) {
            model->tensor_ptrs = new_ptrs;
            model->max_tensors = new_max;
        } else {
            esp_firevad_LOGE("Failed to grow tensor tracking array! Memory leak warning!");
            return;
        }
    }
    model->tensor_ptrs[model->num_tensors++] = ptr;
}

static const void* read_tensor(const uint8_t* data, size_t data_len, size_t* offset,
                               uint32_t* out_count, int version, float* out_scale, const float** out_channel_scales, EspFirevadModel* model) 
{
    if (*offset + 8 > data_len) return nullptr;
    *offset += 4;
    uint32_t count = read_u32(data, *offset);
    *offset += 4;
    if (out_count) *out_count = count;

    size_t data_bytes = 0;
    if (version == 4) {
        if (*offset + 4 > data_len) return nullptr;
        uint32_t num_channels = read_u32(data, *offset);
        *offset += 4;
        
        if (num_channels > 0) {
            // Per-channel quantized INT8
            if (out_channel_scales) {
                float* scales = nullptr;
#ifdef ESP_PLATFORM
                scales = (float*)heap_caps_aligned_alloc(16, num_channels * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!scales) scales = (float*)heap_caps_aligned_alloc(16, num_channels * sizeof(float), MALLOC_CAP_SPIRAM);
#else
                scales = (float*)malloc(num_channels * sizeof(float));
#endif
                if (scales) {
                    memcpy(scales, data + *offset, num_channels * sizeof(float));
                    register_tensor_ptr(model, scales);
                }
                *out_channel_scales = scales;
                if (out_scale) *out_scale = 1.0f;
            } else {
                if (out_scale) {
                    float scale;
                    memcpy(&scale, data + *offset, sizeof(float));
                    *out_scale = scale;
                }
            }
            *offset += num_channels * sizeof(float);
            data_bytes = count * sizeof(int8_t);
        } else {
            // Unquantized Float32 (for bias)
            if (out_scale) *out_scale = 1.0f;
            if (out_channel_scales) *out_channel_scales = nullptr;
            data_bytes = count * sizeof(float);
        }
    } else if (version == 2 || version == 3) {
        if (*offset + 4 > data_len) return nullptr;
        uint32_t s_bits = read_u32(data, *offset);
        if (out_scale) {
            float scale;
            memcpy(&scale, &s_bits, sizeof(float));
            *out_scale = scale;
        }
        if (out_channel_scales) *out_channel_scales = nullptr;
        *offset += 4;
        data_bytes = count * (version == 2 ? sizeof(int8_t) : sizeof(int16_t));
    } else {
        if (out_scale) *out_scale = 1.0f;
        if (out_channel_scales) *out_channel_scales = nullptr;
        data_bytes = count * sizeof(float);
    }

    if (*offset + data_bytes > data_len) return nullptr;

    const void* ptr = data + *offset;
    *offset += data_bytes;
    return ptr;
}

int esp_firevad_load(const uint8_t* data, size_t data_len, EspFirevadModel* model) {
    if (data == nullptr || model == nullptr) return -1;
    if (data_len < 68) return -2;

    memset(model, 0, sizeof(EspFirevadModel));

    if (data[0] != 'F' || data[1] != 'R' || data[2] != 'V' || data[3] != 'D') {
        esp_firevad_LOGE("Invalid magic"); return -3;
    }

    uint32_t version = read_u32(data, 4);
    if (version != 1 && version != 2 && version != 3 && version != 4) {
        esp_firevad_LOGE("Unsupported version: %u", version); return -4;
    }
    model->version = version;
    model->is_int8 = (version == 2 || version == 4);
    model->is_int16 = (version == 3);
    model->is_int8_per_ch = (version == 4);
    model->num_tensors = 0;
    model->max_tensors = 128;
#ifdef ESP_PLATFORM
    model->tensor_ptrs = (void**)heap_caps_malloc(model->max_tensors * sizeof(void*), MALLOC_CAP_SPIRAM);
    if (!model->tensor_ptrs) {
        model->tensor_ptrs = (void**)heap_caps_malloc(model->max_tensors * sizeof(void*), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
#else
    model->tensor_ptrs = (void**)malloc(model->max_tensors * sizeof(void*));
#endif
    if (!model->tensor_ptrs) {
        esp_firevad_LOGE("Failed to allocate tensor tracking array");
        return -5;
    }

    model->weight_buffer = (uint8_t*)const_cast<uint8_t*>(data);
    model->weight_buffer_size = data_len;
    model->owns_weight_buffer = false;
    model->chunk_scratch = nullptr;
    model->chunk_scratch_bytes = 0;

    const uint8_t* buf = model->weight_buffer;

    model->arch.R = read_u32(buf, 16);
    model->arch.M = read_u32(buf, 20);

    model->arch.D    = read_u32(buf, 32);
    model->arch.H    = read_u32(buf, 36);
    model->arch.P    = read_u32(buf, 40);
    model->arch.odim = read_u32(buf, 44);
    model->arch.N1   = read_u32(buf, 48);
    model->arch.S1   = read_u32(buf, 52);
    model->arch.N2   = read_u32(buf, 56);
    model->arch.S2   = read_u32(buf, 60);

    esp_firevad_LOGI("Loaded: D=%u H=%u P=%u odim=%u R=%u M=%u Int8=%d",
              model->arch.D, model->arch.H, model->arch.P, model->arch.odim,
              model->arch.R, model->arch.M, model->is_int8);

    size_t offset = 64;
    model->cmvn_dim = read_u32(buf, offset);
    offset += 4;

    if (model->cmvn_dim > 0) {
        model->cmvn_means = (const float*)(buf + offset);
        offset += model->cmvn_dim * sizeof(float);
        model->cmvn_istd = (const float*)(buf + offset);
        offset += model->cmvn_dim * sizeof(float);
    }

    const uint32_t D = model->arch.D;
    const uint32_t H = model->arch.H;
    const uint32_t P = model->arch.P;
    const uint32_t R = model->arch.R;
    const uint32_t M = model->arch.M;

    // SESSION 18: SLIDING WINDOW CACHE - sequential access, better cache locality
    // Full cache length used to maintain 100% PyTorch equivalence.
    uint32_t lookback_len = (model->arch.N1 > 1) ? (model->arch.N1 - 1) * model->arch.S1 : 0;
    uint32_t lookahead_len = (model->arch.N2 > 0) ? model->arch.N2 * model->arch.S2 : 0;
    uint32_t full_cache_len = lookback_len + lookahead_len;
    model->cache_len = full_cache_len;

    if (model->cache_len > 0) {
        model->fsmn_caches = (float**)heap_caps_aligned_alloc(16, R * sizeof(float*), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        model->fsmn_cache_heads = (uint32_t*)heap_caps_aligned_alloc(16, R * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        for (uint32_t r = 0; r < R; r++) {
            size_t cache_bytes = P * model->cache_len * sizeof(float);
            model->fsmn_caches[r] = (float*)heap_caps_aligned_alloc(16, cache_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!model->fsmn_caches[r]) {
                esp_firevad_LOGE("WARNING: FSMN cache block %u fell back to PSRAM!", r);
                model->fsmn_caches[r] = (float*)esp_firevad_malloc(model->version, cache_bytes);
            }
            memset(model->fsmn_caches[r], 0, cache_bytes);
            model->fsmn_cache_heads[r] = 0;
        }
        esp_firevad_LOGI("SESSION 18: FSMN sliding window cache: %u frames, %.1f KB per layer",
                        model->cache_len, 
                        (P * model->cache_len * sizeof(float)) / 1024.0f);
    }

    model->scratch_h    = (float*)heap_caps_aligned_alloc(16, H * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->scratch_p    = (float*)heap_caps_aligned_alloc(16, P * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->scratch_p2   = (float*)heap_caps_aligned_alloc(16, P * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->scratch_conv = (float*)heap_caps_aligned_alloc(16, P * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    uint32_t max_dim = (H > P) ? H : P;
    if (D > max_dim) max_dim = D;
    model->scratch_in_q = (int8_t*)esp_firevad_malloc(model->version, max_dim * sizeof(int8_t));
    model->scratch_in_q16 = nullptr;
    if (model->is_int16) {
        model->scratch_in_q16 = (int16_t*)esp_firevad_malloc(model->version, max_dim * sizeof(int16_t));
    }

    if (!model->scratch_h || !model->scratch_p || !model->scratch_p2 || !model->scratch_conv || !model->scratch_in_q || (model->is_int16 && !model->scratch_in_q16)) {
        esp_firevad_LOGE("Failed to allocate inference scratch buffers");
        esp_firevad_free(model);
        return -7;
    }

    uint32_t count = 0;
    model->fc1.weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->fc1.weight_scale, &model->fc1.channel_scales, model);
    model->fc1.in_dim = D;
    model->fc1.out_dim = H;
    model->fc1.bias = read_tensor(buf, data_len, &offset, &count, model->version, &model->fc1.bias_scale, nullptr, model);

    model->fc2.weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->fc2.weight_scale, &model->fc2.channel_scales, model);
    model->fc2.in_dim = H;
    model->fc2.out_dim = P;
    model->fc2.bias = read_tensor(buf, data_len, &offset, &count, model->version, &model->fc2.bias_scale, nullptr, model);

    model->fsmn1.lookback_weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->fsmn1.lookback_scale, nullptr, model);
    model->fsmn1.lookahead_weight = nullptr;
    if (model->arch.N2 > 0) {
        model->fsmn1.lookahead_weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->fsmn1.lookahead_scale, nullptr, model);
    }

    uint32_t num_blocks = R - 1;
    if (num_blocks > 0) {
        model->block_fc1  = (DenseLayer*)esp_firevad_malloc(model->version, num_blocks * sizeof(DenseLayer));
        model->block_fc2  = (DenseLayer*)esp_firevad_malloc(model->version, num_blocks * sizeof(DenseLayer));
        model->block_fsmn = (FsmnFilter*)esp_firevad_malloc(model->version, num_blocks * sizeof(FsmnFilter));

        for (uint32_t b = 0; b < num_blocks; b++) {
            model->block_fc1[b].weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->block_fc1[b].weight_scale, &model->block_fc1[b].channel_scales, model);
            model->block_fc1[b].in_dim = P;
            model->block_fc1[b].out_dim = H;
            model->block_fc1[b].bias = read_tensor(buf, data_len, &offset, &count, model->version, &model->block_fc1[b].bias_scale, nullptr, model);

            model->block_fc2[b].weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->block_fc2[b].weight_scale, &model->block_fc2[b].channel_scales, model);
            model->block_fc2[b].in_dim = H;
            model->block_fc2[b].out_dim = P;
            model->block_fc2[b].bias = nullptr;

            model->block_fsmn[b].lookback_weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->block_fsmn[b].lookback_scale, nullptr, model);
            model->block_fsmn[b].lookahead_weight = nullptr;
            if (model->arch.N2 > 0) {
                model->block_fsmn[b].lookahead_weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->block_fsmn[b].lookahead_scale, nullptr, model);
            }
        }
    }

    model->num_dnn_layers = M;
    if (M > 0) {
        model->dnn_layers = (DenseLayer*)esp_firevad_malloc(model->version, M * sizeof(DenseLayer));
        model->dnn_layers[0].weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->dnn_layers[0].weight_scale, &model->dnn_layers[0].channel_scales, model);
        model->dnn_layers[0].in_dim = P;
        model->dnn_layers[0].out_dim = H;
        model->dnn_layers[0].bias = read_tensor(buf, data_len, &offset, &count, model->version, &model->dnn_layers[0].bias_scale, nullptr, model);

        for (uint32_t d = 1; d < M; d++) {
            model->dnn_layers[d].weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->dnn_layers[d].weight_scale, &model->dnn_layers[d].channel_scales, model);
            model->dnn_layers[d].in_dim = H;
            model->dnn_layers[d].out_dim = H;
            model->dnn_layers[d].bias = read_tensor(buf, data_len, &offset, &count, model->version, &model->dnn_layers[d].bias_scale, nullptr, model);
        }
    }

    model->out.weight = read_tensor(buf, data_len, &offset, &count, model->version, &model->out.weight_scale, &model->out.channel_scales, model);
    model->out.in_dim = H;
    model->out.out_dim = model->arch.odim;
    model->out.bias = read_tensor(buf, data_len, &offset, &count, model->version, &model->out.bias_scale, nullptr, model);

    return 0;
}

void IRAM_ATTR __attribute__((aligned(16))) esp_firevad_infer_frame(EspFirevadModel* model, const float* features, bool apply_cmvn_flag, float* out_probs) {
    if (model == nullptr || features == nullptr) return;

    const uint32_t D = model->arch.D;
    float feat_buf[80]; 
    if (apply_cmvn_flag && model->cmvn_dim > 0) {
        apply_cmvn(model->cmvn_means, model->cmvn_istd, features, feat_buf, D);
        features = feat_buf;
    }

    // Delegate to the prepared inference function
    esp_firevad_infer_prepared(model, features, out_probs);
}

// DUAL-CORE API: Prepare features (CMVN normalization only)
void esp_firevad_prepare_features(EspFirevadModel* model, const float* features, bool apply_cmvn_flag, float* out_features) {
    if (model == nullptr || features == nullptr || out_features == nullptr) return;

    const uint32_t D = model->arch.D;
    
    if (apply_cmvn_flag && model->cmvn_dim > 0) {
        apply_cmvn(model->cmvn_means, model->cmvn_istd, features, out_features, D);
    } else {
        memcpy(out_features, features, D * sizeof(float));
    }
}

// DUAL-CORE API: Inference on pre-prepared features
void IRAM_ATTR esp_firevad_infer_prepared(EspFirevadModel* model, const float* features, float* out_probs) {
    if (model == nullptr || features == nullptr) return;

    const uint32_t H = model->arch.H;
    const uint32_t P = model->arch.P;
    const uint32_t R = model->arch.R;
    const uint32_t N1 = model->arch.N1;
    const uint32_t S1 = model->arch.S1;

    float* h = model->scratch_h;
    float* p = model->scratch_p;
    float* p2 = model->scratch_p2;
    float* conv_out = model->scratch_conv;

    int64_t t_dense = 0;
    int64_t t_fsmn = 0;
    uint32_t t0, t1;

    t0 = esp_cpu_get_cycle_count();
    dense_relu_forward(&model->fc1, features, h, model);
    t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);

    t0 = esp_cpu_get_cycle_count();
    dense_relu_forward(&model->fc2, h, p, model);
    t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);

    t0 = esp_cpu_get_cycle_count();
    fsmn_memory_frame(p, conv_out, &model->fsmn1, model->fsmn_caches[0], &model->fsmn_cache_heads[0], P, N1, S1, model->arch.N2, model->arch.S2, model->cache_len, model->version);
    t1 = esp_cpu_get_cycle_count(); t_fsmn += (t1 - t0);
    memcpy(p, conv_out, P * sizeof(float));

    for (uint32_t b = 0; b < R - 1; b++) {
        memcpy(p2, p, P * sizeof(float));
        t0 = esp_cpu_get_cycle_count();
        dense_relu_forward(&model->block_fc1[b], p, h, model);
        t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
        t0 = esp_cpu_get_cycle_count();
        dense_forward(&model->block_fc2[b], h, p, model);
        t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
        
        t0 = esp_cpu_get_cycle_count();
        fsmn_memory_frame(p, conv_out, &model->block_fsmn[b], model->fsmn_caches[b + 1], &model->fsmn_cache_heads[b + 1], P, N1, S1, model->arch.N2, model->arch.S2, model->cache_len, model->version);
        t1 = esp_cpu_get_cycle_count(); t_fsmn += (t1 - t0);
        for (uint32_t i = 0; i < P; i++) p[i] = conv_out[i] + p2[i];
    }

    if (model->num_dnn_layers > 0) {
        t0 = esp_cpu_get_cycle_count();
        dense_relu_forward(&model->dnn_layers[0], p, h, model);
        t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
        for (uint32_t d = 1; d < model->num_dnn_layers; d++) {
            float* temp = model->scratch_p; 
            t0 = esp_cpu_get_cycle_count();
            dense_relu_forward(&model->dnn_layers[d], h, temp, model);
            t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
            memcpy(h, temp, H * sizeof(float));
        }
    } else {
        memcpy(h, p, P * sizeof(float));
    }

    float logits[8] = {0};
    t0 = esp_cpu_get_cycle_count();
    dense_forward(&model->out, h, logits, model);
    t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
    
    
    if (out_probs) {
        uint32_t out_dim = (model->arch.odim < 8) ? model->arch.odim : 8;
        for (uint32_t o = 0; o < out_dim; o++) {
            out_probs[o] = sigmoid(logits[o]);
        }
    }
}

void esp_firevad_reset(EspFirevadModel* model) {
    if (model == nullptr) return;
    if (model->fsmn_caches != nullptr && model->cache_len > 0) {
        uint32_t P = model->arch.P;
        for (uint32_t r = 0; r < model->arch.R; r++) {
            if (model->fsmn_caches[r] != nullptr) {
                memset(model->fsmn_caches[r], 0, P * model->cache_len * sizeof(float));
            }
            if (model->fsmn_cache_heads != nullptr) {
                model->fsmn_cache_heads[r] = 0;
            }
        }
    }
}

void esp_firevad_free(EspFirevadModel* model) {
    if (model == nullptr) return;
    if (model->fsmn_caches != nullptr) {
        for (uint32_t r = 0; r < model->arch.R; r++) esp_firevad_free_ptr(model->fsmn_caches[r]);
        esp_firevad_free_ptr(model->fsmn_caches);
    }
    if (model->fsmn_cache_heads != nullptr) {
        esp_firevad_free_ptr(model->fsmn_cache_heads);
    }
    esp_firevad_free_ptr(model->block_fc1);
    esp_firevad_free_ptr(model->block_fc2);
    esp_firevad_free_ptr(model->block_fsmn);
    esp_firevad_free_ptr(model->dnn_layers);
    esp_firevad_free_ptr(model->scratch_h);
    esp_firevad_free_ptr(model->scratch_p);
    esp_firevad_free_ptr(model->scratch_p2);
    esp_firevad_free_ptr(model->scratch_conv);
    if (model->scratch_in_q) esp_firevad_free_ptr(model->scratch_in_q);
    if (model->scratch_in_q16) esp_firevad_free_ptr(model->scratch_in_q16);
    if (model->chunk_scratch) esp_firevad_free_ptr(model->chunk_scratch);

    // Free individually allocated tensors
    if (model->tensor_ptrs != nullptr) {
        for (uint32_t i = 0; i < model->num_tensors; i++) {
#ifdef ESP_PLATFORM
            heap_caps_free(model->tensor_ptrs[i]);
#else
            free(model->tensor_ptrs[i]);
#endif
        }
        esp_firevad_free_ptr(model->tensor_ptrs);
    }
    model->num_tensors = 0;
    model->max_tensors = 0;

    if (model->owns_weight_buffer && model->weight_buffer) {
        esp_firevad_free_ptr(model->weight_buffer);
    }
    memset(model, 0, sizeof(EspFirevadModel));
}

size_t esp_firevad_memory_usage(const EspFirevadModel* model) {
    if (model == nullptr) return 0;
    size_t total = 0;
    if (model->weight_buffer) {
        total += model->weight_buffer_size;
    }
    if (model->tensor_ptrs) {
        total += model->max_tensors * sizeof(void*);
    }
    if (model->chunk_scratch) {
        total += model->chunk_scratch_bytes;
    }
    if (model->cache_len > 0) {
        total += model->arch.R * sizeof(float*);
        total += model->arch.R * sizeof(uint32_t);
        total += model->arch.R * model->arch.P * model->cache_len * sizeof(float);
    }
    uint32_t num_blocks = (model->arch.R > 1) ? model->arch.R - 1 : 0;
    total += num_blocks * (sizeof(DenseLayer) * 2 + sizeof(FsmnFilter));
    total += model->num_dnn_layers * sizeof(DenseLayer);
    total += (model->arch.H + model->arch.P * 3) * sizeof(float);
    
    uint32_t max_dim = (model->arch.H > model->arch.P) ? model->arch.H : model->arch.P;
    if (model->arch.D > max_dim) max_dim = model->arch.D;
    total += max_dim * sizeof(int8_t);
    if (model->scratch_in_q16) {
        total += max_dim * sizeof(int16_t);
    }
    return total;
}

void esp_firevad_infer_chunk(EspFirevadModel* model, const float* features, uint32_t num_frames, bool apply_cmvn_flag, float* out_probs) {
    if (model == nullptr || features == nullptr || num_frames == 0) return;

    const uint32_t D = model->arch.D;
    const uint32_t H = model->arch.H;
    const uint32_t P = model->arch.P;
    const uint32_t R = model->arch.R;
    const uint32_t N1 = model->arch.N1;
    const uint32_t S1 = model->arch.S1;
    const uint32_t N2 = model->arch.N2;
    const uint32_t S2 = model->arch.S2;

    float* feat_buf = NULL;
    float* h_buf = NULL;
    float* p_buf = NULL;
    float* p2_buf = NULL;
    float* conv_out = NULL;
    float* batch_buf = NULL;
    bool batch_allocated = false;
    bool using_chunk_scratch = false;

    size_t total_floats = (size_t)num_frames * (D + H + P * 3);
    size_t required_bytes = total_floats * sizeof(float);
    if (model->chunk_scratch_bytes >= required_bytes) {
        batch_buf = model->chunk_scratch;
        using_chunk_scratch = true;
    } else {
        float* new_chunk = (float*)esp_firevad_malloc(model->version, required_bytes);
        if (new_chunk) {
            if (model->chunk_scratch) {
                esp_firevad_free_ptr(model->chunk_scratch);
            }
            model->chunk_scratch = new_chunk;
            model->chunk_scratch_bytes = required_bytes;
            batch_buf = model->chunk_scratch;
            using_chunk_scratch = true;
        }
    }

    if (batch_buf) {
        batch_allocated = true;
        feat_buf = batch_buf;
        h_buf = feat_buf + (size_t)num_frames * D;
        p_buf = h_buf + (size_t)num_frames * H;
        p2_buf = p_buf + (size_t)num_frames * P;
        conv_out = p2_buf + (size_t)num_frames * P;
    } else {
        feat_buf = (float*)esp_firevad_malloc(model->version, num_frames * D * sizeof(float));
        h_buf = (float*)esp_firevad_malloc(model->version, num_frames * H * sizeof(float));
        p_buf = (float*)esp_firevad_malloc(model->version, num_frames * P * sizeof(float));
        p2_buf = (float*)esp_firevad_malloc(model->version, num_frames * P * sizeof(float));
        conv_out = (float*)esp_firevad_malloc(model->version, num_frames * P * sizeof(float));
    }

    if (!feat_buf || !h_buf || !p_buf || !p2_buf || !conv_out) {
        if (batch_allocated) esp_firevad_free_ptr(batch_buf);
        else {
            if (feat_buf) esp_firevad_free_ptr(feat_buf);
            if (h_buf) esp_firevad_free_ptr(h_buf);
            if (p_buf) esp_firevad_free_ptr(p_buf);
            if (p2_buf) esp_firevad_free_ptr(p2_buf);
            if (conv_out) esp_firevad_free_ptr(conv_out);
        }
        return;
    }

    if (apply_cmvn_flag && model->cmvn_dim > 0) {
        for (uint32_t t = 0; t < num_frames; t++) {
            apply_cmvn(model->cmvn_means, model->cmvn_istd, features + t * D, feat_buf + t * D, D);
        }
    } else {
        memcpy(feat_buf, features, num_frames * D * sizeof(float));
    }

    // 1st FSMN block
    for (uint32_t t = 0; t < num_frames; t++) {
        dense_relu_forward(&model->fc1, feat_buf + t * D, h_buf + t * H, model);
        dense_relu_forward(&model->fc2, h_buf + t * H, p_buf + t * P, model);
    }

    auto apply_fsmn_chunk = [&](const FsmnFilter* filter, float* in_p, float* out_p) {
        for (uint32_t t = 0; t < num_frames; t++) {
            // Initialize out_p with in_p
            for (uint32_t p = 0; p < P; p++) {
                out_p[t * P + p] = in_p[t * P + p];
            }
            
            // Lookback
            if (model->version == 1) {
                const float* fw = (const float*)filter->lookback_weight;
                for (uint32_t p = 0; p < P; p++) out_p[t * P + p] += fw[p] * in_p[t * P + p];
                
                for (uint32_t n = 1; n < N1; n++) {
                    int32_t offset = (int32_t)t - (int32_t)(n * S1);
                    if (offset >= 0) {
                        const float* w_row = fw + n * P;
                        const float* in_row = in_p + offset * P;
                        for (uint32_t p = 0; p < P; p++) out_p[t * P + p] += w_row[p] * in_row[p];
                    }
                }
            } else if (model->version == 2 || model->version == 4) {
                const int8_t* fw = (const int8_t*)filter->lookback_weight;
                float scale = filter->lookback_scale;
                for (uint32_t p = 0; p < P; p++) out_p[t * P + p] += (float)fw[p] * scale * in_p[t * P + p];
                
                for (uint32_t n = 1; n < N1; n++) {
                    int32_t offset = (int32_t)t - (int32_t)(n * S1);
                    if (offset >= 0) {
                        const int8_t* w_row = fw + n * P;
                        const float* in_row = in_p + offset * P;
                        for (uint32_t p = 0; p < P; p += 4) {
                            out_p[t * P + p + 0] += (float)w_row[p + 0] * scale * in_row[p + 0];
                            out_p[t * P + p + 1] += (float)w_row[p + 1] * scale * in_row[p + 1];
                            out_p[t * P + p + 2] += (float)w_row[p + 2] * scale * in_row[p + 2];
                            out_p[t * P + p + 3] += (float)w_row[p + 3] * scale * in_row[p + 3];
                        }
                    }
                }
            } else if (model->version == 3) {
                const int16_t* fw = (const int16_t*)filter->lookback_weight;
                float scale = filter->lookback_scale;
                for (uint32_t p = 0; p < P; p++) out_p[t * P + p] += (float)fw[p] * scale * in_p[t * P + p];
                
                for (uint32_t n = 1; n < N1; n++) {
                    int32_t offset = (int32_t)t - (int32_t)(n * S1);
                    if (offset >= 0) {
                        const int16_t* w_row = fw + n * P;
                        const float* in_row = in_p + offset * P;
                        for (uint32_t p = 0; p < P; p += 4) {
                            out_p[t * P + p + 0] += (float)w_row[p + 0] * scale * in_row[p + 0];
                            out_p[t * P + p + 1] += (float)w_row[p + 1] * scale * in_row[p + 1];
                            out_p[t * P + p + 2] += (float)w_row[p + 2] * scale * in_row[p + 2];
                            out_p[t * P + p + 3] += (float)w_row[p + 3] * scale * in_row[p + 3];
                        }
                    }
                }
            }

            // Lookahead
            if (N2 > 0 && num_frames > 1 && filter->lookahead_weight != nullptr) {
                if (model->version == 1) {
                    const float* fw = (const float*)filter->lookahead_weight;
                    for (uint32_t n = 0; n < N2; n++) {
                        int32_t offset = (int32_t)t + (int32_t)((n + 1) * S2);
                        if (offset < (int32_t)num_frames) {
                            const float* w_row = fw + n * P;
                            const float* in_row = in_p + offset * P;
                            for (uint32_t p = 0; p < P; p++) out_p[t * P + p] += w_row[p] * in_row[p];
                        }
                    }
                } else if (model->version == 2 || model->version == 4) {
                    const int8_t* fw = (const int8_t*)filter->lookahead_weight;
                    float scale = filter->lookahead_scale;
                    for (uint32_t n = 0; n < N2; n++) {
                        int32_t offset = (int32_t)t + (int32_t)((n + 1) * S2);
                        if (offset < (int32_t)num_frames) {
                            const int8_t* w_row = fw + n * P;
                            const float* in_row = in_p + offset * P;
                            for (uint32_t p = 0; p < P; p += 4) {
                                out_p[t * P + p + 0] += (float)w_row[p + 0] * scale * in_row[p + 0];
                                out_p[t * P + p + 1] += (float)w_row[p + 1] * scale * in_row[p + 1];
                                out_p[t * P + p + 2] += (float)w_row[p + 2] * scale * in_row[p + 2];
                                out_p[t * P + p + 3] += (float)w_row[p + 3] * scale * in_row[p + 3];
                            }
                        }
                    }
                } else if (model->version == 3) {
                    const int16_t* fw = (const int16_t*)filter->lookahead_weight;
                    float scale = filter->lookahead_scale;
                    for (uint32_t n = 0; n < N2; n++) {
                        int32_t offset = (int32_t)t + (int32_t)((n + 1) * S2);
                        if (offset < (int32_t)num_frames) {
                            const int16_t* w_row = fw + n * P;
                            const float* in_row = in_p + offset * P;
                            for (uint32_t p = 0; p < P; p += 4) {
                                out_p[t * P + p + 0] += (float)w_row[p + 0] * scale * in_row[p + 0];
                                out_p[t * P + p + 1] += (float)w_row[p + 1] * scale * in_row[p + 1];
                                out_p[t * P + p + 2] += (float)w_row[p + 2] * scale * in_row[p + 2];
                                out_p[t * P + p + 3] += (float)w_row[p + 3] * scale * in_row[p + 3];
                            }
                        }
                    }
                }
            }
        }
    };

    apply_fsmn_chunk(&model->fsmn1, p_buf, conv_out);
    memcpy(p_buf, conv_out, num_frames * P * sizeof(float));

    for (uint32_t b = 0; b < R - 1; b++) {
        memcpy(p2_buf, p_buf, num_frames * P * sizeof(float));
        for (uint32_t t = 0; t < num_frames; t++) {
            dense_relu_forward(&model->block_fc1[b], p_buf + t * P, h_buf + t * H, model);
            dense_forward(&model->block_fc2[b], h_buf + t * H, p_buf + t * P, model);
        }
        apply_fsmn_chunk(&model->block_fsmn[b], p_buf, conv_out);
        for (uint32_t t = 0; t < num_frames; t++) {
            for (uint32_t i = 0; i < P; i++) {
                p_buf[t * P + i] = conv_out[t * P + i] + p2_buf[t * P + i];
            }
        }
    }

    if (model->num_dnn_layers > 0) {
        for (uint32_t t = 0; t < num_frames; t++) {
            dense_relu_forward(&model->dnn_layers[0], p_buf + t * P, h_buf + t * H, model);
        }
        for (uint32_t d = 1; d < model->num_dnn_layers; d++) {
            for (uint32_t t = 0; t < num_frames; t++) {
                float* temp = model->scratch_p; 
                dense_relu_forward(&model->dnn_layers[d], h_buf + t * H, temp, model);
                memcpy(h_buf + t * H, temp, H * sizeof(float));
            }
        }
    } else {
        for (uint32_t t = 0; t < num_frames; t++) {
            memcpy(h_buf + t * H, p_buf + t * P, P * sizeof(float));
        }
    }

    float logits[8] = {0};
    uint32_t out_dim = (model->arch.odim < 8) ? model->arch.odim : 8;

    for (uint32_t t = 0; t < num_frames; t++) {
        dense_forward(&model->out, h_buf + t * H, logits, model);
        if (out_probs) {
            for (uint32_t o = 0; o < out_dim; o++) {
                out_probs[t * model->arch.odim + o] = sigmoid(logits[o]);
            }
        }
    }

    if (!using_chunk_scratch) {
        if (batch_buf) esp_firevad_free_ptr(batch_buf);
        else {
            esp_firevad_free_ptr(feat_buf);
            esp_firevad_free_ptr(h_buf);
            esp_firevad_free_ptr(p_buf);
            esp_firevad_free_ptr(p2_buf);
            esp_firevad_free_ptr(conv_out);
        }
    }
}

