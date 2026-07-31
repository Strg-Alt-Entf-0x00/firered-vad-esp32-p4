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
#include "dsps_dotprod.h"

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
#else
#define esp_firevad_LOGI(fmt, ...) printf("[FRVD INFO] " fmt "\n", ##__VA_ARGS__)
#define esp_firevad_LOGE(fmt, ...) fprintf(stderr, "[FRVD ERROR] " fmt "\n", ##__VA_ARGS__)
static void* esp_firevad_malloc(int version, size_t size) { return malloc(size); }
#define esp_firevad_free_ptr(ptr) free(ptr)
#endif

// ---- Math primitives ----

static void IRAM_ATTR dense_forward(const DenseLayer* layer, const float* input, float* output, const EspFirevadModel* model) {
    if (model->is_int8) {
        float max_in = 0.0f;
        for (uint32_t i = 0; i < layer->in_dim; i++) {
            float a = std::abs(input[i]);
            if (a > max_in) max_in = a;
        }
        float in_scale = (max_in > 0.0f) ? (max_in / 127.0f) : 1.0f;
        float inv_scale = 1.0f / in_scale;
        
        int8_t* in_q = tcm_scratch_in_q;
        
        const int8_t* W = (const int8_t*)layer->weight;
        
        static int align_print = 0;
        if (align_print++ < 2) {
            printf("DEBUG: W=%p (aligned=%d), in_q=%p (aligned=%d), channel_scales=%p\n", W, ((uintptr_t)W % 16 == 0), in_q, ((uintptr_t)in_q % 16 == 0), layer->channel_scales);
        }

        for (uint32_t i = 0; i < layer->in_dim; i++) {
            float val = input[i] * inv_scale;
            int32_t q = (int32_t)(val + (val >= 0.0f ? 0.5f : -0.5f));
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            in_q[i] = (int8_t)q;
        }

        for (uint32_t o = 0; o < layer->out_dim; o++) {
            const int8_t* __restrict__ row = W + o * layer->in_dim;
            assert(((uintptr_t)row % 16 == 0) && ((uintptr_t)in_q % 16 == 0));
            
            int32_t sum = 0;
#ifdef ESP_PLATFORM
            sum = fc_dot_s8_pie(in_q, row, (int32_t)layer->in_dim);
#else
            for (uint32_t i = 0; i < layer->in_dim; i++) {
                sum += (int32_t)row[i] * (int32_t)in_q[i];
            }
#endif
            
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
                    out_f += B[o]; // bias is unquantized float32 in v4
                } else {
                    const int8_t* B = (const int8_t*)layer->bias;
                    out_f += (float)B[o] * layer->bias_scale; // bias is int8 in v2
                }
            }
            output[o] = out_f;
        }
    } else if (model->is_int16) {
        float max_in = 0.0f;
        for (uint32_t i = 0; i < layer->in_dim; i++) {
            float a = std::abs(input[i]);
            if (a > max_in) max_in = a;
        }
        // Scale input to [-1023, 1023] (10-bit) to prevent PIE hardware overflow
        float in_scale = (max_in > 0.0f) ? (max_in / 1023.0f) : 1.0f;
        float inv_scale = 1.0f / in_scale;
        
#ifdef ESP_PLATFORM
        int16_t* in_q = tcm_scratch_in_q16;
#else
        int16_t* in_q = model->scratch_in_q16;
#endif
        for (uint32_t i = 0; i < layer->in_dim; i++) {
            float val = input[i] * inv_scale;
            int32_t q = (int32_t)(val + (val >= 0.0f ? 0.5f : -0.5f));
            if (q > 1023) q = 1023;
            else if (q < -1023) q = -1023;
            in_q[i] = (int16_t)q;
        }

        const int16_t* W = (const int16_t*)layer->weight;
        const int16_t* B = (const int16_t*)layer->bias;
        float out_scale = in_scale * layer->weight_scale;

        for (uint32_t o = 0; o < layer->out_dim; o++) {
            const int16_t* __restrict__ row = W + o * layer->in_dim;
            float sum_f = 0.0f;
            int64_t sum = 0;
            for (uint32_t i = 0; i < layer->in_dim; i++) {
                sum += (int32_t)row[i] * (int32_t)in_q[i];
            }
            sum_f = (float)sum * out_scale;
            if (B != nullptr) sum_f += (float)B[o] * layer->bias_scale;
            output[o] = sum_f;
        }
    } else {
        const float* W = (const float*)layer->weight;
        const float* B = (const float*)layer->bias;
        for (uint32_t o = 0; o < layer->out_dim; o++) {
            float sum = 0.0f;
            const float* row = W + o * layer->in_dim;
            for (uint32_t i = 0; i < layer->in_dim; i++) sum += row[i] * input[i];
            if (B != nullptr) sum += ((const float*)layer->bias)[o];
            output[o] = sum;
        }
    }
}

