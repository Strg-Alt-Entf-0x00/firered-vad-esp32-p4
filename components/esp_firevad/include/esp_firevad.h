#pragma once
/**
 * FireRedVAD Native C++ Inference Engine for ESP32-P4
 *
 * Loads a .EspFirevad binary (exported by export_weights.py) and runs
 * DFSMN inference directly using matrix-vector operations.
 * No TFLite, no ONNX, no ncnn dependency.
 *
 * Architecture: DFSMN (Deep Feedforward Sequential Memory Network)
 *   Input(D=80) -> FC1(H=256) -> FC2(P=128) -> FSMN1
 *   -> [DFSMNBlock x (R-1)] -> DNN(M layers) -> Output(odim=1) -> sigmoid
 *
 * Memory layout: All weights loaded into PSRAM as a single contiguous block.
 * Streaming cache: Ring buffer for FSMN lookback (N1 * P floats per block).
 */

#include <cstdint>
#include <cstddef>

/**
 * Architecture parameters extracted from .EspFirevad header.
 * These define the exact network topology.
 */
struct EspFirevadArchParams {
    uint32_t D;      // Input dimension (80 = mel bins)
    uint32_t H;      // Hidden size (256)
    uint32_t P;      // Projection size (128)
    uint32_t odim;   // Output dimension (1 for VAD)
    uint32_t R;      // Number of DFSMN blocks (8)
    uint32_t M;      // Number of DNN layers (1)
    uint32_t N1;     // Lookback order (20)
    uint32_t S1;     // Lookback stride (1)
    uint32_t N2;     // Lookahead order (0 for streaming)
    uint32_t S2;     // Lookahead stride
};

/**
 * Pointers into the weight buffer for a single Dense layer (weight + bias).
 */
struct DenseLayer {
    const void* weight;    // [out_dim, in_dim] row-major (float* or int8_t*)
    const void* bias;      // [out_dim] float32 (or nullptr if no bias)
    float weight_scale;    // For global scale (v2)
    float bias_scale;      // For global scale (v2, v3)
    const float* channel_scales; // For per-channel scale (v4), array of size out_dim
    uint32_t in_dim;
    uint32_t out_dim;
};

/**
 * Pointers into the weight buffer for a single FSMN memory filter.
 */
struct FsmnFilter {
    const void* lookback_weight;   // [P, 1, N1] depthwise conv kernel
    float lookback_scale;
    const void* lookahead_weight;  // [P, 1, N2] or nullptr
    float lookahead_scale;
};

/**
 * Complete model state: architecture + weight pointers + streaming cache.
 */
struct EspFirevadModel {
    uint32_t model_type; // 0=VAD, 1=Stream-VAD, 2=AED
    bool is_int8;
    bool is_int16;
    bool is_int8_per_ch;

    // Architecture config
    EspFirevadArchParams arch;
    int version;

    // CMVN normalization
    uint32_t cmvn_dim;
    const float* cmvn_means;
    const float* cmvn_istd;

    // Layer pointers (into weight_buffer)
    DenseLayer fc1;           // Input projection: D -> H
    DenseLayer fc2;           // Second projection: H -> P
    FsmnFilter fsmn1;         // First FSMN block filter

    // R-1 DFSMN blocks
    DenseLayer* block_fc1;    // Array[R-1]: P -> H
    DenseLayer* block_fc2;    // Array[R-1]: H -> P (no bias)
    FsmnFilter* block_fsmn;   // Array[R-1]: memory filters

    // DNN layers (after DFSMN blocks)
    DenseLayer* dnn_layers;   // Array[M]
    uint32_t num_dnn_layers;

    // Output layer
    DenseLayer out;           // H -> odim

    // Streaming cache: one ring buffer per FSMN block
    // Each cache is [P, lookback_padding] where lookback_padding = (N1-1)*S1
    float** fsmn_caches;      // Array[R] of cache buffers
    uint32_t* fsmn_cache_heads; // Array[R] of current write indices for ring buffer
    uint32_t cache_len;       // (N1-1)*S1

    // Raw weight buffer (allocated in PSRAM)
    uint8_t* weight_buffer;
    size_t weight_buffer_size;

    void** tensor_ptrs;
    uint32_t num_tensors;
    uint32_t max_tensors;

    // Scratch buffers for inference
    float* scratch_h;   // [H]
    float* scratch_p;   // [P]
    float* scratch_p2;  // [P]
    float* scratch_conv; // [P] for conv output
    int8_t* scratch_in_q; // [max(H, P)] for Int8 dynamic quant
    int16_t* scratch_in_q16; // [max(H, P)] for Int16 dynamic quant
};

// ---- Public API ----

/**
 * Load a .EspFirevad model file from a memory buffer.
 * The buffer contents are copied into PSRAM.
 *
 * @param data      Pointer to .EspFirevad file contents
 * @param data_len  Length of file in bytes
 * @param model     Output: populated model struct
 * @return 0 on success, negative error code on failure
 */
int esp_firevad_load(const uint8_t* data, size_t data_len, EspFirevadModel* model);

/**
 * Run inference on a single frame of 80-dim log-mel features.
 * This is the streaming API: it uses and updates internal caches.
 * NOTE: For models with N2 > 0 (VAD, AED), this API will IGNORE the lookahead
 * filter and only run the causal portion. Use esp_firevad_infer_chunk instead
 * for offline models.
 *
 * @param model       Loaded model
 * @param features    Input features [D] (80 floats, already fbank-extracted)
 * @param apply_cmvn  If true, apply CMVN normalization to features
 * @param out_probs   Output array of size `odim`.
 */
void esp_firevad_infer_frame(EspFirevadModel* model, const float* features, bool apply_cmvn, float* out_probs);

/**
 * Run inference on a full chunk of 80-dim log-mel features.
 * This computes the FSMN forward pass non-causally (including lookahead).
 * It expects all frames to be available at once.
 *
 * @param model       Loaded model
 * @param features    Input features [num_frames * D]
 * @param num_frames  Number of frames to process
 * @param apply_cmvn  If true, apply CMVN normalization to features
 * @param out_probs   Output probability array [num_frames * odim]
 */
void esp_firevad_infer_chunk(EspFirevadModel* model, const float* features, uint32_t num_frames, bool apply_cmvn, float* out_probs);

/**
 * Reset all streaming caches to zero.
 * Call this when starting a new audio stream.
 */
void esp_firevad_reset(EspFirevadModel* model);

/**
 * Free all allocated memory (weights, caches, scratch buffers).
 */
void esp_firevad_free(EspFirevadModel* model);

/**
 * Get total memory usage in bytes (weights + caches + scratch).
 */
size_t esp_firevad_memory_usage(const EspFirevadModel* model);