static void IRAM_ATTR relu_inplace(float* x, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (x[i] < 0.0f) x[i] = 0.0f;
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

static void IRAM_ATTR fsmn_lookback_frame(
    const float* input, float* output, const FsmnFilter* filter, float* cache, uint32_t* cache_head_ptr,
    uint32_t P, uint32_t N1, uint32_t S1, uint32_t cache_len, int version) 
{
    float scale = filter->lookback_scale;
    if (version == 1) scale = 1.0f; // Float32 has scale baked into weights

    if (S1 == 1 && cache_len == (N1 - 1) && cache_head_ptr != nullptr) {
        uint32_t head = *cache_head_ptr;
        float* __restrict__ out = output;
        const float* __restrict__ in = input;

        if (version == 1) {
            const float* __restrict__ w = (const float*)filter->lookback_weight;
            for (uint32_t p = 0; p < P; p += 4) {
                out[p+0] = in[p+0] + w[p+0] * in[p+0];
                out[p+1] = in[p+1] + w[p+1] * in[p+1];
                out[p+2] = in[p+2] + w[p+2] * in[p+2];
                out[p+3] = in[p+3] + w[p+3] * in[p+3];
            }
            int32_t curr_pos = (int32_t)head - 1;
            if (curr_pos < 0) curr_pos = (int32_t)cache_len - 1;
            for (uint32_t t = 1; t < N1; t++) {
                const float* __restrict__ wt = w + t * P;
                const float* __restrict__ ct = cache + curr_pos * P;
                for (uint32_t p = 0; p < P; p += 4) {
                    out[p+0] += wt[p+0] * ct[p+0];
                    out[p+1] += wt[p+1] * ct[p+1];
                    out[p+2] += wt[p+2] * ct[p+2];
                    out[p+3] += wt[p+3] * ct[p+3];
                }
                curr_pos--;
                if (curr_pos < 0) curr_pos = (int32_t)cache_len - 1;
            }
        } else if (version == 2 || version == 4) { // Int8
            const int8_t* __restrict__ w = (const int8_t*)filter->lookback_weight;
            for (uint32_t p = 0; p < P; p += 4) {
                out[p+0] = in[p+0] + (float)w[p+0] * scale * in[p+0];
                out[p+1] = in[p+1] + (float)w[p+1] * scale * in[p+1];
                out[p+2] = in[p+2] + (float)w[p+2] * scale * in[p+2];
                out[p+3] = in[p+3] + (float)w[p+3] * scale * in[p+3];
            }
            int32_t curr_pos = (int32_t)head - 1;
            if (curr_pos < 0) curr_pos = (int32_t)cache_len - 1;
            for (uint32_t t = 1; t < N1; t++) {
                const int8_t* __restrict__ wt = w + t * P;
                const float* __restrict__ ct = cache + curr_pos * P;
                for (uint32_t p = 0; p < P; p += 4) {
                    out[p+0] += (float)wt[p+0] * scale * ct[p+0];
                    out[p+1] += (float)wt[p+1] * scale * ct[p+1];
                    out[p+2] += (float)wt[p+2] * scale * ct[p+2];
                    out[p+3] += (float)wt[p+3] * scale * ct[p+3];
                }
                curr_pos--;
                if (curr_pos < 0) curr_pos = (int32_t)cache_len - 1;
            }
        } else { // Int16
            const int16_t* __restrict__ w = (const int16_t*)filter->lookback_weight;
            for (uint32_t p = 0; p < P; p += 4) {
                out[p+0] = in[p+0] + (float)w[p+0] * scale * in[p+0];
                out[p+1] = in[p+1] + (float)w[p+1] * scale * in[p+1];
                out[p+2] = in[p+2] + (float)w[p+2] * scale * in[p+2];
                out[p+3] = in[p+3] + (float)w[p+3] * scale * in[p+3];
            }
            int32_t curr_pos = (int32_t)head - 1;
            if (curr_pos < 0) curr_pos = (int32_t)cache_len - 1;
            for (uint32_t t = 1; t < N1; t++) {
                const int16_t* __restrict__ wt = w + t * P;
                const float* __restrict__ ct = cache + curr_pos * P;
                for (uint32_t p = 0; p < P; p += 4) {
                    out[p+0] += (float)wt[p+0] * scale * ct[p+0];
                    out[p+1] += (float)wt[p+1] * scale * ct[p+1];
                    out[p+2] += (float)wt[p+2] * scale * ct[p+2];
                    out[p+3] += (float)wt[p+3] * scale * ct[p+3];
                }
                curr_pos--;
                if (curr_pos < 0) curr_pos = (int32_t)cache_len - 1;
            }
        }

        float* __restrict__ ch_head = cache + head * P;
        memcpy(ch_head, in, P * sizeof(float));
        *cache_head_ptr = (head + 1 >= cache_len) ? 0 : (head + 1);
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

static void apply_cmvn(const float* means, const float* istd, const float* input,
                        float* output, uint32_t dim) {
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
            if (out_scale) {
                float first_scale;
                memcpy(&first_scale, data + *offset, sizeof(float));
                *out_scale = first_scale;
            }
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
                    if (model->num_tensors < 64) {
                        model->tensor_ptrs[model->num_tensors++] = scales;
                    }
                }
                *out_channel_scales = scales;
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

    void* ptr = nullptr;
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 100 * 1024) {
        ptr = heap_caps_aligned_alloc(16, data_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!ptr) ptr = heap_caps_aligned_alloc(16, data_bytes, MALLOC_CAP_SPIRAM);
#else
    ptr = malloc(data_bytes);
#endif

    if (ptr) {
        memcpy(ptr, data + *offset, data_bytes);
        if (model->num_tensors < 64) {
            model->tensor_ptrs[model->num_tensors++] = ptr;
        }
    }
    
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

    const uint8_t* buf = data;
    model->weight_buffer = nullptr;
    model->weight_buffer_size = data_len;

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

    // Allocate FSMN caches and scratch buffers FIRST, to guarantee they get internal SRAM
    // before the weights consume it all.
    model->cache_len = (model->arch.N1 > 1) ? (model->arch.N1 - 1) * model->arch.S1 : 0;
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

    uint32_t t_start = esp_cpu_get_cycle_count();
    const uint32_t D = model->arch.D;
    const uint32_t H = model->arch.H;
    const uint32_t P = model->arch.P;
    const uint32_t R = model->arch.R;
    const uint32_t N1 = model->arch.N1;
    const uint32_t S1 = model->arch.S1;

    float* h = model->scratch_h;
    float* p = model->scratch_p;
    float* p2 = model->scratch_p2;
    float* conv_out = model->scratch_conv;

    float feat_buf[80]; 
    if (apply_cmvn_flag && model->cmvn_dim > 0) {
        apply_cmvn(model->cmvn_means, model->cmvn_istd, features, feat_buf, D);
        features = feat_buf;
    }

    int64_t t_dense = 0;
    int64_t t_fsmn = 0;
    uint32_t t0, t1;

    t0 = esp_cpu_get_cycle_count();
    dense_forward(&model->fc1, features, h, model);
    t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
    relu_inplace(h, H);

    t0 = esp_cpu_get_cycle_count();
    dense_forward(&model->fc2, h, p, model);
    t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
    relu_inplace(p, P);

    t0 = esp_cpu_get_cycle_count();
    fsmn_lookback_frame(p, conv_out, &model->fsmn1, model->fsmn_caches[0], &model->fsmn_cache_heads[0], P, N1, S1, model->cache_len, model->version);
    t1 = esp_cpu_get_cycle_count(); t_fsmn += (t1 - t0);
    memcpy(p, conv_out, P * sizeof(float));

    for (uint32_t b = 0; b < R - 1; b++) {
        memcpy(p2, p, P * sizeof(float));
        t0 = esp_cpu_get_cycle_count();
        dense_forward(&model->block_fc1[b], p, h, model);
        t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
        relu_inplace(h, H);
        t0 = esp_cpu_get_cycle_count();
        dense_forward(&model->block_fc2[b], h, p, model);
        t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
        
        t0 = esp_cpu_get_cycle_count();
        fsmn_lookback_frame(p, conv_out, &model->block_fsmn[b], model->fsmn_caches[b + 1], &model->fsmn_cache_heads[b + 1], P, N1, S1, model->cache_len, model->version);
        t1 = esp_cpu_get_cycle_count(); t_fsmn += (t1 - t0);
        for (uint32_t i = 0; i < P; i++) p[i] = conv_out[i] + p2[i];
    }

    if (model->num_dnn_layers > 0) {
        t0 = esp_cpu_get_cycle_count();
        dense_forward(&model->dnn_layers[0], p, h, model);
        t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
        relu_inplace(h, H);
        for (uint32_t d = 1; d < model->num_dnn_layers; d++) {
            float* temp = model->scratch_p; 
            t0 = esp_cpu_get_cycle_count();
            dense_forward(&model->dnn_layers[d], h, temp, model);
            t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
            relu_inplace(temp, H);
            memcpy(h, temp, H * sizeof(float));
        }
    } else {
        memcpy(h, p, P * sizeof(float));
    }

    float logits[8] = {0};
    t0 = esp_cpu_get_cycle_count();
    dense_forward(&model->out, h, logits, model);
    t1 = esp_cpu_get_cycle_count(); t_dense += (t1 - t0);
    
    static int p_cnt = 0;
    if (p_cnt++ % 100 == 0) printf("Infer: dense=%d, fsmn=%d\n", (int)t_dense, (int)t_fsmn);
    
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

    // Free individually allocated tensors
    for (uint32_t i = 0; i < model->num_tensors; i++) {
#ifdef ESP_PLATFORM
        heap_caps_free(model->tensor_ptrs[i]);
#else
        free(model->tensor_ptrs[i]);
#endif
    }
    model->num_tensors = 0;

    esp_firevad_free_ptr(model->weight_buffer);
    memset(model, 0, sizeof(EspFirevadModel));
}

size_t esp_firevad_memory_usage(const EspFirevadModel* model) {
    if (model == nullptr) return 0;
    size_t total = model->weight_buffer_size;
    if (model->cache_len > 0) {
        total += model->arch.R * sizeof(float*);
        total += model->arch.R * model->arch.P * model->cache_len * sizeof(float);
    }
    uint32_t num_blocks = (model->arch.R > 1) ? model->arch.R - 1 : 0;
    total += num_blocks * (sizeof(DenseLayer) * 2 + sizeof(FsmnFilter));
    total += model->num_dnn_layers * sizeof(DenseLayer);
    total += (model->arch.H + model->arch.P * 3) * sizeof(float);
    
    uint32_t max_dim = (model->arch.H > model->arch.P) ? model->arch.H : model->arch.P;
    if (model->arch.D > max_dim) max_dim = model->arch.D;
    total += max_dim * sizeof(int8_t);
    
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

    float* feat_buf = (float*)esp_firevad_malloc(model->version, num_frames * D * sizeof(float));
    if (apply_cmvn_flag && model->cmvn_dim > 0) {
        for (uint32_t t = 0; t < num_frames; t++) {
            apply_cmvn(model->cmvn_means, model->cmvn_istd, features + t * D, feat_buf + t * D, D);
        }
    } else {
        memcpy(feat_buf, features, num_frames * D * sizeof(float));
    }

    float* h_buf = (float*)esp_firevad_malloc(model->version, num_frames * H * sizeof(float));
    float* p_buf = (float*)esp_firevad_malloc(model->version, num_frames * P * sizeof(float));
    float* p2_buf = (float*)esp_firevad_malloc(model->version, num_frames * P * sizeof(float));
    float* conv_out = (float*)esp_firevad_malloc(model->version, num_frames * P * sizeof(float));

    // 1st FSMN block
    for (uint32_t t = 0; t < num_frames; t++) {
        dense_forward(&model->fc1, feat_buf + t * D, h_buf + t * H, model);
        relu_inplace(h_buf + t * H, H);
        dense_forward(&model->fc2, h_buf + t * H, p_buf + t * P, model);
        relu_inplace(p_buf + t * P, P);
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
            } else if (model->version == 2) {
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
                } else if (model->version == 2) {
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
            dense_forward(&model->block_fc1[b], p_buf + t * P, h_buf + t * H, model);
            relu_inplace(h_buf + t * H, H);
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
            dense_forward(&model->dnn_layers[0], p_buf + t * P, h_buf + t * H, model);
            relu_inplace(h_buf + t * H, H);
        }
        for (uint32_t d = 1; d < model->num_dnn_layers; d++) {
            for (uint32_t t = 0; t < num_frames; t++) {
                float* temp = model->scratch_p; 
                dense_forward(&model->dnn_layers[d], h_buf + t * H, temp, model);
                relu_inplace(temp, H);
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

    esp_firevad_free_ptr(feat_buf);
    esp_firevad_free_ptr(h_buf);
    esp_firevad_free_ptr(p_buf);
    esp_firevad_free_ptr(p2_buf);
    esp_firevad_free_ptr(conv_out);
}

